#include "exif_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace dapple {

namespace {
using Bytes = std::vector<std::uint8_t>;

void put16le(Bytes& b, unsigned v) {
  b.push_back(std::uint8_t(v & 255));
  b.push_back(std::uint8_t((v >> 8) & 255));
}
void put32le(Bytes& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t((v >> (8 * i)) & 255));
}
void put16be(Bytes& b, unsigned v) {
  b.push_back(std::uint8_t((v >> 8) & 255));
  b.push_back(std::uint8_t(v & 255));
}
void put32be(Bytes& b, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(std::uint8_t((v >> (8 * i)) & 255));
}

// ---- Exif writer ----------------------------------------------------------------------------------------------------------
enum : std::uint16_t { tByte = 1, tAscii = 2, tShort = 3, tLong = 4, tRational = 5, tUndef = 7 };

struct Ent {
  std::uint16_t tag, type;
  std::uint32_t count;
  Bytes v;  // little-endian value bytes
};

Ent ascii(std::uint16_t tag, const std::string& s) {
  Ent e{tag, tAscii, std::uint32_t(s.size() + 1), Bytes(s.begin(), s.end())};
  e.v.push_back(0);
  return e;
}
Ent shortE(std::uint16_t tag, unsigned v) {
  Ent e{tag, tShort, 1, {}};
  put16le(e.v, v);
  return e;
}
Ent longE(std::uint16_t tag, std::uint32_t v) {
  Ent e{tag, tLong, 1, {}};
  put32le(e.v, v);
  return e;
}
Ent bytesE(std::uint16_t tag, std::uint16_t type, const Bytes& v) { return Ent{tag, type, std::uint32_t(v.size()), v}; }

void toRational(double v, std::uint32_t& n, std::uint32_t& d) {
  if (!(v > 0) || !std::isfinite(v)) {
    n = 0;
    d = 1;
    return;
  }
  std::uint64_t den = 1;
  while (den < 1000000 && std::fabs(v * double(den) - std::round(v * double(den))) > 1e-4) den *= 10;
  double num = std::round(v * double(den));
  while (num > 4.0e9) {
    num = std::round(num / 10);
    den = std::max<std::uint64_t>(1, den / 10);
  }
  std::uint64_t a = std::uint64_t(num), b = den;
  while (b) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  if (a == 0) a = 1;
  n = std::uint32_t(std::uint64_t(num) / a);
  d = std::uint32_t(den / a);
}
Ent rationals(std::uint16_t tag, const std::vector<double>& vals) {
  Ent e{tag, tRational, std::uint32_t(vals.size()), {}};
  for (double v : vals) {
    std::uint32_t n, d;
    toRational(v, n, d);
    put32le(e.v, n);
    put32le(e.v, d);
  }
  return e;
}
Ent rationalExact(std::uint16_t tag, std::uint32_t n, std::uint32_t d) {
  Ent e{tag, tRational, 1, {}};
  put32le(e.v, n);
  put32le(e.v, d);
  return e;
}

std::size_t ifdBytes(const std::vector<Ent>& es) {
  std::size_t n = 2 + 12 * es.size() + 4;
  for (const auto& e : es)
    if (e.v.size() > 4) n += (e.v.size() + 1) & ~std::size_t(1);
  return n;
}

// Appends the IFD that starts at buf.size() (which is its TIFF offset): entries sorted by tag, long values after the table.
void emitIfd(Bytes& buf, std::vector<Ent> es) {
  std::sort(es.begin(), es.end(), [](const Ent& a, const Ent& b) { return a.tag < b.tag; });
  const std::size_t at = buf.size();
  const std::size_t data = at + 2 + 12 * es.size() + 4;
  put16le(buf, unsigned(es.size()));
  Bytes tail;
  for (const auto& e : es) {
    put16le(buf, e.tag);
    put16le(buf, e.type);
    put32le(buf, e.count);
    if (e.v.size() <= 4) {
      buf.insert(buf.end(), e.v.begin(), e.v.end());
      buf.insert(buf.end(), 4 - e.v.size(), 0);
    } else {
      put32le(buf, std::uint32_t(data + tail.size()));
      tail.insert(tail.end(), e.v.begin(), e.v.end());
      if (tail.size() & 1) tail.push_back(0);
    }
  }
  put32le(buf, 0);
  buf.insert(buf.end(), tail.begin(), tail.end());
}

