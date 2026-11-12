// Archival writers for merged results: 16-bit linear TIFF and linear DNG.
//
// Both are classic little-endian TIFF containers with one uncompressed
// interleaved RGB strip. Values are stored as v / headroom * 65535 where the
// headroom is a power of two >= the largest value, so scene values above the
// clipping point (HDR) survive. The DNG declares ProPhoto D50 as its
// "camera" colour space (ColorMatrix1 = XYZ D50 -> ProPhoto, AsShotNeutral =
// 1,1,1) and carries BaselineExposure = log2(headroom).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "color.h"
#include "dapple/merge.h"
#include "file_util.h"
#include "parallel.h"

namespace dapple {

namespace {

struct Entry {
  std::uint16_t tag, type;
  std::uint32_t count;
  std::vector<std::uint8_t> data;  // already little-endian
};

void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(std::uint8_t(x));
  v.push_back(std::uint8_t(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
}

Entry shorts(std::uint16_t tag, std::vector<std::uint16_t> vals) {
  Entry e{tag, 3, std::uint32_t(vals.size()), {}};
  for (auto v : vals) put16(e.data, v);
  return e;
}
Entry longs(std::uint16_t tag, std::vector<std::uint32_t> vals) {
  Entry e{tag, 4, std::uint32_t(vals.size()), {}};
  for (auto v : vals) put32(e.data, v);
  return e;
}
Entry bytes(std::uint16_t tag, std::vector<std::uint8_t> vals) {
  return Entry{tag, 1, std::uint32_t(vals.size()), std::move(vals)};
}
Entry ascii(std::uint16_t tag, const std::string& s) {
  Entry e{tag, 2, std::uint32_t(s.size() + 1), {}};
  e.data.assign(s.begin(), s.end());
  e.data.push_back(0);
  return e;
}
Entry srationals(std::uint16_t tag, const std::vector<double>& vals, std::int32_t den = 10000) {
  Entry e{tag, 10, std::uint32_t(vals.size()), {}};
  for (double v : vals) {
    put32(e.data, std::uint32_t(std::int32_t(std::lround(v * den))));
    put32(e.data, std::uint32_t(den));
  }
  return e;
}
Entry rationals(std::uint16_t tag, const std::vector<double>& vals, std::uint32_t den = 10000) {
  Entry e{tag, 5, std::uint32_t(vals.size()), {}};
  for (double v : vals) {
    put32(e.data, std::uint32_t(std::lround(v * den)));
    put32(e.data, den);
  }
  return e;
}

float autoHeadroom(const ImageF& im) {
  float mx = 0;
  for (float v : im.data) mx = std::max(mx, v);
  float h = 1.f;
  while (h < mx && h < 1024.f) h *= 2.f;
  return h;
}

Result<void> writeTiffLike(const ImageF& image, const std::filesystem::path& path, float headroom, bool dng,
                           const std::string& make, const std::string& model) {
  if (image.empty()) return fail("Empty image");
  {
    float mx = 0.f;
    for (float v : image.data) {
      if (!std::isfinite(v)) return fail("Image has non-finite pixels; refusing to write");
      mx = std::max(mx, v);
    }
    if (mx <= 0.f) return fail("Image is entirely black; refusing to write");
  }
  if (headroom <= 0.f) headroom = autoHeadroom(image);
  const std::uint64_t pixelBytes = std::uint64_t(image.width) * image.height * 6;
  if (pixelBytes > 0xF0000000ull) return fail("Image too large for a classic TIFF/DNG (4 GB limit)");

  std::vector<Entry> es;
  es.push_back(longs(254, {0}));
  es.push_back(longs(256, {std::uint32_t(image.width)}));
  es.push_back(longs(257, {std::uint32_t(image.height)}));
  es.push_back(shorts(258, {16, 16, 16}));
  es.push_back(shorts(259, {1}));
  es.push_back(shorts(262, {std::uint16_t(dng ? 34892 : 2)}));
  char desc[96];
  std::snprintf(desc, sizeof desc, "Dapple linear ProPhoto RGB, stored = value / %g * 65535", double(headroom));
  es.push_back(ascii(270, desc));
  es.push_back(longs(273, {0}));  // patched below
  es.push_back(shorts(277, {3}));
  es.push_back(longs(278, {std::uint32_t(image.height)}));
  es.push_back(longs(279, {std::uint32_t(pixelBytes)}));
  es.push_back(shorts(284, {1}));
  es.push_back(ascii(305, "Dapple"));
  if (dng) {
    es.push_back(bytes(50706, {1, 4, 0, 0}));
    es.push_back(bytes(50707, {1, 1, 0, 0}));
    es.push_back(ascii(271, make.empty() ? "Dapple" : make));
    es.push_back(ascii(272, model.empty() ? "Linear Merge" : model));
    es.push_back(ascii(50708, "Dapple Linear Merge"));  // marker the decoder looks for
    const auto& m = color::kXyzD50ToProPhoto.m;
    es.push_back(srationals(50721, {m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]}, 1000000));
    es.push_back(longs(50717, {65535}));
    es.push_back(shorts(50778, {23}));  // CalibrationIlluminant1 = D50
    es.push_back(rationals(50728, {1.0, 1.0, 1.0}));
    es.push_back(srationals(50730, {std::log2(double(headroom))}, 10000));
  }
  std::sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

  // layout
  const std::uint32_t ifdOffset = 8;
  std::uint32_t extra = ifdOffset + 2 + 12 * std::uint32_t(es.size()) + 4;
  std::vector<std::uint32_t> dataOffset(es.size(), 0);
  for (std::size_t i = 0; i < es.size(); ++i)
    if (es[i].data.size() > 4) {
      if (extra & 1) ++extra;
      dataOffset[i] = extra;
      extra += std::uint32_t(es[i].data.size());
    }
  if (extra & 1) ++extra;
  const std::uint32_t pixelOffset = extra;
  for (auto& e : es)
    if (e.tag == 273) {
      e.data.clear();
      put32(e.data, pixelOffset);
    }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return fail("Cannot write " + pathToUtf8(path));
  std::vector<std::uint8_t> head;
  head.push_back('I');
  head.push_back('I');
  put16(head, 42);
  put32(head, ifdOffset);
  put16(head, std::uint16_t(es.size()));
  for (std::size_t i = 0; i < es.size(); ++i) {
    put16(head, es[i].tag);
    put16(head, es[i].type);
    put32(head, es[i].count);
    if (es[i].data.size() <= 4) {
      std::vector<std::uint8_t> d = es[i].data;
      d.resize(4, 0);
      head.insert(head.end(), d.begin(), d.end());
    } else {
      put32(head, dataOffset[i]);
    }
  }
  put32(head, 0);  // no next IFD
  for (std::size_t i = 0; i < es.size(); ++i)
    if (es[i].data.size() > 4) {
      head.resize(dataOffset[i], 0);
      head.insert(head.end(), es[i].data.begin(), es[i].data.end());
    }
  head.resize(pixelOffset, 0);
  out.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));

