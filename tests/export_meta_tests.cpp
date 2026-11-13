// Export metadata tests: the Exif APP1 block and the sRGB / Display P3 ICC profile of an exported JPEG, location stripping (plain JPEG, Ultra HDR, Motion
// Photo), the threaded strip encoder with the segments in place, the Ultra HDR base equal to the plain export, the source reader, and the C API export.
// `dapple_export_meta_tests <id>`; ids: icc exif jpeg strip uhdr motion reader session. The C API is compiled in (as in uhdr_tests). `session` and the
// RAW part of `reader` skip (exit 77) without build/rawsamples.

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "colour_space.h"
#include "dapple/dapple_c.h"
#include "dapple/encode.h"
#include "dapple/motion_photo.h"
#include "dapple/ultrahdr.h"
#include "exif_writer.h"

namespace fs = std::filesystem;
using namespace dapple;
using Bytes = std::vector<std::uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

static fs::path root() {
#ifdef DAPPLE_SOURCE_DIR
  return fs::path(DAPPLE_SOURCE_DIR);
#else
  return fs::current_path();
#endif
}
static Bytes readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void writeAll(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// ---- an independent little-endian TIFF walker (the writer's own reader is not used to judge the writer) ----
struct Tag {
  unsigned type = 0;
  std::uint32_t count = 0;
  Bytes v;
};
using Ifd = std::map<unsigned, Tag>;
static unsigned r16(const Bytes& b, std::size_t o) { return b[o] | (b[o + 1] << 8); }
static std::uint32_t r32(const Bytes& b, std::size_t o) { return r16(b, o) | (std::uint32_t(r16(b, o + 2)) << 16); }
static bool walk(const Bytes& t, std::uint32_t off, Ifd& out) {
  if (off + 2 > t.size()) return false;
  const unsigned n = r16(t, off);
  for (unsigned i = 0; i < n; ++i) {
    const std::size_t e = off + 2 + 12 * i;
    if (e + 12 > t.size()) return false;
    Tag g;
    g.type = r16(t, e + 2);
    g.count = r32(t, e + 4);
    static const unsigned sz[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8};
    const std::size_t len = std::size_t(g.count) * (g.type < 11 ? sz[g.type] : 1);
    const std::size_t at = len <= 4 ? e + 8 : r32(t, e + 8);
    if (at + len > t.size()) return false;
    g.v.assign(t.begin() + std::ptrdiff_t(at), t.begin() + std::ptrdiff_t(at + len));
    out[r16(t, e)] = g;
  }
  return true;
}
struct Exif {
  Ifd i0, ex, gps;
  bool haveGpsPtr = false;
};
static std::string ascii(const Tag& g) { return std::string(g.v.begin(), g.v.end() - (g.v.empty() ? 0 : 1)); }
static double rat(const Tag& g, std::size_t i = 0) { return double(r32(g.v, 8 * i)) / double(r32(g.v, 8 * i + 4)); }

// the Exif APP1 segments of a JPEG stream's leading segments (before the first SOS), start offsets
struct Seg {
  unsigned marker;
  std::size_t at, len;  // at = marker position, len = bytes including the marker
};
static std::vector<Seg> leadSegments(const Bytes& j) {
  std::vector<Seg> v;
  std::size_t p = 2;
  while (p + 4 <= j.size() && j[p] == 0xFF) {
    const unsigned m = j[p + 1];
    if (m == 0xDA) break;
    const std::size_t len = 2 + ((std::size_t(j[p + 2]) << 8) | j[p + 3]);
    v.push_back({m, p, len});
    p += len;
  }
  return v;
}
static bool parseExifSegment(const Bytes& j, const Seg& s, Exif& e) {
  if (s.marker != 0xE1 || s.len < 12 || std::memcmp(&j[s.at + 4], "Exif\0\0", 6) != 0) return false;
  const Bytes t(j.begin() + std::ptrdiff_t(s.at + 10), j.begin() + std::ptrdiff_t(s.at + s.len));
  if (t.size() < 8 || t[0] != 'I' || t[1] != 'I' || r16(t, 2) != 42) return false;
  if (!walk(t, r32(t, 4), e.i0)) return false;
  if (e.i0.count(0x8769) && !walk(t, r32(e.i0[0x8769].v, 0), e.ex)) return false;
  if (e.i0.count(0x8825)) {
    e.haveGpsPtr = true;
    if (!walk(t, r32(e.i0[0x8825].v, 0), e.gps)) return false;
  }
  return true;
}
static bool exifOf(const Bytes& j, Exif& e) {
  for (const auto& s : leadSegments(j))
    if (parseExifSegment(j, s, e)) return true;
  return false;
}
// any Exif block anywhere in the file (the gain-map JPEG of an Ultra HDR file is a second stream) that has a GPS IFD pointer
static bool anyGps(const Bytes& f) {
  static const unsigned char pat[10] = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
  for (std::size_t i = 0; i + 12 < f.size(); ++i) {
    if (f[i] != 0xFF || f[i + 1] != 0xE1 || std::memcmp(&f[i + 4], pat + 4, 6) != 0) continue;
    const std::size_t len = 2 + ((std::size_t(f[i + 2]) << 8) | f[i + 3]);
    const Bytes one(f.begin() + std::ptrdiff_t(i), f.begin() + std::ptrdiff_t(std::min(f.size(), i + len)));
    const Seg s{0xE1, 0, one.size()};
    Exif e;
    if (parseExifSegment(one, s, e) && e.haveGpsPtr) return true;
  }
  return false;
}
// every XMP packet of the file (APP1 "http://ns.adobe.com/xap/1.0/") as text
static std::string allXmp(const Bytes& f) {
  std::string all;
  const std::string hdr = "http://ns.adobe.com/xap/1.0/";
  for (std::size_t i = 0; i + 4 + hdr.size() < f.size(); ++i) {
    if (f[i] != 0xFF || f[i + 1] != 0xE1 || std::memcmp(&f[i + 4], hdr.data(), hdr.size()) != 0) continue;
    const std::size_t len = 2 + ((std::size_t(f[i + 2]) << 8) | f[i + 3]);
    all.append(reinterpret_cast<const char*>(&f[i + 4]), std::min(len - 2, f.size() - i - 4));
  }
  return all;
}
static bool lacksLocation(const std::string& xmp) {
  std::string l = xmp;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* w : {"gps", "geo:", "latitude", "longitude", "location", "altitude"})
    if (l.find(w) != std::string::npos) return false;
  return true;
}

