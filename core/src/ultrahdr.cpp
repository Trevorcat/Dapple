// Ultra HDR (gain-map JPEG) writer / reader, from the public specifications (Ultra HDR v1 gain-map JPEG format with the hdrgm namespace,
// CIPA DC-007 Multi-Picture Format, ISO 21496-1). See dapple/ultrahdr.h and docs/engine/pipeline-architecture.md section 34.

#include "dapple/ultrahdr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dapple::uhdr {

namespace {
constexpr float kLw[3] = {0.2126f, 0.7152f, 0.0722f};  // Rec.709 / sRGB luminance
inline float luma(const float* c) { return kLw[0] * c[0] + kLw[1] * c[1] + kLw[2] * c[2]; }
inline float smooth01(float t) {
  t = std::min(1.f, std::max(0.f, t));
  return t * t * (3.f - 2.f * t);
}
// 8-bit sRGB -> linear lookup
const float* lut8() {
  static float t[256];
  static bool init = [] {
    for (int i = 0; i < 256; ++i) t[i] = srgbToLinear(float(i) / 255.f);
    return true;
  }();
  (void)init;
  return t;
}
}  // namespace

float srgbToLinear(float e) { return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f; }

float brightPeak(const Image8& bright, float H) {
  const float* t = lut8();
  const std::size_t n = std::size_t(bright.width) * bright.height;
  if (!n) return 0.f;
  std::vector<float> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float c[3] = {t[bright.data[3 * i]], t[bright.data[3 * i + 1]], t[bright.data[3 * i + 2]]};
    y[i] = luma(c);
  }
  const std::size_t k = std::min(n - 1, std::size_t(double(n) * 0.999));
  std::nth_element(y.begin(), y.begin() + std::ptrdiff_t(k), y.end());
  return y[k] * std::exp2(H);
}

float autoHeadroom(float peak) {
  if (!(peak >= kWorthPeak)) return 0.f;
  const float h = std::ceil(2.f * std::log2(peak)) / 2.f;
  return std::min(kMaxHeadroomStops, std::max(0.5f, h));
}

ImageF hdrLinear(const Image8& sdr, const Image8& bright, float H) {
  ImageF out(sdr.width, sdr.height);
  const float* t = lut8();
  const float g = std::exp2(H);
  const std::size_t n = std::size_t(sdr.width) * sdr.height;
  for (std::size_t i = 0; i < n; ++i) {
    const float b[3] = {t[bright.data[3 * i]] * g, t[bright.data[3 * i + 1]] * g, t[bright.data[3 * i + 2]] * g};
    const float yb = luma(b);
    const float c = std::max(0.f, yb - 1.f);
    const float e = c > 0.f ? c * smooth01(c) / yb : 0.f;  // excess per unit of the bright colour
    for (int k = 0; k < 3; ++k) out.data[3 * i + k] = t[sdr.data[3 * i + k]] + e * b[k];
  }
  return out;
}