  const float scale = 65535.f / headroom;
  std::vector<std::uint16_t> rowBuf(std::size_t(image.width) * 3);
  // chunked conversion to keep memory flat
  const int chunkRows = 64;
  std::vector<std::uint16_t> chunk(std::size_t(chunkRows) * image.width * 3);
  for (int y0 = 0; y0 < image.height; y0 += chunkRows) {
    const int rows = std::min(chunkRows, image.height - y0);
    parallelRows(rows, [&](int r) {
      const float* s = image.row(y0 + r);
      std::uint16_t* d = chunk.data() + std::size_t(r) * image.width * 3;
      for (int i = 0; i < image.width * 3; ++i) d[i] = std::uint16_t(std::clamp(s[i] * scale + 0.5f, 0.f, 65535.f));
    });
    out.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(std::size_t(rows) * image.width * 6));
  }
  out.flush();
  if (!out) return fail("Write failed: " + pathToUtf8(path));
  return {};
}

}  // namespace

Result<void> writeLinearTiff16(const ImageF& image, const std::filesystem::path& path, float headroom) {
  try {
    return writeTiffLike(image, path, headroom, false, "", "");
  } catch (const std::exception& e) {
    return fail(std::string("TIFF write failed: ") + e.what());
  }
}

Result<void> writeLinearDng(const SourceImage& image, const std::filesystem::path& path, float headroom) {
  try {
    return writeTiffLike(image.linear, path, headroom, true, image.meta.make, image.meta.model);
  } catch (const std::exception& e) {
    return fail(std::string("DNG write failed: ") + e.what());
  }
}

}  // namespace dapple