static ExportMeta fullMeta(bool gps) {
  ExportMeta m;
  m.make = "SONY";
  m.model = "ILCE-7RM5";
  m.lens = "FE 24-70mm F2.8 GM II";
  m.software = "Dapple 0.1.0";
  m.iso = 400;
  m.shutterSeconds = 1.f / 250.f;
  m.aperture = 2.8f;
  m.focalLengthMm = 35.f;
  m.dateTimeOriginal = "2026:03:14 09:26:53";
  m.subSec = "127";
  m.offsetTime = "+08:00";
  m.width = 2000;
  m.height = 1500;
  if (gps) {
    m.hasGps = true;
    m.latitude = 31.2304;
    m.longitude = -121.4737;
    m.hasAltitude = true;
    m.altitude = 12.5;
    m.hasGpsTime = true;
    m.gpsTime[0] = 1, m.gpsTime[1] = 26, m.gpsTime[2] = 53;
    m.gpsDate = "2026:03:14";
  }
  return m;
}

static Image8 scene(int w, int h) {
  Image8 im;
  im.width = w;
  im.height = h;
  im.data.resize(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = &im.data[3 * (std::size_t(y) * w + x)];
      p[0] = std::uint8_t((x * 255) / (w - 1));
      p[1] = std::uint8_t((y * 255) / (h - 1));
      p[2] = std::uint8_t(((x / 16 + y / 16) & 1) * 90 + 60);
    }
  return im;
}
static JpegOptions optionsFor(const ExportMeta& m) {
  JpegOptions o;
  o.exifSegment = buildExifSegment(m);
  o.iccSegment = buildIccSegment(IccSpace::Srgb);
  return o;
}
static bool decodePx(const Bytes& j, int& w, int& h, std::vector<std::uint8_t>& px) {
  int c = 0;
  std::uint8_t* p = stbi_load_from_memory(j.data(), int(j.size()), &w, &h, &c, 3);
  if (!p) return false;
  px.assign(p, p + std::size_t(w) * h * 3);
  stbi_image_free(p);
  return true;
}
// the stream without its Exif and ICC segments
static Bytes withoutMeta(const Bytes& j) {
  Bytes out(j.begin(), j.begin() + 2);
  std::size_t p = 2;
  for (const auto& s : leadSegments(j)) {
    const bool exif = s.marker == 0xE1 && std::memcmp(&j[s.at + 4], "Exif\0\0", 6) == 0;
    const bool icc = s.marker == 0xE2 && std::memcmp(&j[s.at + 4], "ICC_PROFILE", 11) == 0;
    if (!exif && !icc) out.insert(out.end(), j.begin() + std::ptrdiff_t(s.at), j.begin() + std::ptrdiff_t(s.at + s.len));
    p = s.at + s.len;
  }
  out.insert(out.end(), j.begin() + std::ptrdiff_t(p), j.end());
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
static void testIcc() {
  for (IccSpace sp : {IccSpace::Srgb, IccSpace::DisplayP3}) {
    const Bytes p = buildIccProfile(sp);
    std::printf("  profile %s: %zu bytes\n", sp == IccSpace::Srgb ? "sRGB" : "P3", p.size());
    CHECK(p.size() >= 400 && p.size() <= 600);
    CHECK(((std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) == p.size());
    CHECK(std::memcmp(&p[36], "acsp", 4) == 0 && p[8] == 4);
    color::SourceSpace sps;
    std::string why;
    CHECK(color::spaceFromIcc(p.data(), p.size(), sps, why));
    if (sp == IccSpace::Srgb) {
      // the engine's own sRGB: matrix and curve of the parsed profile equal the historic ones
      const color::Mat3 ref = color::srgbToProPhoto();
      float worst = 0;
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) worst = std::max(worst, std::fabs(sps.toProPhoto.m[i][j] - ref.m[i][j]));
      std::printf("  sRGB matrix vs engine: max diff %.5f\n", double(worst));
      CHECK(worst < 2e-3f);
      for (float x : {0.02f, 0.04f, 0.1f, 0.5f, 0.9f}) {
        const float a = sps.trc[0].eval(x), b = uhdr::srgbToLinear(x);
        CHECK(std::fabs(a - b) < 2e-4f);
      }
    } else {
      // P3 primaries: pure P3 green (0,1,0) is outside sRGB (negative red there)
      const color::Mat3 s = color::srgbToProPhoto();
      float worst = 0;
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) worst = std::max(worst, std::fabs(sps.toProPhoto.m[i][j] - s.m[i][j]));
      CHECK(worst > 0.05f);
    }
  }
  const Bytes seg = buildIccSegment(IccSpace::Srgb);
  CHECK(seg[0] == 0xFF && seg[1] == 0xE2 && std::memcmp(&seg[4], "ICC_PROFILE\0", 12) == 0 && seg[16] == 1 && seg[17] == 1);
  CHECK(buildIccProfile(IccSpace::Srgb) == buildIccProfile(IccSpace::Srgb));  // reproducible
}

