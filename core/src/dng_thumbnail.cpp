#include "dng_thumbnail.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dapple {
namespace {

struct Buf {
  std::vector<std::uint8_t> b;
  bool le = true;
  unsigned r16(std::size_t o) const { return o + 2 <= b.size() ? (le ? unsigned(b[o] | b[o + 1] << 8) : unsigned(b[o + 1] | b[o] << 8)) : 0u; }
  std::uint32_t r32(std::size_t o) const {
    if (o + 4 > b.size()) return 0;
    return le ? (std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8 | std::uint32_t(b[o + 2]) << 16 | std::uint32_t(b[o + 3]) << 24)
              : (std::uint32_t(b[o + 3]) | std::uint32_t(b[o + 2]) << 8 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o]) << 24);
  }
  void w16(std::vector<std::uint8_t>& v, unsigned x) const {
    if (le) v.push_back(std::uint8_t(x)), v.push_back(std::uint8_t(x >> 8));
    else v.push_back(std::uint8_t(x >> 8)), v.push_back(std::uint8_t(x));
  }
  void w32(std::vector<std::uint8_t>& v, std::uint32_t x) const {
    if (le) for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
    else for (int i = 3; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i)));
  }
};

}  // namespace

bool embedDngThumbnail(const std::filesystem::path& dng, const std::vector<std::uint8_t>& jpeg, int width, int height, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || width <= 0 || height <= 0) return fail("not a JPEG");
  Buf f;
  {
    std::ifstream in(dng, std::ios::binary | std::ios::ate);
    if (!in) return fail("cannot open the DNG");
    const std::streamsize n = in.tellg();
    if (n < 16 || n > (std::streamsize(1) << 30)) return fail("unreasonable file size");
    f.b.resize(std::size_t(n));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(f.b.data()), n)) return fail("read failed");
  }
  if (f.b[0] == 'I' && f.b[1] == 'I') f.le = true;
  else if (f.b[0] == 'M' && f.b[1] == 'M') f.le = false;
  else return fail("not a TIFF");
  if (f.r16(2) != 42) return fail("not a TIFF (BigTIFF is not handled)");
  const std::size_t ifd0 = f.r32(4);
  const unsigned n = f.r16(ifd0);
  if (ifd0 + 2 + std::size_t(12) * n + 4 > f.b.size() || n == 0 || n > 512) return fail("bad IFD0");
  for (unsigned i = 0; i < n; ++i)
    if (f.r16(ifd0 + 2 + std::size_t(12) * i) == 330) return fail("the file already has SubIFDs");
  const std::uint32_t nextIfd = f.r32(ifd0 + 2 + std::size_t(12) * n);

  std::vector<std::uint8_t>& out = f.b;
  if (out.size() & 1) out.push_back(0);
  // 1. the JPEG
  const std::uint32_t jpegOff = std::uint32_t(out.size());
  out.insert(out.end(), jpeg.begin(), jpeg.end());
  if (out.size() & 1) out.push_back(0);
  // 2. BitsPerSample [8, 8, 8] of the SubIFD (out of line)
  const std::uint32_t bpsOff = std::uint32_t(out.size());
  for (int i = 0; i < 3; ++i) f.w16(out, 8);
  // 3. the SubIFD
  const std::uint32_t subOff = std::uint32_t(out.size());
  struct E { unsigned tag, type; std::uint32_t count, value; };
  const std::vector<E> sub = {
      {254, 4, 1, 1}, {256, 4, 1, std::uint32_t(width)}, {257, 4, 1, std::uint32_t(height)}, {258, 3, 3, bpsOff}, {259, 3, 1, 7}, {262, 3, 1, 6},
      {273, 4, 1, jpegOff}, {277, 3, 1, 3}, {278, 4, 1, std::uint32_t(height)}, {279, 4, 1, std::uint32_t(jpeg.size())},
  };
  f.w16(out, unsigned(sub.size()));
  for (const E& e : sub) {
    f.w16(out, e.tag), f.w16(out, e.type), f.w32(out, e.count);
    if (e.type == 3 && e.count == 1) f.w16(out, e.value), f.w16(out, 0);  // a SHORT sits left-justified in the 4 value bytes
    else f.w32(out, e.value);
  }
  f.w32(out, 0);
  // 4. the SubIFDs offset array (one entry, stored in the entry itself) and the copy of IFD0 with the extra entry, sorted by tag
  struct Raw { unsigned tag; std::uint8_t bytes[12]; };
  std::vector<Raw> entries;
  for (unsigned i = 0; i < n; ++i) {
    Raw r{};
    std::memcpy(r.bytes, &f.b[ifd0 + 2 + std::size_t(12) * i], 12);
    r.tag = f.r16(ifd0 + 2 + std::size_t(12) * i);
    entries.push_back(r);
  }
  {
    Raw r{};
    r.tag = 330;
    std::vector<std::uint8_t> e;
    f.w16(e, 330), f.w16(e, 4), f.w32(e, 1), f.w32(e, subOff);
    std::memcpy(r.bytes, e.data(), 12);
    entries.push_back(r);
  }
  std::stable_sort(entries.begin(), entries.end(), [](const Raw& a, const Raw& b) { return a.tag < b.tag; });
  if (out.size() & 1) out.push_back(0);
  const std::uint32_t newIfd = std::uint32_t(out.size());
  f.w16(out, unsigned(entries.size()));
  for (const Raw& r : entries) out.insert(out.end(), r.bytes, r.bytes + 12);
  f.w32(out, nextIfd);
  // 5. the header points at the new IFD0
  std::vector<std::uint8_t> p;
  f.w32(p, newIfd);
  std::memcpy(&out[4], p.data(), 4);

  const std::filesystem::path tmp = dng.string() + ".thumbtmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o || !o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) return fail("write failed");
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dng, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return fail("replace failed");
  }
  return true;
}

bool embedDngThumbnailFile(const std::filesystem::path& dng, const std::filesystem::path& jpegFile, std::string* error) {
  std::ifstream f(jpegFile, std::ios::binary);
  std::vector<std::uint8_t> jpeg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int w = 0, h = 0;
  for (std::size_t i = 2; i + 9 < jpeg.size();) {  // walk the marker segments up to the first SOFn
    if (jpeg[i] != 0xFF) {
      ++i;
      continue;
    }
    const std::uint8_t m = jpeg[i + 1];
    if (m == 0xFF) {
      ++i;
      continue;
    }
    const std::size_t len = (std::size_t(jpeg[i + 2]) << 8) | jpeg[i + 3];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      h = (jpeg[i + 5] << 8) | jpeg[i + 6];
      w = (jpeg[i + 7] << 8) | jpeg[i + 8];
      break;
    }
    i += 2 + len;
  }
  if (w <= 0 || h <= 0) {
    if (error) *error = "the thumbnail is not a decodable JPEG";
    return false;
  }
  return embedDngThumbnail(dng, jpeg, w, h, error);
}

}  // namespace dapple
