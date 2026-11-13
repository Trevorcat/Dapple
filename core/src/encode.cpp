#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fstream>
#include <future>

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

// Rows per strip for a picture `W` wide: 512, or fewer (a multiple of 16) when the restart interval of a 512-row strip would not fit the 16-bit DRI
// field (more than 65535 MCUs: wider than ~8190 px at quality > 90, where stb does not subsample, ~16380 px below). Before section 42 such
// pictures fell back to one single-threaded encode; the streaming export cannot (it never has the whole picture), so both use the shorter strips.
int stripRowsFor(int W, int quality) {
  const bool sub = (quality ? quality : 90) <= 90;  // stb_image_write's rule (quality 0 = 90)
  const int m = sub ? 16 : 8;
  int rows = kStripRows;
  while (rows > 16 && ((W + m - 1) / m) * (rows / m) > 65535) rows -= 16;
  return rows;
}

// The strips (each an stb baseline JPEG of kStripRows rows, the last shorter) joined into one stream of width W and height H.
bool assembleStrips(const std::vector<std::vector<std::uint8_t>>& parts, int W, int H, int stripRows, std::vector<std::uint8_t>& out) {
  const int strips = int(parts.size());
  if (strips < 1) return false;
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
  const int interval = ((W + mw - 1) / mw) * (stripRows / mh);
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

bool encodeStrips(const Image8& image, int quality, std::vector<std::uint8_t>& out) {
  const int W = image.width, H = image.height, SR = stripRowsFor(W, quality);
  const int strips = (H + SR - 1) / SR;
  std::vector<std::vector<std::uint8_t>> parts(std::size_t(std::max(strips, 1)));
  std::vector<int> ok(parts.size(), 1);
  parallelFor(strips, [&](int i) {
    const int rows = std::min(SR, H - i * SR);
    ok[std::size_t(i)] = stbi_write_jpg_to_func(vecSink, &parts[std::size_t(i)], W, rows, 3, image.data.data() + std::size_t(i) * SR * W * 3, quality) != 0;
  });
  for (int v : ok)
    if (!v) return false;
  return assembleStrips(parts, W, H, SR, out);
}

bool stripsWanted(int W, int H) {
  return H >= 2 * kStripRows && std::size_t(W) * std::size_t(H) >= 2000000 && !std::getenv("DAPPLE_JPEG_THREADS_OFF");
}

// The one encoder behind encodeJpeg and writeJpeg (the Ultra HDR base image must stay byte-identical to the SDR export).
bool encodeAll(const Image8& image, int quality, std::vector<std::uint8_t>& out) {
  const bool big = stripsWanted(image.width, image.height);
  if (big && encodeStrips(image, quality, out)) return true;
  out.clear();
  return stbi_write_jpg_to_func(vecSink, &out, image.width, image.height, 3, image.data.data(), quality) != 0;
}

// Metadata segments around stb's JFIF header: SOI, Exif (APP1), JFIF (APP0), ICC (APP2), then the tables and the scan. Done on the finished stream, so
// the threaded strip path and the single encode get the same treatment.
void addSegments(const JpegOptions& o, std::vector<std::uint8_t>& s) {
  if (o.exifSegment.empty() && o.iccSegment.empty()) return;
  const bool jfif = s.size() >= 20 && s[2] == 0xFF && s[3] == 0xE0 && s[4] == 0x00 && s[5] == 0x10 && std::memcmp(&s[6], "JFIF", 4) == 0;
  const std::size_t afterApp0 = jfif ? 20 : 2;
  std::vector<std::uint8_t> r;
  r.reserve(s.size() + o.exifSegment.size() + o.iccSegment.size());
  r.insert(r.end(), s.begin(), s.begin() + 2);
  r.insert(r.end(), o.exifSegment.begin(), o.exifSegment.end());
  r.insert(r.end(), s.begin() + 2, s.begin() + std::ptrdiff_t(afterApp0));
  r.insert(r.end(), o.iccSegment.begin(), o.iccSegment.end());
  r.insert(r.end(), s.begin() + std::ptrdiff_t(afterApp0), s.end());
  s.swap(r);
}

bool encodeWith(const Image8& image, const JpegOptions& o, std::vector<std::uint8_t>& out) {
  if (!encodeAll(image, o.quality, out)) return false;
  addSegments(o, out);
  return true;
}
}  // namespace