static void testExif() {
  const ExportMeta m = fullMeta(true);
  const Bytes seg = buildExifSegment(m);
  CHECK(seg.size() > 100 && seg.size() < 65000 && seg[0] == 0xFF && seg[1] == 0xE1);
  CHECK(((std::size_t(seg[2]) << 8) | seg[3]) + 2 == seg.size());
  Bytes j = {0xFF, 0xD8};
  j.insert(j.end(), seg.begin(), seg.end());
  j.push_back(0xFF);
  j.push_back(0xD9);
  Exif e;
  CHECK(exifOf(j, e));
  CHECK(ascii(e.i0[0x010F]) == "SONY" && ascii(e.i0[0x0110]) == "ILCE-7RM5");
  CHECK(r16(e.i0[0x0112].v, 0) == 1);  // orientation: normal
  CHECK(ascii(e.i0[0x0131]) == "Dapple 0.1.0");
  CHECK(ascii(e.i0[0x0132]) == "2026:03:14 09:26:53");
  CHECK(e.ex[0x829A].v.size() == 8 && r32(e.ex[0x829A].v, 0) == 1 && r32(e.ex[0x829A].v, 4) == 250);  // 1/250 s
  CHECK(std::fabs(rat(e.ex[0x829D]) - 2.8) < 1e-6);
  CHECK(r16(e.ex[0x8827].v, 0) == 400);
  CHECK(std::fabs(rat(e.ex[0x920A]) - 35.0) < 1e-6);
  CHECK(ascii(e.ex[0x9003]) == "2026:03:14 09:26:53" && ascii(e.ex[0x9004]) == "2026:03:14 09:26:53");
  CHECK(ascii(e.ex[0x9291]) == "127" && ascii(e.ex[0x9011]) == "+08:00");
  CHECK(ascii(e.ex[0xA434]) == "FE 24-70mm F2.8 GM II");
  CHECK(r32(e.ex[0xA002].v, 0) == 2000 && r32(e.ex[0xA003].v, 0) == 1500);
  CHECK(r16(e.ex[0xA001].v, 0) == 1);
  CHECK(e.haveGpsPtr);
  CHECK(ascii(e.gps[1]) == "N" && ascii(e.gps[3]) == "W");
  CHECK(std::fabs(rat(e.gps[2], 0) + rat(e.gps[2], 1) / 60 + rat(e.gps[2], 2) / 3600 - 31.2304) < 1e-5);
  CHECK(std::fabs(rat(e.gps[4], 0) + rat(e.gps[4], 1) / 60 + rat(e.gps[4], 2) / 3600 - 121.4737) < 1e-5);
  CHECK(e.gps[5].v[0] == 0 && std::fabs(rat(e.gps[6]) - 12.5) < 1e-6);
  CHECK(ascii(e.gps[0x1D]) == "2026:03:14");
  // the same without a fix: no GPS pointer, no GPS IFD
  ExportMeta n = fullMeta(false);
  n.hasGps = false;
  Bytes j2 = {0xFF, 0xD8};
  const Bytes s2 = buildExifSegment(n);
  j2.insert(j2.end(), s2.begin(), s2.end());
  Exif e2;
  CHECK(exifOf(j2, e2) && !e2.haveGpsPtr && e2.gps.empty());
  // unknown fields are left out, not written as zeros
  ExportMeta bare;
  bare.software = "x";
  Bytes j3 = {0xFF, 0xD8};
  const Bytes s3 = buildExifSegment(bare);
  j3.insert(j3.end(), s3.begin(), s3.end());
  Exif e3;
  CHECK(exifOf(j3, e3) && !e3.i0.count(0x010F) && !e3.ex.count(0x829A) && !e3.ex.count(0x8827) && !e3.ex.count(0x9003) && r16(e3.i0[0x0112].v, 0) == 1);
}