GainMap makeGainMap(const Image8& sdr, const Image8& bright, float H) {
  GainMap gm;
  gm.width = sdr.width;
  gm.height = sdr.height;
  const ImageF hdr = hdrLinear(sdr, bright, H);
  const float* t = lut8();
  const std::size_t n = std::size_t(gm.width) * gm.height;
  std::vector<float> g(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float s[3] = {t[sdr.data[3 * i]], t[sdr.data[3 * i + 1]], t[sdr.data[3 * i + 2]]};
    const float ys = luma(s), yh = luma(&hdr.data[3 * i]);
    g[i] = std::max(0.f, std::log2((yh + kOffset) / (ys + kOffset)));
  }
  // Edge guard: where a neighbour is much darker in the SDR picture (below 0.6 of this pixel) the gain is limited to the neighbour's, but never below half
  // of its own (stops), so the up-scaled map lights the dark side of an edge by at most a quarter of the highlight's gain while a thin highlight (a glint, sky
  // through leaves) keeps half of its gain; smooth gradients are left alone (a plain 3x3 minimum costs 0.3 stop on a sky gradient).
  std::vector<float> ys(n);
  for (std::size_t i = 0; i < n; ++i) {
    const float s[3] = {t[sdr.data[3 * i]], t[sdr.data[3 * i + 1]], t[sdr.data[3 * i + 2]]};
    ys[i] = luma(s);
  }
  std::vector<float> m(n);
  for (int y = 0; y < gm.height; ++y)
    for (int x = 0; x < gm.width; ++x) {
      const std::size_t i = std::size_t(y) * gm.width + x;
      float v = g[i];
      if (v > 0.f)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = std::min(gm.width - 1, std::max(0, x + dx)), yy = std::min(gm.height - 1, std::max(0, y + dy));
            const std::size_t j = std::size_t(yy) * gm.width + xx;
            if (ys[j] < 0.6f * ys[i]) v = std::min(v, g[j]);
          }
      m[i] = std::max(v, 0.5f * g[i]);
    }
  float mx = 0.f;
  for (float v : m) mx = std::max(mx, v);
  gm.maxGain = mx;
  const float gmax = mx < 0.005f ? 0.f : std::ceil(mx * 1000.f) / 1000.f;
  gm.meta.gainMin = 0.f;
  gm.meta.gainMax = gmax;
  gm.meta.capacityMin = 0.f;
  gm.meta.capacityMax = gmax;
  gm.data.assign(n, 0);
  if (gmax > 0.f)
    for (std::size_t i = 0; i < n; ++i) gm.data[i] = std::uint8_t(std::lround(std::min(1.f, m[i] / gmax) * 255.f));
  return gm;
}

