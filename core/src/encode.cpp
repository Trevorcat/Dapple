#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fstream>

#include "dapple/encode.h"
#include "file_util.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <cstring>

#include "parallel.h"

namespace dapple {

namespace {
void vecSink(void* ctx, void* data, int size) {
  auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
  v->insert(v->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
}

// Baseline JPEG of the whole image, encoded as horizontal strips in parallel. Every strip is an ordinary stb baseline JPEG of the same width (so the same
// quantisation and Huffman tables, the same colour conversion and DCT), 512 rows high (a multiple of the 16-row MCU, so no strip needs padding except the
// last, which stb pads with its own last row exactly as a single encode would). The strips' entropy-coded segments start with DC prediction at zero and end
// byte-aligned, which is what a restart interval asks for: the result is the first strip's header with the real height, a DRI marker, then the segments
// separated by RST0..RST7. A decoder sees an ordinary baseline JPEG whose pixels equal those of the single-threaded encode (restarts change only the
// bytes, DC differences are lossless). The strip height is fixed, so the output does not depend on the thread count.
constexpr int kStripRows = 512;

bool encodeStrips(const Image8& image, int quality, std::vector<std::uint8_t>& out) {
  const int W = image.width, H = image.height;
  const int strips = (H + kStripRows - 1) / kStripRows;
  std::vector<std::vector<std::uint8_t>> parts(std::size_t(std::max(strips, 1)));
  std::vector<int> ok(parts.size(), 1);
  parallelFor(strips, [&](int i) {
    const int rows = std::min(kStripRows, H - i * kStripRows);
    ok[std::size_t(i)] = stbi_write_jpg_to_func(vecSink, &parts[std::size_t(i)], W, rows, 3, image.data.data() + std::size_t(i) * kStripRows * W * 3, quality) != 0;
  });
  for (int v : ok)
    if (!v) return false;
  // position of the SOS header (FF DA 00 0C, 14 bytes) and of the SOF0 height in the first strip
  auto find = [](const std::vector<std::uint8_t>& b, const std::uint8_t* pat, std::size_t len, std::size_t from) -> std::size_t {
    for (std::size_t i = from; i + len <= b.size(); ++i)
      if (std::memcmp(&b[i], pat, len) == 0) return i;
    return std::string::npos;
  };
  static const std::uint8_t sos[4] = {0xFF, 0xDA, 0x00, 0x0C}, sof[5] = {0xFF, 0xC0, 0x00, 0x11, 0x08};
  const std::size_t sosAt = find(parts[0], sos, 4, 0), sofAt = find(parts[0], sof, 5, 0);
  if (sosAt == std::string::npos || sofAt == std::string::npos || sosAt + 14 > parts[0].size()) return false;
  const int mcuW = 16, mcuH = 16;  // the strips are encoded at the quality's own sampling; 16 x 16 covers both (an 8 x 8 MCU divides it)
  const bool sub = parts[0][sofAt + 11] == 0x22;  // component 1 sampling factors
  const int mw = sub ? mcuW : 8, mh = sub ? mcuH : 8;
  const int interval = ((W + mw - 1) / mw) * (kStripRows / mh);
  if (interval > 65535) return false;
  out.clear();
  out.reserve(parts[0].size() * std::size_t(strips));
  out.insert(out.end(), parts[0].begin(), parts[0].begin() + std::ptrdiff_t(sosAt));
  out[sofAt + 5] = std::uint8_t(H >> 8), out[sofAt + 6] = std::uint8_t(H & 255);
  const std::uint8_t dri[6] = {0xFF, 0xDD, 0x00, 0x04, std::uint8_t(interval >> 8), std::uint8_t(interval & 255)};
  out.insert(out.end(), dri, dri + 6);
  out.insert(out.end(), parts[0].begin() + std::ptrdiff_t(sosAt), parts[0].begin() + std::ptrdiff_t(sosAt) + 14);
  for (int i = 0; i < strips; ++i) {
    const auto& b = parts[std::size_t(i)];
    std::size_t start = find(b, sos, 4, 0);
    if (start == std::string::npos || b.size() < start + 16 || b[b.size() - 2] != 0xFF || b[b.size() - 1] != 0xD9) return false;
    start += 14;
    out.insert(out.end(), b.begin() + std::ptrdiff_t(start), b.end() - 2);
    if (i + 1 < strips) {
      out.push_back(0xFF);
      out.push_back(std::uint8_t(0xD0 + (i & 7)));
    }
  }
  out.push_back(0xFF);
  out.push_back(0xD9);
  return true;
}

// The one encoder behind encodeJpeg and writeJpeg (the Ultra HDR base image must stay byte-identical to the SDR export).
bool encodeAll(const Image8& image, int quality, std::vector<std::uint8_t>& out) {
  const bool big = image.height >= 2 * kStripRows && std::size_t(image.width) * std::size_t(image.height) >= 2000000 && !std::getenv("DAPPLE_JPEG_THREADS_OFF");
  if (big && encodeStrips(image, quality, out)) return true;
  out.clear();
  return stbi_write_jpg_to_func(vecSink, &out, image.width, image.height, 3, image.data.data(), quality) != 0;
}
}  // namespace

Result<std::vector<std::uint8_t>> encodeJpeg(const Image8& image, const JpegOptions& options) {
  if (image.width <= 0 || image.height <= 0) return fail("empty image");
  std::vector<std::uint8_t> bytes;
  if (!encodeAll(image, options.quality, bytes)) return fail("JPEG encode failed");
  return bytes;
}

Result<void> writeJpeg(const Image8& image, const std::filesystem::path& path, const JpegOptions& options) {
  if (image.width <= 0 || image.height <= 0) return fail("empty image");
  // Write to a temp file and rename, so a failed or interrupted write never
  // leaves a truncated JPEG at the final path.
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return fail("Cannot write " + pathToUtf8(path));
    std::vector<std::uint8_t> bytes;
    const bool ok = encodeAll(image, options.quality, bytes);
    if (ok) out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!ok || !out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      return fail(ok ? "Write failed: " + pathToUtf8(path) : "JPEG encode failed");
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::error_code ec2;
    std::filesystem::rename(tmp, path, ec2);
    if (ec2) {
      std::filesystem::remove(tmp, ec);
      return fail("Cannot write " + pathToUtf8(path));
    }
  }
  return {};
}

}  // namespace dapple