static void testJpeg() {
  // small image: single stb encode
  const Image8 small = scene(320, 240);
  const ExportMeta m = fullMeta(true);
  auto bare = encodeJpeg(small);
  auto with = encodeJpeg(small, optionsFor(m));
  CHECK(bare && with);
  if (!bare || !with) return;
  const auto segs = leadSegments(*with);
  CHECK(segs.size() >= 5 && segs[0].marker == 0xE1 && segs[1].marker == 0xE0 && segs[2].marker == 0xE2 && segs[3].marker == 0xDB);
  CHECK(withoutMeta(*with) == *bare);  // only the two segments were added
  Exif e;
  CHECK(exifOf(*with, e));
  int w, h, w2, h2;
  std::vector<std::uint8_t> a, b;
  CHECK(decodePx(*with, w, h, a) && decodePx(*bare, w2, h2, b) && w == w2 && h == h2 && a == b);
  // the same options give the same bytes
  auto again = encodeJpeg(small, optionsFor(m));
  CHECK(again && *again == *with);
}

static void testStrip() {
  // threaded strip path: >= 2 MP and >= 1024 rows
  const Image8 big = scene(2100, 1100);
  const ExportMeta m = fullMeta(true);
  auto bare = encodeJpeg(big);
  auto with = encodeJpeg(big, optionsFor(m));
  CHECK(bare && with);
  if (!bare || !with) return;
  // restart markers: a DRI segment, and 3 strips (1100 / 512) = RST0, RST1 in the entropy data
  const Bytes& j = *with;
  std::size_t dri = 0;
  for (const auto& s : leadSegments(j))
    if (s.marker == 0xDD) dri = s.at;
  CHECK(dri != 0);
  const unsigned interval = dri ? (unsigned(j[dri + 4]) << 8) | j[dri + 5] : 0;
  std::size_t sof = 0;
  for (const auto& sg : leadSegments(j))
    if (sg.marker == 0xC0) sof = sg.at;
  CHECK(sof != 0);
  const int mcu = sof && j[sof + 11] == 0x22 ? 16 : 8;  // quality 92 is 4:4:4 in stb, lower qualities subsample
  CHECK(interval == unsigned(((2100 + mcu - 1) / mcu) * (512 / mcu)));
  int rst[8] = {0};
  for (std::size_t i = 2; i + 1 < j.size(); ++i)
    if (j[i] == 0xFF && j[i + 1] >= 0xD0 && j[i + 1] <= 0xD7) ++rst[j[i + 1] - 0xD0];
  CHECK(rst[0] == 1 && rst[1] == 1 && rst[2] == 0);
  CHECK(withoutMeta(j) == *bare);
  int w, h, w2, h2;
  std::vector<std::uint8_t> a, b;
  CHECK(decodePx(j, w, h, a) && decodePx(*bare, w2, h2, b) && w == 2100 && h == 1100 && a == b);
  Exif e;
  CHECK(exifOf(j, e) && e.haveGpsPtr);
  // single-threaded encode of the same image has the same pixels (restarts only change the bytes)
  CHECK(std::fabs(double(j.size()) - double(bare->size())) > 0);
}