bool validDate(const std::string& s) {
  if (s.size() != 19) return false;
  for (std::size_t i = 0; i < 19; ++i) {
    const char c = s[i];
    if (i == 4 || i == 7 || i == 13 || i == 16) {
      if (c != ':') return false;
    } else if (i == 10) {
      if (c != ' ') return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}
bool digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool validOffset(const std::string& s) {
  return s.size() == 6 && (s[0] == '+' || s[0] == '-') && s[3] == ':' && digits(s.substr(1, 2)) && digits(s.substr(4, 2));
}

std::vector<double> dms(double deg) {
  deg = std::fabs(deg);
  const double d = std::floor(deg);
  const double mFull = (deg - d) * 60.0;
  const double m = std::floor(mFull);
  double s = (mFull - m) * 60.0;
  // seconds on a 1/10000 grid so the rational is short and exact
  s = std::round(s * 10000.0) / 10000.0;
  return {d, m, s};
}
}  // namespace

std::vector<std::uint8_t> buildExifSegment(const ExportMeta& m) {
  std::vector<Ent> ifd0, exif, gps;
  if (!m.make.empty()) ifd0.push_back(ascii(0x010F, m.make));
  if (!m.model.empty()) ifd0.push_back(ascii(0x0110, m.model));
  ifd0.push_back(shortE(0x0112, 1));  // Orientation: normal, the pixels are already upright
  ifd0.push_back(rationalExact(0x011A, 72, 1));
  ifd0.push_back(rationalExact(0x011B, 72, 1));
  ifd0.push_back(shortE(0x0128, 2));
  if (!m.software.empty()) ifd0.push_back(ascii(0x0131, m.software));
  const bool haveDate = validDate(m.dateTimeOriginal);
  if (haveDate) ifd0.push_back(ascii(0x0132, m.dateTimeOriginal));
  ifd0.push_back(shortE(0x0213, 1));  // YCbCrPositioning: centred

  if (m.shutterSeconds > 0) {
    const double t = double(m.shutterSeconds);
    std::uint32_t n = 1, d = 1;
    if (t < 1.0) {
      const double den = std::round(1.0 / t);
      if (den >= 1 && std::fabs(1.0 / den - t) <= 0.01 * t) {
        n = 1;
        d = std::uint32_t(den);
      } else {
        toRational(t, n, d);
      }
    } else {
      toRational(t, n, d);
    }
    exif.push_back(rationalExact(0x829A, n, d));
  }
  if (m.aperture > 0) exif.push_back(rationals(0x829D, {double(m.aperture)}));
  if (m.iso > 0) exif.push_back(shortE(0x8827, unsigned(std::min(65535.f, std::round(m.iso)))));
  exif.push_back(bytesE(0x9000, tUndef, {'0', '2', '3', '2'}));
  if (haveDate) {
    exif.push_back(ascii(0x9003, m.dateTimeOriginal));
    exif.push_back(ascii(0x9004, m.dateTimeOriginal));
    if (validOffset(m.offsetTime)) {
      exif.push_back(ascii(0x9010, m.offsetTime));
      exif.push_back(ascii(0x9011, m.offsetTime));
      exif.push_back(ascii(0x9012, m.offsetTime));
    }
    if (digits(m.subSec) && m.subSec.size() <= 9) {
      exif.push_back(ascii(0x9290, m.subSec));
      exif.push_back(ascii(0x9291, m.subSec));
      exif.push_back(ascii(0x9292, m.subSec));
    }
  }
  exif.push_back(bytesE(0x9101, tUndef, {1, 2, 3, 0}));
  if (m.focalLengthMm > 0) exif.push_back(rationals(0x920A, {double(m.focalLengthMm)}));
  exif.push_back(bytesE(0xA000, tUndef, {'0', '1', '0', '0'}));
  exif.push_back(shortE(0xA001, unsigned(m.colorSpace)));
  if (m.width > 0 && m.height > 0) {
    exif.push_back(longE(0xA002, std::uint32_t(m.width)));
    exif.push_back(longE(0xA003, std::uint32_t(m.height)));
  }
  if (!m.lens.empty()) exif.push_back(ascii(0xA434, m.lens));

  const bool withGps = m.hasGps && std::isfinite(m.latitude) && std::isfinite(m.longitude) && std::fabs(m.latitude) <= 90 && std::fabs(m.longitude) <= 180;
  if (withGps) {
    gps.push_back(bytesE(0x0000, tByte, {2, 3, 0, 0}));
    gps.push_back(ascii(0x0001, m.latitude < 0 ? "S" : "N"));
    gps.push_back(rationals(0x0002, dms(m.latitude)));
    gps.push_back(ascii(0x0003, m.longitude < 0 ? "W" : "E"));
    gps.push_back(rationals(0x0004, dms(m.longitude)));
    if (m.hasAltitude) {
      gps.push_back(bytesE(0x0005, tByte, {std::uint8_t(m.altitude < 0 ? 1 : 0)}));
      gps.push_back(rationals(0x0006, {std::fabs(m.altitude)}));
    }
    if (m.hasGpsTime) gps.push_back(rationals(0x0007, {m.gpsTime[0], m.gpsTime[1], m.gpsTime[2]}));
    if (m.gpsDate.size() == 10) gps.push_back(ascii(0x001D, m.gpsDate));
  }

  // pointers: the sizes do not depend on their values
  ifd0.push_back(longE(0x8769, 0));
  if (withGps) ifd0.push_back(longE(0x8825, 0));
  const std::uint32_t o0 = 8, oE = std::uint32_t(o0 + ifdBytes(ifd0)), oG = std::uint32_t(oE + ifdBytes(exif));
  for (auto& e : ifd0) {
    if (e.tag == 0x8769) e.v = {std::uint8_t(oE), std::uint8_t(oE >> 8), std::uint8_t(oE >> 16), std::uint8_t(oE >> 24)};
    if (e.tag == 0x8825) e.v = {std::uint8_t(oG), std::uint8_t(oG >> 8), std::uint8_t(oG >> 16), std::uint8_t(oG >> 24)};
  }
  Bytes tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
  emitIfd(tiff, ifd0);
  emitIfd(tiff, exif);
  if (withGps) emitIfd(tiff, gps);

  Bytes seg = {0xFF, 0xE1};
  put16be(seg, unsigned(2 + 6 + tiff.size()));
  const char hdr[6] = {'E', 'x', 'i', 'f', 0, 0};
  seg.insert(seg.end(), hdr, hdr + 6);
  seg.insert(seg.end(), tiff.begin(), tiff.end());
  return seg;
}

// ---- ICC ------------------------------------------------------------------------------------------------------------------
namespace {
std::uint32_t s15(double v) { return std::uint32_t(std::int32_t(std::lround(v * 65536.0))); }
void sig(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
void padTo4(Bytes& b) {
  while (b.size() & 3) b.push_back(0);
}
Bytes mluc(const std::string& text) {
  Bytes b;
  sig(b, "mluc");
  put32be(b, 0);
  put32be(b, 1);
  put32be(b, 12);
  b.push_back('e');
  b.push_back('n');
  b.push_back('U');
  b.push_back('S');
  put32be(b, std::uint32_t(text.size() * 2));
  put32be(b, 28);
  for (char c : text) {
    b.push_back(0);
    b.push_back(std::uint8_t(c));
  }
  padTo4(b);
  return b;
}
Bytes xyz(double x, double y, double z) {
  Bytes b;
  sig(b, "XYZ ");
  put32be(b, 0);
  put32be(b, s15(x));
  put32be(b, s15(y));
  put32be(b, s15(z));
  return b;
}
}  // namespace

std::vector<std::uint8_t> buildIccProfile(IccSpace space) {
  // colorants adapted to D50 (Bradford); both spaces use the sRGB transfer curve
  const bool p3 = space == IccSpace::DisplayP3;
  const double prim[3][3] = {{0.4360747, 0.2225045, 0.0139322}, {0.3850649, 0.7168786, 0.0971045}, {0.1430804, 0.0606169, 0.7141733}};
  const double prim3[3][3] = {{0.5151146, 0.2411999, -0.0010521}, {0.2919776, 0.6922491, 0.0418842}, {0.1571181, 0.0665510, 0.7840791}};
  const auto& P = p3 ? prim3 : prim;
  struct Tag {
    const char* sg;
    Bytes data;
    int shareWith;
  };
  std::vector<Tag> tags;
  tags.push_back({"cprt", mluc("No copyright, use freely"), -1});
  tags.push_back({"desc", mluc(p3 ? "Display P3" : "sRGB"), -1});
  tags.push_back({"wtpt", xyz(0.9642, 1.0, 0.8249), -1});
  tags.push_back({"rXYZ", xyz(P[0][0], P[0][1], P[0][2]), -1});
  tags.push_back({"gXYZ", xyz(P[1][0], P[1][1], P[1][2]), -1});
  tags.push_back({"bXYZ", xyz(P[2][0], P[2][1], P[2][2]), -1});
  Bytes para;  // parametricCurveType 3: Y = (aX + b)^g for X >= d, cX below
  sig(para, "para");
  put32be(para, 0);
  put16be(para, 3);
  put16be(para, 0);
  put32be(para, s15(2.4));
  put32be(para, s15(1.0 / 1.055));
  put32be(para, s15(0.055 / 1.055));
  put32be(para, s15(1.0 / 12.92));
  put32be(para, s15(0.04045));
  tags.push_back({"rTRC", para, -1});
  tags.push_back({"gTRC", {}, 6});
  tags.push_back({"bTRC", {}, 6});

  const std::size_t n = tags.size();
  Bytes body;
  std::vector<std::uint32_t> off(n), size(n);
  const std::size_t base = 128 + 4 + 12 * n;
  for (std::size_t i = 0; i < n; ++i) {
    if (tags[i].shareWith >= 0) {
      off[i] = off[std::size_t(tags[i].shareWith)];
      size[i] = size[std::size_t(tags[i].shareWith)];
      continue;
    }
    off[i] = std::uint32_t(base + body.size());
    size[i] = std::uint32_t(tags[i].data.size());
    body.insert(body.end(), tags[i].data.begin(), tags[i].data.end());
    padTo4(body);
  }
  Bytes p;
  put32be(p, std::uint32_t(base + body.size()));
  put32be(p, 0);
  put32be(p, 0x04300000);
  sig(p, "mntr");
  sig(p, "RGB ");
  sig(p, "XYZ ");
  // fixed creation date: the profile bytes are reproducible
  put16be(p, 2024), put16be(p, 1), put16be(p, 1), put16be(p, 0), put16be(p, 0), put16be(p, 0);
  sig(p, "acsp");
  p.insert(p.end(), 28, 0);  // platform, flags, manufacturer, model, attributes
  put32be(p, 0);             // rendering intent: perceptual
  put32be(p, s15(0.9642));
  put32be(p, s15(1.0));
  put32be(p, s15(0.8249));
  sig(p, "DPPL");
  p.insert(p.end(), 128 - p.size(), 0);  // profile ID (not computed) and reserved
  put32be(p, std::uint32_t(n));
  for (std::size_t i = 0; i < n; ++i) {
    sig(p, tags[i].sg);
    put32be(p, off[i]);
    put32be(p, size[i]);
  }
  p.insert(p.end(), body.begin(), body.end());
  return p;
}

std::vector<std::uint8_t> buildIccSegment(IccSpace space) {
  const Bytes prof = buildIccProfile(space);
  Bytes seg = {0xFF, 0xE2};
  put16be(seg, unsigned(2 + 12 + 2 + prof.size()));
  const char id[12] = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
  seg.insert(seg.end(), id, id + 12);
  seg.push_back(1);
  seg.push_back(1);
  seg.insert(seg.end(), prof.begin(), prof.end());
  return seg;
}

// ---- reader ---------------------------------------------------------------------------------------------------------------
namespace {
// A TIFF block: in memory or at an offset of an open file.
struct Blob {
  const std::uint8_t* mem = nullptr;
  std::size_t memSize = 0;
  std::ifstream* f = nullptr;
  std::uint64_t base = 0, size = 0;  // file mode: TIFF origin and bytes available from it
  bool read(std::uint64_t off, void* dst, std::size_t n) const {
    if (off + n > (mem ? memSize : size) || off + n < off) return false;
    if (mem) {
      std::memcpy(dst, mem + off, n);
      return true;
    }
    f->clear();
    f->seekg(std::streamoff(base + off));
    f->read(static_cast<char*>(dst), std::streamsize(n));
    return bool(*f);
  }
};

struct Rd {
  const Blob& b;
  bool le;
  bool u16(std::uint64_t off, unsigned& v) const {
    std::uint8_t t[2];
    if (!b.read(off, t, 2)) return false;
    v = le ? unsigned(t[0] | (t[1] << 8)) : unsigned(t[1] | (t[0] << 8));
    return true;
  }
  bool u32(std::uint64_t off, std::uint32_t& v) const {
    std::uint8_t t[4];
    if (!b.read(off, t, 4)) return false;
    v = le ? (std::uint32_t(t[0]) | (std::uint32_t(t[1]) << 8) | (std::uint32_t(t[2]) << 16) | (std::uint32_t(t[3]) << 24))
           : (std::uint32_t(t[3]) | (std::uint32_t(t[2]) << 8) | (std::uint32_t(t[1]) << 16) | (std::uint32_t(t[0]) << 24));
    return true;
  }
};

struct Ent2 {
  unsigned tag = 0, type = 0;
  std::uint32_t count = 0;
  std::uint64_t at = 0;  // where the value bytes are
};

std::size_t typeSize(unsigned t) {
  switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
  }
}

bool readIfd(const Rd& r, std::uint64_t off, std::vector<Ent2>& out) {
  unsigned n;
  if (!r.u16(off, n) || n == 0 || n > 1024) return false;
  for (unsigned i = 0; i < n; ++i) {
    const std::uint64_t e = off + 2 + 12ull * i;
    Ent2 x;
    if (!r.u16(e, x.tag) || !r.u16(e + 2, x.type) || !r.u32(e + 4, x.count)) return false;
    const std::size_t ts = typeSize(x.type);
    if (ts == 0 || std::uint64_t(x.count) * ts > (1u << 20)) continue;
    if (std::uint64_t(x.count) * ts <= 4) {
      x.at = e + 8;
    } else {
      std::uint32_t o;
      if (!r.u32(e + 8, o)) continue;
      x.at = o;
    }
    out.push_back(x);
  }
  return true;
}
const Ent2* find(const std::vector<Ent2>& v, unsigned tag) {
  for (const auto& e : v)
    if (e.tag == tag) return &e;
  return nullptr;
}
std::string str(const Rd& r, const Ent2* e) {
  if (!e || e->type != 2 || e->count == 0) return {};
  std::string s(e->count, '\0');
  if (!r.b.read(e->at, &s[0], s.size())) return {};
  const std::size_t z = s.find('\0');
  if (z != std::string::npos) s.resize(z);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
  return s;
}
bool num(const Rd& r, const Ent2* e, std::size_t i, double& v) {
  if (!e || i >= e->count) return false;
  switch (e->type) {
    case 3: {
      unsigned x;
      if (!r.u16(e->at + 2 * i, x)) return false;
      v = x;
      return true;
    }
    case 4: {
      std::uint32_t x;
      if (!r.u32(e->at + 4 * i, x)) return false;
      v = x;
      return true;
    }
    case 1: {
      std::uint8_t x;
      if (!r.b.read(e->at + i, &x, 1)) return false;
      v = x;
      return true;
    }
    case 5: {
      std::uint32_t a, b;
      if (!r.u32(e->at + 8 * i, a) || !r.u32(e->at + 8 * i + 4, b) || b == 0) return false;
      v = double(a) / double(b);
      return true;
    }
    default:
      return false;
  }
}
std::string cleanStr(std::string s) {
  for (auto& c : s)
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

bool parseBlob(const Blob& b, ExportMeta& m) {
  std::uint8_t h[8];
  if (!b.read(0, h, 8)) return false;
  if (!((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M'))) return false;
  const Rd r{b, h[0] == 'I'};
  std::uint32_t o0;
  if (!r.u32(4, o0)) return false;
  std::vector<Ent2> i0;
  if (!readIfd(r, o0, i0)) return false;
  bool any = false;
  if (auto s = cleanStr(str(r, find(i0, 0x010F))); !s.empty()) m.make = s, any = true;
  if (auto s = cleanStr(str(r, find(i0, 0x0110))); !s.empty()) m.model = s, any = true;
  const std::string dt0 = str(r, find(i0, 0x0132));
  std::vector<Ent2> ex;
  double v;
  if (num(r, find(i0, 0x8769), 0, v) && readIfd(r, std::uint64_t(v), ex)) {
    if (num(r, find(ex, 0x829A), 0, v) && v > 0) m.shutterSeconds = float(v), any = true;
    if (num(r, find(ex, 0x829D), 0, v) && v > 0) m.aperture = float(v), any = true;
    if (num(r, find(ex, 0x8827), 0, v) && v > 0) m.iso = float(v), any = true;
    else if (num(r, find(ex, 0x8833), 0, v) && v > 0) m.iso = float(v), any = true;
    if (num(r, find(ex, 0x920A), 0, v) && v > 0) m.focalLengthMm = float(v), any = true;
    if (auto s = cleanStr(str(r, find(ex, 0xA434))); !s.empty()) m.lens = s, any = true;
    const std::string d = str(r, find(ex, 0x9003));
    if (validDate(d)) {
      m.dateTimeOriginal = d;
      any = true;
      const std::string ss = str(r, find(ex, 0x9291));
      if (digits(ss) && ss.size() <= 9) m.subSec = ss;
      std::string of = str(r, find(ex, 0x9011));
      if (of.empty()) of = str(r, find(ex, 0x9010));
      if (validOffset(of)) m.offsetTime = of;
    }
  }
  if (m.dateTimeOriginal.empty() && validDate(dt0)) m.dateTimeOriginal = dt0, any = true;
  std::vector<Ent2> gp;
  if (num(r, find(i0, 0x8825), 0, v) && readIfd(r, std::uint64_t(v), gp)) {
    const std::string la = str(r, find(gp, 0x0001)), lo = str(r, find(gp, 0x0003));
    const Ent2 *lat = find(gp, 0x0002), *lon = find(gp, 0x0004);
    double d[3], e[3];
    if (!la.empty() && !lo.empty() && lat && lon && lat->count >= 3 && lon->count >= 3 && num(r, lat, 0, d[0]) && num(r, lat, 1, d[1]) &&
        num(r, lat, 2, d[2]) && num(r, lon, 0, e[0]) && num(r, lon, 1, e[1]) && num(r, lon, 2, e[2])) {
      double la1 = d[0] + d[1] / 60 + d[2] / 3600, lo1 = e[0] + e[1] / 60 + e[2] / 3600;
      if (la[0] == 'S') la1 = -la1;
      if (lo[0] == 'W') lo1 = -lo1;
      // 0,0 is what a phone writes without a fix
      if (std::isfinite(la1) && std::isfinite(lo1) && std::fabs(la1) <= 90 && std::fabs(lo1) <= 180 && (la1 != 0 || lo1 != 0)) {
        m.hasGps = true;
        m.latitude = la1;
        m.longitude = lo1;
        double a, ar = 0;
        m.hasAltitude = num(r, find(gp, 0x0006), 0, a);
        if (m.hasAltitude) {
          num(r, find(gp, 0x0005), 0, ar);
          m.altitude = ar == 1 ? -a : a;
        }
        const Ent2* ts = find(gp, 0x0007);
        double t[3];
        if (ts && ts->count >= 3 && num(r, ts, 0, t[0]) && num(r, ts, 1, t[1]) && num(r, ts, 2, t[2])) {
          m.hasGpsTime = true;
          for (int i = 0; i < 3; ++i) m.gpsTime[i] = t[i];
        }
        const std::string gd = str(r, find(gp, 0x001D));
        if (gd.size() == 10) m.gpsDate = gd;
        any = true;
      }
    }
  }
  return any;
}

bool parseMem(const Bytes& tiff, ExportMeta& m) {
  Blob b;
  b.mem = tiff.data();
  b.memSize = tiff.size();
  return parseBlob(b, m);
}

// JPEG at `base` of an open file: the Exif APP1 segment.
bool fromJpeg(std::ifstream& f, std::uint64_t base, std::uint64_t fileSize, ExportMeta& m) {
  std::uint64_t p = base + 2;
  std::uint8_t h[4];
  while (p + 4 <= fileSize) {
    f.clear();
    f.seekg(std::streamoff(p));
    f.read(reinterpret_cast<char*>(h), 4);
    if (!f || h[0] != 0xFF) return false;
    if (h[1] == 0xFF) {
      ++p;
      continue;
    }
    if (h[1] == 0xDA || h[1] == 0xD9) return false;
    const std::uint64_t len = (std::uint64_t(h[2]) << 8) | h[3];
    if (len < 2) return false;
    if (h[1] == 0xE1 && len >= 8) {
      Bytes seg(len - 2);
      f.read(reinterpret_cast<char*>(seg.data()), std::streamsize(seg.size()));
      if (f && std::memcmp(seg.data(), "Exif\0\0", 6) == 0) {
        const Bytes tiff(seg.begin() + 6, seg.end());
        return parseMem(tiff, m);
      }
    }
    p += 2 + len;
  }
  return false;
}

// HEIC / AVIF: the Exif item is "offset(4) [Exif\0\0] TIFF"; found by its TIFF header (IFD0 at 8) after either marker.
bool fromIsoBmff(std::ifstream& f, std::uint64_t size, ExportMeta& m) {
  const std::size_t chunk = 4u << 20, window = 1u << 20;
  Bytes buf;
  for (std::uint64_t at = 0; at < size && at < (256ull << 20); at += chunk) {
    const std::size_t n = std::size_t(std::min<std::uint64_t>(chunk + 16, size - at));
    buf.resize(n);
    f.clear();
    f.seekg(std::streamoff(at));
    f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n));
    if (!f) return false;
    for (std::size_t i = 0; i + 8 <= n && i < chunk; ++i) {
      const bool ii = buf[i] == 'I' && buf[i + 1] == 'I' && buf[i + 2] == 42 && buf[i + 3] == 0 && buf[i + 4] == 8 && buf[i + 5] == 0;
      const bool mm = buf[i] == 'M' && buf[i + 1] == 'M' && buf[i + 2] == 0 && buf[i + 3] == 42 && buf[i + 4] == 0 && buf[i + 5] == 0 &&
                      buf[i + 6] == 0 && buf[i + 7] == 8;
      if (!ii && !mm) continue;
      const bool afterTag = i >= 6 && std::memcmp(&buf[i - 6], "Exif\0\0", 6) == 0;
      const bool afterZero = i >= 4 && buf[i - 4] == 0 && buf[i - 3] == 0 && buf[i - 2] == 0 && buf[i - 1] == 0;
      if (!afterTag && !afterZero) continue;
      Bytes tiff;
      tiff.resize(std::size_t(std::min<std::uint64_t>(window, size - (at + i))));
      f.clear();
      f.seekg(std::streamoff(at + i));
      f.read(reinterpret_cast<char*>(tiff.data()), std::streamsize(tiff.size()));
      if (!f) continue;
      ExportMeta t = m;
      if (parseMem(tiff, t)) {
        m = t;
        return true;
      }
    }
  }
  return false;
}
}  // namespace

bool parseExifTiff(const std::uint8_t* tiff, std::size_t size, ExportMeta& m) {
  Blob b;
  b.mem = tiff;
  b.memSize = size;
  return parseBlob(b, m);
}

bool readSourceExif(const std::filesystem::path& file, ExportMeta& m) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::uint64_t size = std::uint64_t(f.tellg());
  f.seekg(0);
  std::uint8_t h[96] = {};
  f.read(reinterpret_cast<char*>(h), 96);
  const std::size_t got = std::size_t(f.gcount());
  if (got < 12) return false;
  ExportMeta t = m;
  bool ok = false;
  if (h[0] == 0xFF && h[1] == 0xD8) {
    ok = fromJpeg(f, 0, size, t);
  } else if ((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M')) {
    Blob b;
    b.f = &f;
    b.base = 0;
    b.size = size;
    ok = parseBlob(b, t);
  } else if (std::memcmp(h, "FUJIFILMCCD-RAW", 15) == 0 && got >= 92) {
    const std::uint64_t off = (std::uint64_t(h[84]) << 24) | (std::uint64_t(h[85]) << 16) | (std::uint64_t(h[86]) << 8) | h[87];
    std::uint8_t j[2] = {};
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(j), 2);
    ok = off > 0 && off < size && f && j[0] == 0xFF && j[1] == 0xD8 && fromJpeg(f, off, size, t);
  } else if (std::memcmp(h, "\x89PNG\r\n\x1a\n", 8) == 0) {
    std::uint64_t p = 8;
    while (p + 12 <= size) {
      std::uint8_t c[8];
      f.clear();
      f.seekg(std::streamoff(p));
      f.read(reinterpret_cast<char*>(c), 8);
      if (!f) break;
      const std::uint64_t len = (std::uint64_t(c[0]) << 24) | (std::uint64_t(c[1]) << 16) | (std::uint64_t(c[2]) << 8) | c[3];
      if (std::memcmp(c + 4, "eXIf", 4) == 0 && len < (1u << 24)) {
        Bytes tiff;
        tiff.resize(std::size_t(len));
        f.read(reinterpret_cast<char*>(tiff.data()), std::streamsize(len));
        ok = f && parseMem(tiff, t);
        break;
      }
      if (std::memcmp(c + 4, "IDAT", 4) == 0 || std::memcmp(c + 4, "IEND", 4) == 0) break;
      p += 12 + len;
    }
  } else if (std::memcmp(h + 4, "ftyp", 4) == 0) {
    ok = fromIsoBmff(f, size, t);
  }
  if (ok) m = t;
  return ok;
}

}  // namespace dapple