ImageF reconstruct(const Image8& sdr, const GainMap& gm) {
  ImageF out(sdr.width, sdr.height);
  const float* t = lut8();
  const Meta& md = gm.meta;
  for (int y = 0; y < sdr.height; ++y) {
    const float fy = (float(y) + 0.5f) * gm.height / sdr.height - 0.5f;
    const int y0 = std::min(gm.height - 1, std::max(0, int(std::floor(fy)))), y1 = std::min(gm.height - 1, y0 + 1);
    const float ty = std::min(1.f, std::max(0.f, fy - float(y0)));
    for (int x = 0; x < sdr.width; ++x) {
      const float fx = (float(x) + 0.5f) * gm.width / sdr.width - 0.5f;
      const int x0 = std::min(gm.width - 1, std::max(0, int(std::floor(fx)))), x1 = std::min(gm.width - 1, x0 + 1);
      const float tx = std::min(1.f, std::max(0.f, fx - float(x0)));
      const auto G = [&](int xx, int yy) { return float(gm.data[std::size_t(yy) * gm.width + xx]) / 255.f; };
      const float v = (G(x0, y0) * (1 - tx) + G(x1, y0) * tx) * (1 - ty) + (G(x0, y1) * (1 - tx) + G(x1, y1) * tx) * ty;
      const float vv = md.gamma == 1.f ? v : std::pow(v, 1.f / md.gamma);
      const float boost = md.gainMin + (md.gainMax - md.gainMin) * vv;
      const float k = std::exp2(boost);
      const std::size_t i = std::size_t(y) * sdr.width + x;
      for (int c = 0; c < 3; ++c) out.data[3 * i + c] = std::max(0.f, (t[sdr.data[3 * i + c]] + md.offsetSdr) * k - md.offsetHdr);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Grayscale baseline JPEG
namespace {
const unsigned char kDcBits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const unsigned char kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char kAcBits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const unsigned char kAcVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const int kQ50[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87,
                      80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const int kZig[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Huff {
  unsigned code[256]{};
  unsigned len[256]{};
};
Huff makeHuff(const unsigned char* bits, const unsigned char* vals) {
  Huff h;
  unsigned code = 0;
  int k = 0;
  for (int l = 1; l <= 16; ++l) {
    for (int i = 0; i < bits[l]; ++i, ++k) {
      h.code[vals[k]] = code++;
      h.len[vals[k]] = unsigned(l);
    }
    code <<= 1;
  }
  return h;
}

struct BitOut {
  Bytes& out;
  unsigned acc = 0;
  int n = 0;
  void put(unsigned bits, int len) {
    for (int i = len - 1; i >= 0; --i) {
      acc = (acc << 1) | ((bits >> i) & 1u);
      if (++n == 8) {
        out.push_back(std::uint8_t(acc));
        if ((acc & 0xFF) == 0xFF) out.push_back(0);
        acc = 0;
        n = 0;
      }
    }
  }
  void flush() {
    while (n != 0) put(1, 1);
  }
};

void put16(Bytes& b, unsigned v) {
  b.push_back(std::uint8_t(v >> 8));
  b.push_back(std::uint8_t(v));
}
void put32(Bytes& b, std::uint32_t v) {
  put16(b, v >> 16);
  put16(b, v & 0xFFFF);
}
}  // namespace

Bytes encodeGray(const std::uint8_t* data, int w, int h, int quality) {
  quality = std::min(100, std::max(1, quality));
  const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
  int qt[64];
  for (int i = 0; i < 64; ++i) qt[i] = std::min(255, std::max(1, (kQ50[i] * scale + 50) / 100));
  Bytes o = {0xFF, 0xD8, 0xFF, 0xDB};
  put16(o, 67);
  o.push_back(0);
  for (int i = 0; i < 64; ++i) o.push_back(std::uint8_t(qt[kZig[i]]));
  o.push_back(0xFF);
  o.push_back(0xC0);
  put16(o, 11);
  o.push_back(8);
  put16(o, unsigned(h));
  put16(o, unsigned(w));
  o.push_back(1);
  o.push_back(1);
  o.push_back(0x11);
  o.push_back(0);
  o.push_back(0xFF);
  o.push_back(0xC4);
  put16(o, 2 + 1 + 16 + 12 + 1 + 16 + 162);
  o.push_back(0x00);
  o.insert(o.end(), kDcBits + 1, kDcBits + 17);
  o.insert(o.end(), kDcVals, kDcVals + 12);
  o.push_back(0x10);
  o.insert(o.end(), kAcBits + 1, kAcBits + 17);
  o.insert(o.end(), kAcVals, kAcVals + 162);
  o.push_back(0xFF);
  o.push_back(0xDA);
  put16(o, 8);
  o.push_back(1);
  o.push_back(1);
  o.push_back(0x00);
  o.push_back(0);
  o.push_back(63);
  o.push_back(0);

  static const Huff dc = makeHuff(kDcBits, kDcVals), ac = makeHuff(kAcBits, kAcVals);
  float cs[8][8];
  for (int u = 0; u < 8; ++u)
    for (int x = 0; x < 8; ++x) cs[u][x] = float((u == 0 ? 0.35355339059 : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
  BitOut bo{o};
  int prevDc = 0;
  auto cat = [](int v) {
    int a = std::abs(v), c = 0;
    while (a) {
      ++c;
      a >>= 1;
    }
    return c;
  };
  for (int by = 0; by < h; by += 8)
    for (int bx = 0; bx < w; bx += 8) {
      float blk[64], tmp[64];
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) blk[y * 8 + x] = float(data[std::size_t(std::min(h - 1, by + y)) * w + std::min(w - 1, bx + x)]) - 128.f;
      for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
          float s = 0;
          for (int x = 0; x < 8; ++x) s += cs[u][x] * blk[y * 8 + x];
          tmp[y * 8 + u] = s;
        }
      int q[64];
      for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
          float s = 0;
          for (int y = 0; y < 8; ++y) s += cs[v][y] * tmp[y * 8 + u];
          q[v * 8 + u] = int(std::lround(s / float(qt[v * 8 + u])));
        }
      auto emit = [&](const Huff& hf, int sym, int val, int c) {
        bo.put(hf.code[sym], int(hf.len[sym]));
        if (c) bo.put(unsigned(val < 0 ? val - 1 : val) & ((1u << c) - 1u), c);
      };
      const int d = q[0] - prevDc;
      prevDc = q[0];
      emit(dc, cat(d), d, cat(d));
      int run = 0;
      for (int i = 1; i < 64; ++i) {
        const int v = q[kZig[i]];
        if (v == 0) {
          ++run;
          continue;
        }
        while (run > 15) {
          emit(ac, 0xF0, 0, 0);
          run -= 16;
        }
        emit(ac, (run << 4) | cat(v), v, cat(v));
        run = 0;
      }
      if (run > 0) emit(ac, 0x00, 0, 0);
    }
  bo.flush();
  o.push_back(0xFF);
  o.push_back(0xD9);
  return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// ISO 21496-1 block, XMP, MPF
namespace {
const char kIsoUrn[] = "urn:iso:std:iso:ts:21496:-1";  // + NUL
const char kXmpHeader[] = "http://ns.adobe.com/xap/1.0/";
constexpr std::int64_t kDen = 1000000;

std::string num(float v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.6g", double(v));
  return b;
}

std::string gainXmp(const Meta& m) {
  std::string x = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Dapple\">\n";
  x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n  <rdf:Description rdf:about=\"\"\n";
  x += "    xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n    hdrgm:Version=\"1.0\"\n";
  x += "    hdrgm:GainMapMin=\"" + num(m.gainMin) + "\"\n    hdrgm:GainMapMax=\"" + num(m.gainMax) + "\"\n    hdrgm:Gamma=\"" + num(m.gamma) + "\"\n";
  x += "    hdrgm:OffsetSDR=\"" + num(m.offsetSdr) + "\"\n    hdrgm:OffsetHDR=\"" + num(m.offsetHdr) + "\"\n";
  x += "    hdrgm:HDRCapacityMin=\"" + num(m.capacityMin) + "\"\n    hdrgm:HDRCapacityMax=\"" + num(m.capacityMax) + "\"\n";
  x += "    hdrgm:BaseRenditionIsHDR=\"False\"/>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
  return x;
}

std::string primaryXmp(std::size_t gainLength) {
  std::string x = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Dapple\">\n";
  x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n  <rdf:Description rdf:about=\"\"\n";
  x += "    xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n    xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n";
  x += "    xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n    hdrgm:Version=\"1.0\">\n   <Container:Directory>\n    <rdf:Seq>\n";
  x += "     <rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>\n";
  x += "     <rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\" Item:Length=\"" + std::to_string(gainLength) +
       "\"/></rdf:li>\n    </rdf:Seq>\n   </Container:Directory>\n  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
  return x;
}

Bytes app(std::uint8_t marker, const void* head, std::size_t headLen, const Bytes& payload) {
  Bytes s = {0xFF, marker};
  put16(s, unsigned(2 + headLen + payload.size()));
  s.insert(s.end(), static_cast<const std::uint8_t*>(head), static_cast<const std::uint8_t*>(head) + headLen);
  s.insert(s.end(), payload.begin(), payload.end());
  return s;
}
Bytes xmpSegment(const std::string& xmp) {
  return app(0xE1, kXmpHeader, sizeof kXmpHeader, Bytes(xmp.begin(), xmp.end()));  // header incl. NUL
}
Bytes isoSegment(const Bytes& payload) { return app(0xE2, kIsoUrn, sizeof kIsoUrn, payload); }

void putS32(Bytes& b, std::int64_t v) { put32(b, std::uint32_t(std::int32_t(v))); }
void frac(Bytes& b, float v, bool isSigned) {
  const std::int64_t n = std::llround(double(v) * double(kDen));
  if (isSigned) putS32(b, n);
  else put32(b, std::uint32_t(n));
  put32(b, std::uint32_t(kDen));
}
}  // namespace

Bytes isoBlock(const Meta& m) {
  Bytes b;
  put16(b, 0);  // minimum version
  put16(b, 0);  // writer version
  b.push_back(0x40);  // not multichannel, use base colour space
  frac(b, 0.f, false);              // base HDR headroom (the SDR base)
  frac(b, m.capacityMax, false);    // alternate HDR headroom
  frac(b, m.gainMin, true);
  frac(b, m.gainMax, true);
  frac(b, m.gamma, false);
  frac(b, m.offsetSdr, true);
  frac(b, m.offsetHdr, true);
  return b;
}

bool parseIso(const std::uint8_t* p, std::size_t n, Meta& m) {
  if (n < 5 + 16 + 40) return false;
  auto r32 = [&](std::size_t o) { return (std::uint32_t(p[o]) << 24) | (std::uint32_t(p[o + 1]) << 16) | (std::uint32_t(p[o + 2]) << 8) | p[o + 3]; };
  auto fr = [&](std::size_t o, bool sgn) {
    const double nn = sgn ? double(std::int32_t(r32(o))) : double(r32(o));
    return float(nn / double(r32(o + 4)));
  };
  if (p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0) return false;  // versions
  if (p[4] & 0x80) return false;                                        // multichannel: not written here
  std::size_t o = 5 + 8;                                                // flags, base headroom
  m.capacityMax = fr(o, false);
  o += 8;
  m.gainMin = fr(o, true);
  o += 8;
  m.gainMax = fr(o, true);
  o += 8;
  m.gamma = fr(o, false);
  o += 8;
  m.offsetSdr = fr(o, true);
  o += 8;
  m.offsetHdr = fr(o, true);
  return true;
}

namespace {
Bytes mpfSegment(std::size_t primaryLen, std::size_t gainOffsetFromHeader, std::size_t gainLen) {
  Bytes t;  // from the TIFF header (offsets are relative to it)
  t.insert(t.end(), {'M', 'M', 0, 42});
  put32(t, 8);
  put16(t, 3);
  put16(t, 0xB000);  // MP format version
  put16(t, 7);
  put32(t, 4);
  t.insert(t.end(), {'0', '1', '0', '0'});
  put16(t, 0xB001);  // number of images
  put16(t, 4);
  put32(t, 1);
  put32(t, 2);
  put16(t, 0xB002);  // MP entries
  put16(t, 7);
  put32(t, 32);
  put32(t, 8 + 2 + 3 * 12 + 4);  // offset of the entries
  put32(t, 0);                   // no next IFD
  put32(t, 0x030000);            // primary: baseline MP primary image, JPEG
  put32(t, std::uint32_t(primaryLen));
  put32(t, 0);
  put16(t, 0);
  put16(t, 0);
  put32(t, 0);  // the gain map: undefined type
  put32(t, std::uint32_t(gainLen));
  put32(t, std::uint32_t(gainOffsetFromHeader));
  put16(t, 0);
  put16(t, 0);
  static const char mpf[4] = {'M', 'P', 'F', 0};
  return app(0xE2, mpf, 4, t);
}

struct Seg {
  std::uint8_t marker;
  std::size_t start, end;  // FF xx .. last payload byte
};
// segments up to (excluding) SOS; sos = offset of FF DA, npos when none
std::vector<Seg> segments(const Bytes& j, std::size_t& sos) {
  std::vector<Seg> v;
  sos = std::size_t(-1);
  std::size_t p = 2;
  while (p + 4 <= j.size() && j[p] == 0xFF) {
    const std::uint8_t mk = j[p + 1];
    if (mk == 0xDA) {
      sos = p;
      break;
    }
    const std::size_t len = (std::size_t(j[p + 2]) << 8) | j[p + 3];
    if (p + 2 + len > j.size()) break;
    v.push_back({mk, p, p + 2 + len});
    p += 2 + len;
  }
  return v;
}
bool isExif(const Bytes& j, const Seg& s) { return s.marker == 0xE1 && s.end - s.start >= 10 && std::memcmp(&j[s.start + 4], "Exif\0\0", 6) == 0; }
}  // namespace

Result<Bytes> assemble(const Bytes& base, const GainMap& gm, int gainQuality) {
  if (base.size() < 4 || base[0] != 0xFF || base[1] != 0xD8) return fail("base is not a JPEG");
  if (gm.width <= 0 || gm.height <= 0 || gm.data.size() != std::size_t(gm.width) * gm.height) return fail("empty gain map");
  std::size_t sos;
  const auto segs = segments(base, sos);
  if (sos == std::size_t(-1)) return fail("base JPEG has no scan");
  std::size_t at = 2;
  for (const auto& s : segs) {
    if (s.marker == 0xE0 || isExif(base, s)) at = s.end;
    else break;
  }
  // gain-map JPEG: its own XMP and ISO block go right after SOI
  const Bytes g0 = encodeGray(gm.data.data(), gm.width, gm.height, gainQuality);
  Bytes gain(g0.begin(), g0.begin() + 2);
  const Bytes gx = xmpSegment(gainXmp(gm.meta)), gi = isoSegment(isoBlock(gm.meta));
  gain.insert(gain.end(), gx.begin(), gx.end());
  gain.insert(gain.end(), gi.begin(), gi.end());
  gain.insert(gain.end(), g0.begin() + 2, g0.end());

  const Bytes px = xmpSegment(primaryXmp(gain.size()));
  Bytes ver;
  put16(ver, 0);
  put16(ver, 0);
  const Bytes pi = isoSegment(ver);
  const std::size_t mpfSize = mpfSegment(0, 0, 0).size();
  const std::size_t primaryLen = base.size() + px.size() + pi.size() + mpfSize;
  const std::size_t headerAt = at + px.size() + pi.size() + 8;  // TIFF header of the MPF segment
  const Bytes mpf = mpfSegment(primaryLen, primaryLen - headerAt, gain.size());
  Bytes out(base.begin(), base.begin() + std::ptrdiff_t(at));
  out.insert(out.end(), px.begin(), px.end());
  out.insert(out.end(), pi.begin(), pi.end());
  out.insert(out.end(), mpf.begin(), mpf.end());
  out.insert(out.end(), base.begin() + std::ptrdiff_t(at), base.end());
  out.insert(out.end(), gain.begin(), gain.end());
  return out;
}

namespace {
std::string attr(const std::string& x, const char* key) {
  const std::string k = std::string(key) + "=\"";
  const std::size_t a = x.find(k);
  if (a == std::string::npos) return {};
  const std::size_t b = x.find('"', a + k.size());
  return b == std::string::npos ? std::string() : x.substr(a + k.size(), b - a - k.size());
}
std::size_t eoiOf(const Bytes& j, std::size_t from) {
  std::size_t sos;
  Bytes view(j.begin() + std::ptrdiff_t(from), j.end());
  segments(view, sos);
  if (sos == std::size_t(-1)) return std::size_t(-1);
  const std::size_t len = (std::size_t(view[sos + 2]) << 8) | view[sos + 3];
  for (std::size_t p = sos + 2 + len; p + 1 < view.size(); ++p)
    if (view[p] == 0xFF && view[p + 1] == 0xD9) return from + p + 2;
  return std::size_t(-1);
}
}  // namespace

Result<Parsed> parse(const Bytes& f) {
  Parsed r;
  if (f.size() < 4 || f[0] != 0xFF || f[1] != 0xD8) return fail("not a JPEG");
  r.primaryEnd = eoiOf(f, 0);
  if (r.primaryEnd == std::size_t(-1)) return fail("primary has no EOI");
  const Bytes primary(f.begin(), f.begin() + std::ptrdiff_t(r.primaryEnd));
  std::size_t sos;
  const auto segs = segments(primary, sos);
  std::vector<bool> drop(segs.size(), false);
  bool haveMpf = false;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const Seg& s = segs[i];
    const std::size_t pl = s.end - s.start - 4;
    const std::uint8_t* pp = &primary[s.start + 4];
    if (s.marker == 0xE1 && pl > sizeof kXmpHeader && std::memcmp(pp, kXmpHeader, sizeof kXmpHeader) == 0) {
      r.primaryXmp.assign(reinterpret_cast<const char*>(pp + sizeof kXmpHeader), pl - sizeof kXmpHeader);
      drop[i] = true;
    } else if (s.marker == 0xE2 && pl > sizeof kIsoUrn && std::memcmp(pp, kIsoUrn, sizeof kIsoUrn) == 0) {
      r.primaryHasIso = true;
      drop[i] = true;
    } else if (s.marker == 0xE2 && pl >= 4 + 8 && std::memcmp(pp, "MPF\0", 4) == 0) {
      drop[i] = true;
      haveMpf = true;
      const std::uint8_t* T = pp + 4;
      const bool le = T[0] == 'I';
      auto r16 = [&](const std::uint8_t* q) { return le ? unsigned(q[0] | (q[1] << 8)) : unsigned((q[0] << 8) | q[1]); };
      auto r32 = [&](const std::uint8_t* q) {
        return le ? std::uint32_t(q[0] | (q[1] << 8) | (q[2] << 16) | (std::uint32_t(q[3]) << 24))
                  : (std::uint32_t(q[0]) << 24) | (std::uint32_t(q[1]) << 16) | (std::uint32_t(q[2]) << 8) | q[3];
      };
      const std::uint8_t* ifd = T + r32(T + 4);
      const unsigned n = r16(ifd);
      for (unsigned k = 0; k < n; ++k) {
        const std::uint8_t* e = ifd + 2 + k * 12;
        if (r16(e) == 0xB001) r.mpfCount = r32(e + 8);
        if (r16(e) == 0xB002) {
          const std::uint8_t* ent = T + r32(e + 8);
          r.mpfPrimarySize = r32(ent + 4);
          r.mpfGainSize = r32(ent + 16 + 4);
          r.mpfGainOffset = r32(ent + 16 + 8);
          r.mpfGainOffsetAbs = std::uint32_t(s.start + 8 + r.mpfGainOffset);
        }
      }
    }
  }
  if (!haveMpf) return fail("no MPF segment");
  if (r.primaryXmp.empty()) return fail("no XMP in the primary image");
  Bytes base(primary.begin(), primary.begin() + 2);
  {
    std::size_t p = 2;
    for (std::size_t i = 0; i < segs.size(); ++i) {
      if (!drop[i]) base.insert(base.end(), primary.begin() + std::ptrdiff_t(segs[i].start), primary.begin() + std::ptrdiff_t(segs[i].end));
      p = segs[i].end;
    }
    base.insert(base.end(), primary.begin() + std::ptrdiff_t(p), primary.end());
  }
  r.base = std::move(base);
  r.gainOffset = r.mpfGainOffsetAbs;
  r.gainSize = r.mpfGainSize;
  if (r.gainOffset + r.gainSize > f.size() || r.gainSize < 4) return fail("MPF gain-map entry lies outside the file");
  r.gainJpeg.assign(f.begin() + std::ptrdiff_t(r.gainOffset), f.begin() + std::ptrdiff_t(r.gainOffset + r.gainSize));
  if (r.gainJpeg[0] != 0xFF || r.gainJpeg[1] != 0xD8) return fail("gain map does not start with SOI");
  r.gainSemantic = r.primaryXmp.find("Item:Semantic=\"GainMap\"") != std::string::npos ? "GainMap" : "";
  const std::size_t gmAt = r.primaryXmp.find("Item:Semantic=\"GainMap\"");
  const std::string len = gmAt == std::string::npos ? std::string() : attr(r.primaryXmp.substr(gmAt), "Item:Length");
  r.directoryLength = len.empty() ? 0 : std::size_t(std::stoull(len));
  std::size_t gs;
  const auto gsegs = segments(r.gainJpeg, gs);
  for (const auto& s : gsegs) {
    const std::size_t pl = s.end - s.start - 4;
    const std::uint8_t* pp = &r.gainJpeg[s.start + 4];
    if (s.marker == 0xE1 && pl > sizeof kXmpHeader && std::memcmp(pp, kXmpHeader, sizeof kXmpHeader) == 0)
      r.gainXmp.assign(reinterpret_cast<const char*>(pp + sizeof kXmpHeader), pl - sizeof kXmpHeader);
    if (s.marker == 0xE2 && pl > sizeof kIsoUrn && std::memcmp(pp, kIsoUrn, sizeof kIsoUrn) == 0) {
      r.gainHasIso = true;
      parseIso(pp + sizeof kIsoUrn, pl - sizeof kIsoUrn, r.isoMeta);
    }
    if (s.marker == 0xC0 || s.marker == 0xC2) {
      r.gainHeight = (pp[1] << 8) | pp[2];
      r.gainWidth = (pp[3] << 8) | pp[4];
      r.gainComponents = pp[5];
    }
  }
  auto f1 = [&](const char* k, float d) {
    const std::string v = attr(r.gainXmp, k);
    return v.empty() ? d : std::stof(v);
  };
  r.meta.gainMin = f1("hdrgm:GainMapMin", 0.f);
  r.meta.gainMax = f1("hdrgm:GainMapMax", 1.f);
  r.meta.gamma = f1("hdrgm:Gamma", 1.f);
  r.meta.offsetSdr = f1("hdrgm:OffsetSDR", 1.f / 64.f);
  r.meta.offsetHdr = f1("hdrgm:OffsetHDR", 1.f / 64.f);
  r.meta.capacityMin = f1("hdrgm:HDRCapacityMin", 0.f);
  r.meta.capacityMax = f1("hdrgm:HDRCapacityMax", 1.f);
  return r;
}

}  // namespace dapple::uhdr