static Bytes sdrBaseAndGain(const Image8& sdr, const JpegOptions& jo, Bytes& baseOut) {
  auto base = encodeJpeg(sdr, jo);
  baseOut = base ? *base : Bytes();
  uhdr::GainMap gm;
  gm.width = sdr.width / 4;
  gm.height = sdr.height / 4;
  gm.data.assign(std::size_t(gm.width) * gm.height, 128);
  gm.meta.gainMin = 0.f;
  gm.meta.gainMax = 2.f;
  gm.meta.capacityMin = 0.f;
  gm.meta.capacityMax = 2.f;
  auto file = uhdr::assemble(baseOut, gm);
  return file ? *file : Bytes();
}

static void testUhdr() {
  const Image8 sdr = scene(640, 480);
  for (bool gps : {true, false}) {
    ExportMeta m = fullMeta(true);
    if (!gps) m.hasGps = false;
    const JpegOptions jo = optionsFor(m);
    Bytes base;
    const Bytes file = sdrBaseAndGain(sdr, jo, base);
    CHECK(!file.empty());
    auto p = uhdr::parse(file);
    CHECK(p.has_value());
    if (!p) continue;
    CHECK(p->base == base);  // the Ultra HDR base is the plain export, byte for byte, metadata included
    auto plain = encodeJpeg(sdr, jo);
    CHECK(plain && p->base == *plain);
    Exif e;
    CHECK(exifOf(file, e));
    CHECK(e.haveGpsPtr == gps);
    CHECK(anyGps(file) == gps);
    CHECK(lacksLocation(allXmp(file)));
    // the order: Exif, JFIF, then the Ultra HDR XMP / ISO / MPF, then the ICC profile
    const auto segs = leadSegments(file);
    std::size_t iccAt = 0, mpfAt = 0;
    for (std::size_t i = 0; i < segs.size(); ++i) {
      if (segs[i].marker == 0xE2 && std::memcmp(&file[segs[i].at + 4], "ICC_PROFILE", 11) == 0) iccAt = i;
      if (segs[i].marker == 0xE2 && std::memcmp(&file[segs[i].at + 4], "MPF", 3) == 0) mpfAt = i;
    }
    CHECK(segs[0].marker == 0xE1 && iccAt > mpfAt && mpfAt > 0);
    int w, h;
    std::vector<std::uint8_t> px;
    CHECK(decodePx(file, w, h, px) && w == 640 && h == 480);
  }
}