Result<std::vector<std::uint8_t>> encodeJpeg(const Image8& image, const JpegOptions& options) {
  if (image.width <= 0 || image.height <= 0) return fail("empty image");
  std::vector<std::uint8_t> bytes;
  if (!encodeWith(image, options, bytes)) return fail("JPEG encode failed");
  return bytes;
}

Result<void> writeJpegBytes(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& path) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return fail("Cannot write " + pathToUtf8(path));
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      return fail("Write failed: " + pathToUtf8(path));
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

// ---- streaming encode (section 42) ------------------------------------------------------------------------------------------------------
struct JpegStripEncoder::Impl {
  int W = 0, H = 0;
  JpegOptions o;
  bool strips = false;
  int SR = kStripRows;
  std::vector<std::uint8_t> buf;                 // the strip being filled (or the whole picture when it is not stripped)
  int filled = 0, done = 0;                      // rows in buf, rows handed over so far
  std::vector<std::vector<std::uint8_t>> parts;  // encoded strips (reserved up front: the worker writes into its element)
  std::future<bool> pending;                     // the previous strip, encoding on a worker while the next band renders
  std::vector<std::uint8_t> pendingBuf;
  bool failed = false;
  bool wait() {
    if (pending.valid() && !pending.get()) failed = true;
    return !failed;
  }
  bool encodeStrip() {  // the filled rows of buf -> parts[i], on a worker
    if (!wait()) return false;
    const std::size_t i = parts.size();
    parts.emplace_back();
    pendingBuf.swap(buf);
    if (buf.size() != pendingBuf.size()) buf.assign(pendingBuf.size(), 0);
    const int rows = filled;
    filled = 0;
    std::vector<std::uint8_t>* dst = &parts[i];
    const std::uint8_t* src = pendingBuf.data();
    pending = std::async(std::launch::async, [this, dst, src, rows] { return stbi_write_jpg_to_func(vecSink, dst, W, rows, 3, src, o.quality) != 0; });
    return true;
  }
};

JpegStripEncoder::JpegStripEncoder(int width, int height, const JpegOptions& options) : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  m.W = width, m.H = height, m.o = options;
  m.strips = width > 0 && height > 0 && stripsWanted(width, height);
  m.SR = stripRowsFor(width, options.quality);
  m.parts.reserve(std::size_t(std::max(1, (height + m.SR - 1) / m.SR)));
  m.buf.assign(std::size_t(m.strips ? std::min(m.SR, height) : std::max(height, 0)) * std::size_t(std::max(width, 0)) * 3, 0);
}
JpegStripEncoder::~JpegStripEncoder() {
  if (impl_ && impl_->pending.valid()) impl_->pending.wait();
}
bool JpegStripEncoder::stripped() const { return impl_->strips; }
std::size_t JpegStripEncoder::bufferBytes() const { return impl_->buf.capacity() + impl_->pendingBuf.capacity(); }

bool JpegStripEncoder::push(const std::uint8_t* rgb, int rows, std::size_t stride) {
  Impl& m = *impl_;
  if (m.failed || rows < 0 || m.done + rows > m.H) return false;
  const std::size_t rowBytes = std::size_t(m.W) * 3;
  if (stride == 0) stride = rowBytes;
  for (int r = 0; r < rows; ++r) {
    std::memcpy(m.buf.data() + std::size_t(m.filled) * rowBytes, rgb + std::size_t(r) * stride, rowBytes);
    ++m.filled, ++m.done;
    if (m.strips && (m.filled == m.SR || m.done == m.H) && !m.encodeStrip()) return false;
  }
  return true;
}

Result<std::vector<std::uint8_t>> JpegStripEncoder::finish() {
  Impl& m = *impl_;
  if (m.done != m.H || m.W <= 0 || m.H <= 0) return fail("JPEG stream: " + std::to_string(m.done) + " of " + std::to_string(m.H) + " rows");
  std::vector<std::uint8_t> out;
  if (m.strips) {
    if (!m.wait() || !assembleStrips(m.parts, m.W, m.H, m.SR, out)) return fail("JPEG encode failed");
  } else if (stbi_write_jpg_to_func(vecSink, &out, m.W, m.H, 3, m.buf.data(), m.o.quality) == 0) {
    return fail("JPEG encode failed");
  }
  std::vector<std::uint8_t>().swap(m.buf);
  std::vector<std::uint8_t>().swap(m.pendingBuf);
  std::vector<std::vector<std::uint8_t>>().swap(m.parts);
  addSegments(m.o, out);
  return out;
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
    const bool ok = encodeWith(image, options, bytes);
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