static void testMotion() {
  const fs::path clip = root() / "tests" / "data" / "video" / "sdr_h264_420_8.mp4";
  if (!fs::exists(clip)) {
    std::printf("  SKIP no %s\n", clip.string().c_str());
    std::exit(77);
  }
  const Bytes mp4 = readAll(clip);
  const Image8 sdr = scene(640, 480);
  for (bool gps : {true, false}) {
    ExportMeta m = fullMeta(true);
    m.hasGps = gps;
    const Bytes jpeg = *encodeJpeg(sdr, optionsFor(m));
    auto f = buildMotionPhoto(jpeg, mp4);
    CHECK(f.has_value());
    if (!f) continue;
    CHECK(anyGps(*f) == gps);
    CHECK(lacksLocation(allXmp(*f)));
    Exif e;
    CHECK(exifOf(*f, e) && e.haveGpsPtr == gps && ascii(e.i0[0x0110]) == "ILCE-7RM5");
    const auto segs = leadSegments(*f);
    bool icc = false;
    for (const auto& s : segs) icc = icc || (s.marker == 0xE2 && std::memcmp(&(*f)[s.at + 4], "ICC_PROFILE", 11) == 0);
    CHECK(icc);
    // the container is still valid
    const fs::path out = root() / "build" / "export-meta" / (gps ? "motion_gps.jpg" : "motion_nogps.jpg");
    writeAll(out, *f);
    const auto info = validateMotionPhotoFile(out);
    CHECK(info.has_value() && info->valid);
    if (info && !info->valid)
      for (const auto& pr : info->problems) std::printf("  problem: %s\n", pr.c_str());
  }
}

static void testReader() {
  // JPEG round trip through the reader
  const fs::path dir = root() / "build" / "export-meta";
  const ExportMeta m = fullMeta(true);
  const fs::path jp = dir / "reader.jpg";
  writeAll(jp, *encodeJpeg(scene(64, 48), optionsFor(m)));
  ExportMeta r;
  CHECK(readSourceExif(jp, r));
  CHECK(r.make == "SONY" && r.model == "ILCE-7RM5" && r.lens == m.lens);
  CHECK(r.dateTimeOriginal == m.dateTimeOriginal && r.subSec == "127" && r.offsetTime == "+08:00");
  CHECK(std::fabs(r.shutterSeconds - 1.f / 250.f) < 1e-6f && std::fabs(r.aperture - 2.8f) < 1e-4f && r.iso == 400.f && r.focalLengthMm == 35.f);
  CHECK(r.hasGps && std::fabs(r.latitude - 31.2304) < 1e-5 && std::fabs(r.longitude + 121.4737) < 1e-5 && r.hasAltitude && std::fabs(r.altitude - 12.5) < 1e-6);
  CHECK(r.hasGpsTime && r.gpsDate == "2026:03:14");
  // PNG eXIf
  {
    const Bytes seg = buildExifSegment(m);
    Bytes tiff(seg.begin() + 10, seg.end());
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 'I', 'H', 'D', 'R', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // IHDR (13 bytes) placeholder length then an eXIf chunk (CRC not checked by the reader)
    png.assign({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0, 0, 0, 0});
    const std::uint32_t n = std::uint32_t(tiff.size());
    png.insert(png.end(), {std::uint8_t(n >> 24), std::uint8_t(n >> 16), std::uint8_t(n >> 8), std::uint8_t(n), 'e', 'X', 'I', 'f'});
    png.insert(png.end(), tiff.begin(), tiff.end());
    png.insert(png.end(), {0, 0, 0, 0});
    png.insert(png.end(), {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0});
    writeAll(dir / "reader.png", png);
    ExportMeta rp;
    CHECK(readSourceExif(dir / "reader.png", rp) && rp.hasGps && rp.model == "ILCE-7RM5");
    // HEIC-like: an ftyp box, then the Exif item "offset, Exif\0\0, TIFF" somewhere in the data
    Bytes heic = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0, 'm', 'i', 'f', '1', 'h', 'e', 'i', 'c'};
    heic.insert(heic.end(), 1000, 0x55);
    heic.insert(heic.end(), {0, 0, 0, 6, 'E', 'x', 'i', 'f', 0, 0});
    heic.insert(heic.end(), tiff.begin(), tiff.end());
    heic.insert(heic.end(), 100, 0x33);
    writeAll(dir / "reader.heic", heic);
    ExportMeta rh;
    CHECK(readSourceExif(dir / "reader.heic", rh) && rh.hasGps && rh.make == "SONY");
  }
  // no fix (0, 0) and no Exif: nothing
  ExportMeta z = fullMeta(true);
  z.latitude = z.longitude = 0;
  writeAll(dir / "zero.jpg", *encodeJpeg(scene(64, 48), optionsFor(z)));
  ExportMeta rz;
  CHECK(readSourceExif(dir / "zero.jpg", rz) && !rz.hasGps);
  writeAll(dir / "plain.jpg", *encodeJpeg(scene(64, 48)));
  ExportMeta rn;
  CHECK(!readSourceExif(dir / "plain.jpg", rn));
  CHECK(!readSourceExif(dir / "missing.jpg", rn));
  // RAW samples: TIFF-based files carry make / model / exposure
  int found = 0;
  const fs::path raws = root() / "build" / "rawsamples";
  for (const char* rel : {"Apple/iPhone-12-Pro/IMG_1361.DNG", "Google/Pixel-6-Pro/PXL_20220910_093206982.dng", "Sony/ILCE-6700/DSC00002.ARW",
                          "Sony/ILCE-7M4/ILCE-7M4_DSC06674_FullFrame-LossLess-Compressed-Large.ARW"}) {
    if (!fs::exists(raws / rel)) continue;
    ExportMeta x;
    const bool ok = readSourceExif(raws / rel, x);
    std::printf("  %s: %d make=%s model=%s iso=%.0f t=%.5f f=%.1f focal=%.1f date=%s sub=%s off=%s gps=%d\n", rel, ok, x.make.c_str(), x.model.c_str(), double(x.iso),
                double(x.shutterSeconds), double(x.aperture), double(x.focalLengthMm), x.dateTimeOriginal.c_str(), x.subSec.c_str(), x.offsetTime.c_str(), x.hasGps);
    CHECK(ok && !x.make.empty() && !x.model.empty() && x.iso > 0 && x.shutterSeconds > 0 && x.aperture > 0 && x.dateTimeOriginal.size() == 19);
    ++found;
  }
  if (!found) std::printf("  note: no build/rawsamples, RAW reader cases skipped\n");
}

static void testSession() {
  const fs::path f = root() / "build" / "rawsamples" / "Canon/EOS-R6-Mark-II/canon-r6m2-img_0346-raw-normal-full.cr3";
  if (!fs::exists(f)) {
    std::printf("  SKIP no build/rawsamples\n");
    std::exit(77);
  }
  const fs::path dir = root() / "build" / "export-meta";
  fs::create_directories(dir);
  // a "source" with a fix: the import path (HEIC / PNG conversions) hands the original file as metaSource
  const fs::path src = dir / "origin.jpg";
  writeAll(src, *encodeJpeg(scene(64, 48), optionsFor(fullMeta(true))));
  DappleSession* s = dapple_session_open(f.string().c_str(), DAPPLE_QUALITY_PREVIEW, 1600);
  CHECK(s != nullptr);
  if (!s) return;
  const std::string keep = "{\"metaSource\":\"" + src.generic_string() + "\",\"stripLocation\":false}";
  const std::string strip = "{\"metaSource\":\"" + src.generic_string() + "\",\"stripLocation\":true}";
  const std::string noKey = "{\"metaSource\":\"" + src.generic_string() + "\"}";
  auto exportTo = [&](const char* name, const std::string& opt) {
    const fs::path o = dir / name;
    CHECK(dapple_session_export_jpeg_ex(s, "{}", "", 1200, 92, o.string().c_str(), opt.c_str()) == DAPPLE_OK);
    return readAll(o);
  };
  const Bytes bare = [&] {
    const fs::path o = dir / "bare.jpg";
    CHECK(dapple_session_export_jpeg(s, "{}", "", 1200, 92, o.string().c_str()) == DAPPLE_OK);
    return readAll(o);
  }();
  CHECK(leadSegments(bare)[0].marker == 0xE0);  // the plain call stays the bare stream
  const Bytes k = exportTo("keep.jpg", keep), st = exportTo("strip.jpg", strip), nk = exportTo("nokey.jpg", noKey);
  Exif ek, es;
  CHECK(exifOf(k, ek) && exifOf(st, es));
  CHECK(ek.haveGpsPtr && !es.haveGpsPtr && !anyGps(st) && !anyGps(nk));  // missing key = strip
  CHECK(std::fabs(rat(ek.gps[2], 0) + rat(ek.gps[2], 1) / 60 + rat(ek.gps[2], 2) / 3600 - 31.2304) < 1e-5);
  CHECK(r16(es.i0[0x0112].v, 0) == 1 && ascii(es.i0[0x0131]).rfind("Dapple ", 0) == 0);
  CHECK(ascii(es.i0[0x0110]) == "ILCE-7RM5");  // the metaSource file's camera wins over the session's
  CHECK(ascii(es.ex[0x9003]) == "2026:03:14 09:26:53");
  int w, h;
  CHECK(r32(es.ex[0xA002].v, 0) > 0 && r32(es.ex[0xA003].v, 0) > 0);
  std::vector<std::uint8_t> px;
  CHECK(decodePx(st, w, h, px) && std::uint32_t(w) == r32(es.ex[0xA002].v, 0));
  CHECK(withoutMeta(st) == bare);  // metadata is the only difference
  // without metaSource the camera facts come from the session
  const Bytes sess = exportTo("session_only.jpg", "{}");
  Exif eo;
  CHECK(exifOf(sess, eo) && ascii(eo.i0[0x0110]).find("EOS") != std::string::npos && !eo.haveGpsPtr);
  // colour spaces this build cannot render
  const fs::path bad = dir / "bad.jpg";
  CHECK(dapple_session_export_jpeg_ex(s, "{}", "", 1200, 92, bad.string().c_str(), "{\"colorSpace\":\"p3\"}") == DAPPLE_ERROR);
  // Ultra HDR: the base is the _ex SDR export, keep and strip
  for (const std::string* opt : {&keep, &strip}) {
    const fs::path sdrP = dir / "u_sdr.jpg", hdrP = dir / "u_hdr.jpg";
    CHECK(dapple_session_export_jpeg_ex(s, "{\"exposureEv\":1.0}", "", 1200, 92, sdrP.string().c_str(), opt->c_str()) == DAPPLE_OK);
    DappleHdrInfo info{};
    CHECK(dapple_session_export_jpeg_hdr_ex(s, "{\"exposureEv\":1.0}", "", 1200, 92, hdrP.string().c_str(), 2.f, &info, opt->c_str()) == DAPPLE_OK);
    const Bytes sdr = readAll(sdrP), hdr = readAll(hdrP);
    const bool wantGps = opt == &keep;
    std::printf("  hdr written=%d (%s)\n", info.written, wantGps ? "keep" : "strip");
    CHECK(anyGps(hdr) == wantGps && anyGps(sdr) == wantGps && lacksLocation(allXmp(hdr)));
    if (info.written) {
      auto p = uhdr::parse(hdr);
      CHECK(p.has_value() && p->base == sdr);
    } else {
      CHECK(hdr == sdr);
    }
  }
  dapple_session_close(s);
}

int main(int argc, char** argv) {
  const std::string id = argc > 1 ? argv[1] : "";
  std::printf("export_meta %s\n", id.c_str());
  if (id == "icc") testIcc();
  else if (id == "exif") testExif();
  else if (id == "jpeg") testJpeg();
  else if (id == "strip") testStrip();
  else if (id == "uhdr") testUhdr();
  else if (id == "motion") testMotion();
  else if (id == "reader") testReader();
  else if (id == "session") testSession();
  else {
    std::printf("unknown test id\n");
    return 2;
  }
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : 0;
}
