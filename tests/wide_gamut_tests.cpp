// Display-referred sources with their own colour space and depth: explicit tags, matrix / TRC ICC profiles, 16-bit samples
// (core/src/colour_space.h, core/src/decoders/jpeg_decoder.cpp). Synthetic files only; run with ctest (name wide_gamut).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "color.h"
#include "colour_space.h"
#include "render_f.h"
#include "test_tmp.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
  do {                                                                                                 \
    ++g_checks;                                                                                        \
    const double va_ = (a), vb_ = (b);                                                                 \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                            \
      ++g_failures;                                                                                    \
      std::fprintf(stderr, "  FAIL %s:%d  %s = %.6g vs %s = %.6g (tol %g)\n", __FILE__, __LINE__, #a, va_, #b, vb_, double(tol)); \
    }                                                                                                  \
  } while (0)

std::uint32_t crc32Of(const std::string& type, const std::vector<std::uint8_t>& d) {
  static std::uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      tab[n] = c;
    }
    init = true;
  }
  std::uint32_t c = 0xFFFFFFFFu;
  for (char ch : type) c = tab[(c ^ std::uint8_t(ch)) & 0xFF] ^ (c >> 8);
  for (std::uint8_t b : d) c = tab[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(std::uint8_t(v >> s));
}
struct PngChunk {
  std::string type;
  std::vector<std::uint8_t> data;
};
std::vector<std::uint8_t> zlibStored(const std::vector<std::uint8_t>& raw) {
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::size_t pos = 0;
  do {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(std::uint8_t(n)), z.push_back(std::uint8_t(n >> 8)), z.push_back(std::uint8_t(~n)), z.push_back(std::uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + n));
    pos += n;
  } while (pos < raw.size());
  std::uint32_t a = 1, b = 0;
  for (std::uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  put32(z, (b << 16) | a);
  return z;
}
// RGB PNG, 8 or 16 bit (samples given as 16-bit codes; 8-bit takes the high byte), chunks placed before IDAT.
void writePng(const fs::path& path, int w, int h, int depth, const std::vector<std::uint16_t>& rgb, const std::vector<PngChunk>& chunks) {
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const std::string& t, const std::vector<std::uint8_t>& d) {
    put32(out, std::uint32_t(d.size()));
    out.insert(out.end(), t.begin(), t.end());
    out.insert(out.end(), d.begin(), d.end());
    put32(out, crc32Of(t, d));
  };
  std::vector<std::uint8_t> ihdr;
  put32(ihdr, std::uint32_t(w)), put32(ihdr, std::uint32_t(h));
  ihdr.insert(ihdr.end(), {std::uint8_t(depth), 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  for (const auto& c : chunks) chunk(c.type, c.data);
  std::vector<std::uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int i = 0; i < w * 3; ++i) {
      const std::uint16_t v = rgb[std::size_t(y) * w * 3 + i];
      if (depth == 16) raw.push_back(std::uint8_t(v >> 8)), raw.push_back(std::uint8_t(v));
      else raw.push_back(std::uint8_t(v >> 8));
    }
  }
  chunk("IDAT", zlibStored(raw));
  chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}
PngChunk cicp(int cp, int tc) { return {"cICP", {std::uint8_t(cp), std::uint8_t(tc), 0, 1}}; }

void s15(std::vector<std::uint8_t>& o, double v) { put32(o, std::uint32_t(std::int32_t(std::lround(v * 65536.0)))); }
// A matrix / TRC RGB ICC profile. `colorants` are the D50 columns (r, g, b); curve 0 = sRGB parametric, 1 = sRGB 1024-entry table,
// 2 = gamma 2.2 (curv), 3 = no colorants (LUT-like profile).
std::vector<std::uint8_t> makeIcc(const double colorants[3][3], int curve, bool gray = false) {
  struct Tag {
    const char* sig;
    std::vector<std::uint8_t> data;
  };
  std::vector<Tag> tags;
  auto xyz = [&](int c) {
    std::vector<std::uint8_t> d = {'X', 'Y', 'Z', ' ', 0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) s15(d, colorants[i][c]);
    return d;
  };
  std::vector<std::uint8_t> trc;
  if (curve == 0) {
    trc = {'p', 'a', 'r', 'a', 0, 0, 0, 0, 0, 3, 0, 0};
    for (double v : {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045}) s15(trc, v);
  } else if (curve == 1) {
    trc = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
    put32(trc, 1024);
    for (int i = 0; i < 1024; ++i) {
      const std::uint16_t v = std::uint16_t(std::lround(color::srgbDecode(i / 1023.f) * 65535.f));
      trc.push_back(std::uint8_t(v >> 8)), trc.push_back(std::uint8_t(v));
    }
  } else {
    trc = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
    put32(trc, 1);
    trc.push_back(2), trc.push_back(0x33);  // u8Fixed8 2.2
    trc.push_back(0), trc.push_back(0);
  }
  if (curve != 3) {
    tags.push_back({"rXYZ", xyz(0)}), tags.push_back({"gXYZ", xyz(1)}), tags.push_back({"bXYZ", xyz(2)});
    tags.push_back({"rTRC", trc}), tags.push_back({"gTRC", trc}), tags.push_back({"bTRC", trc});
  } else {
    tags.push_back({"A2B0", std::vector<std::uint8_t>(64, 0)});
  }
  std::vector<std::uint8_t> p(128, 0);
  std::memcpy(&p[16], gray ? "GRAY" : "RGB ", 4);
  std::memcpy(&p[20], "XYZ ", 4);
  std::memcpy(&p[36], "acsp", 4);
  put32(p, std::uint32_t(tags.size()));
  const std::size_t off = 128 + 4 + 12 * tags.size();
  std::vector<std::uint8_t> body;
  for (const auto& t : tags) {
    p.insert(p.end(), t.sig, t.sig + 4);
    put32(p, std::uint32_t(off + body.size())), put32(p, std::uint32_t(t.data.size()));
    body.insert(body.end(), t.data.begin(), t.data.end());
    while (body.size() % 4) body.push_back(0);
  }
  p.insert(p.end(), body.begin(), body.end());
  const std::uint32_t n = std::uint32_t(p.size());
  p[0] = std::uint8_t(n >> 24), p[1] = std::uint8_t(n >> 16), p[2] = std::uint8_t(n >> 8), p[3] = std::uint8_t(n);
  return p;
}
// rows = X, Y, Z; columns = r, g, b (D50)
const double kP3D50[3][3] = {{0.5151, 0.2920, 0.1571}, {0.2412, 0.6922, 0.0666}, {-0.0011, 0.0419, 0.7841}};
const double kSrgbD50[3][3] = {{0.4361, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7141}};

PngChunk iccpChunk(const std::vector<std::uint8_t>& icc) {
  PngChunk c{"iCCP", {'i', 'c', 'c', 0, 0}};
  const auto z = zlibStored(icc);
  c.data.insert(c.data.end(), z.begin(), z.end());
  return c;
}

SourceImage decodeFile(const fs::path& p) {
  auto r = makeJpegDecoder()->decode(p, DecodeOptions{});
  if (!r) {
    std::fprintf(stderr, "decode failed\n");
    std::abort();
  }
  return std::move(*r);
}
void toSrgbLinear(const float* pro, float* srgb) { color::proPhotoToSrgb().apply(pro, srgb); }

void testParser() {
  color::SourceSpace s;
  std::string why;
  auto p3 = makeIcc(kP3D50, 0);
  CHECK(color::spaceFromIcc(p3.data(), p3.size(), s, why));
  CHECK(s.name == "Display P3" && !s.isSrgb && !s.fallback);
  CHECK(s.note.find("Display P3") != std::string::npos);
  auto t = makeIcc(kSrgbD50, 1);
  CHECK(color::spaceFromIcc(t.data(), t.size(), s, why));
  CHECK(s.isSrgb && s.note.empty());  // an sRGB profile is the historic sRGB path
  t = makeIcc(kP3D50, 2);
  CHECK(color::spaceFromIcc(t.data(), t.size(), s, why));
  CHECK(s.name == "Display P3" && s.trc[0].kind == color::Trc::Gamma);
  color::ColourTags tg;
  tg.icc = makeIcc(kSrgbD50, 3);  // LUT profile: not a matrix / TRC one
  s = color::resolveSourceSpace(tg);
  CHECK(s.fallback && s.isSrgb && s.note.find("unknown colour space") != std::string::npos);
  tg.icc = makeIcc(kSrgbD50, 0, true);  // gray profile
  CHECK(color::resolveSourceSpace(tg).fallback);
  tg.icc.assign(40, 7);  // garbage
  CHECK(color::resolveSourceSpace(tg).fallback);
  color::ColourTags c2;
  c2.hasCicp = true, c2.cicpPrimaries = 12, c2.cicpTransfer = 13;
  CHECK(color::resolveSourceSpace(c2).name == "Display P3");
  c2.cicpPrimaries = 9, c2.cicpTransfer = 1;
  CHECK(color::resolveSourceSpace(c2).name == "Rec.2020");
  c2.cicpPrimaries = 1, c2.cicpTransfer = 13;
  CHECK(color::resolveSourceSpace(c2).isSrgb);
  c2.cicpTransfer = 16;  // PQ
  CHECK(color::resolveSourceSpace(c2).fallback);
  c2.cicpTransfer = 13, c2.cicpPrimaries = 99;
  CHECK(color::resolveSourceSpace(c2).fallback);
  color::ColourTags c3;  // Adobe RGB: cHRM + gAMA 1/2.2
  const double adobe[8] = {0.3127, 0.3290, 0.640, 0.330, 0.210, 0.710, 0.150, 0.060};
  c3.hasChrm = true, c3.gamma = 1 / 2.2;
  std::copy(adobe, adobe + 8, c3.chrm);
  const auto a = color::resolveSourceSpace(c3);
  CHECK(a.name == "Adobe RGB (1998)" && a.trc[0].kind == color::Trc::Gamma);
  color::ColourTags c4;  // the usual sRGB fallback pair stays sRGB
  const double sr[8] = {0.3127, 0.3290, 0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
  c4.hasChrm = true, c4.gamma = 0.45455;
  std::copy(sr, sr + 8, c4.chrm);
  CHECK(color::resolveSourceSpace(c4).isSrgb);
  color::ColourTags c5;
  c5.gamma = 0.45455;  // gAMA alone: as before
  CHECK(color::resolveSourceSpace(c5).isSrgb);
}

void testP3Patch(const fs::path& dir) {
  const std::vector<std::uint16_t> px = {65535, 0, 0, 65535, 32768, 0, 0, 65535, 0, 0, 0, 65535};
  writePng(dir / "p3_cicp.png", 4, 1, 16, px, {cicp(12, 13)});
  writePng(dir / "p3_icc.png", 4, 1, 16, px, {iccpChunk(makeIcc(kP3D50, 0))});
  writePng(dir / "untagged.png", 4, 1, 16, px, {});
  const SourceImage a = decodeFile(dir / "p3_cicp.png"), b = decodeFile(dir / "p3_icc.png"), u = decodeFile(dir / "untagged.png");
  CHECK(a.meta.bitsPerSample == 16);
  float s[3];
  toSrgbLinear(a.linear.row(0), s);
  CHECK(s[0] > 1.2f && s[0] < 1.25f);  // P3 red = (1.2249, -0.0420, -0.0196) in linear sRGB
  CHECK(s[1] < -0.03f && s[1] > -0.05f);
  CHECK(s[2] < -0.01f);
  toSrgbLinear(a.linear.row(0) + 9, s);  // blue patch
  CHECK(s[2] > 1.0f);
  for (int i = 0; i < 12; ++i) CHECK_NEAR(a.linear.row(0)[i], b.linear.row(0)[i], 4e-3);
  toSrgbLinear(u.linear.row(0), s);  // untagged = sRGB: red is exactly in gamut
  CHECK_NEAR(s[0], 1.0, 1e-3);
  CHECK_NEAR(s[1], 0.0, 1e-3);
  for (int i = 0; i < 12; ++i) {
    if (!(a.linear.row(0)[i] > -3e-3f)) std::fprintf(stderr, "  value %d = %g\n", i, a.linear.row(0)[i]);
    CHECK(a.linear.row(0)[i] > -3e-3f);
  }  // nothing clips on the way into the working space
}

void testSrgbPaths(const fs::path& dir) {
  const int W = 256;
  std::vector<std::uint16_t> px;
  for (int x = 0; x < W; ++x) {
    const std::uint16_t v = std::uint16_t(x * 257);
    px.insert(px.end(), {v, std::uint16_t((x * 7 % 256) * 257), std::uint16_t(255 * 257 - v)});
  }
  writePng(dir / "s8.png", W, 1, 8, px, {});
  writePng(dir / "s8_tagged.png", W, 1, 8, px, {{"sRGB", {0}}, {"gAMA", {0, 0, 0xB1, 0x8F}}, iccpChunk(makeIcc(kSrgbD50, 1))});
  writePng(dir / "s16.png", W, 1, 16, px, {{"sRGB", {0}}});
  const SourceImage a = decodeFile(dir / "s8.png"), t = decodeFile(dir / "s8_tagged.png"), d = decodeFile(dir / "s16.png");
  bool same = true, sameTag = true;
  double worst = 0;
  for (int x = 0; x < W; ++x) {
    const float lin[3] = {color::srgbDecode(px[3 * x] / 257 / 255.f), color::srgbDecode(px[3 * x + 1] / 257 / 255.f), color::srgbDecode(px[3 * x + 2] / 257 / 255.f)};
    float ref[3];
    color::srgbToProPhoto().apply(lin, ref);  // what the decoder did before colour spaces existed
    for (int c = 0; c < 3; ++c) {
      same = same && a.linear.row(0)[3 * x + c] == ref[c];
      sameTag = sameTag && t.linear.row(0)[3 * x + c] == ref[c];
      worst = std::max(worst, double(std::fabs(d.linear.row(0)[3 * x + c] - ref[c])));
    }
  }
  CHECK(same);
  CHECK(sameTag);
  CHECK(worst < 1e-5);
}

void testFallback(const fs::path& dir) {
  const std::vector<std::uint16_t> px = {40000, 20000, 10000};
  writePng(dir / "lut.png", 1, 1, 16, px, {iccpChunk(makeIcc(kSrgbD50, 3))});
  writePng(dir / "plain.png", 1, 1, 16, px, {});
  const SourceImage a = decodeFile(dir / "lut.png"), b = decodeFile(dir / "plain.png");
  for (int i = 0; i < 3; ++i) CHECK(a.linear.row(0)[i] == b.linear.row(0)[i]);
  color::ColourTags tg;
  tg.icc = makeIcc(kSrgbD50, 3);
  const auto s = color::resolveSourceSpace(tg);
  CHECK(s.fallback && !s.note.empty() && s.note.rfind("dapple: unknown colour space", 0) == 0);  // the line the decoder logs
}

void testJpegIcc(const fs::path& dir) {
  Image8 img;
  img.width = 8, img.height = 8;
  img.data.assign(8 * 8 * 3, 0);
  for (int i = 0; i < 64; ++i) img.data[std::size_t(3 * i)] = 255;  // pure red
  auto jp = encodeJpeg(img);
  CHECK(bool(jp));
  if (!jp) return;
  const auto icc = makeIcc(kP3D50, 0);
  std::vector<std::uint8_t> seg = {0xFF, 0xE2};
  const std::size_t len = 2 + 12 + 2 + icc.size();
  seg.push_back(std::uint8_t(len >> 8)), seg.push_back(std::uint8_t(len));
  const char sig[] = "ICC_PROFILE";
  seg.insert(seg.end(), sig, sig + 12);
  seg.push_back(1), seg.push_back(1);
  seg.insert(seg.end(), icc.begin(), icc.end());
  std::vector<std::uint8_t> out(jp->begin(), jp->begin() + 2);
  out.insert(out.end(), seg.begin(), seg.end());
  out.insert(out.end(), jp->begin() + 2, jp->end());
  std::ofstream(dir / "p3.jpg", std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  std::ofstream(dir / "plain.jpg", std::ios::binary).write(reinterpret_cast<const char*>(jp->data()), std::streamsize(jp->size()));
  const SourceImage a = decodeFile(dir / "p3.jpg"), b = decodeFile(dir / "plain.jpg");
  float sa[3], sb[3];
  toSrgbLinear(a.linear.row(4) + 12, sa);
  toSrgbLinear(b.linear.row(4) + 12, sb);
  CHECK(sa[0] > 1.1f && sa[1] < -0.02f);   // a P3 red
  CHECK(sb[0] < 1.02f && sb[1] > -0.02f);  // the same pixels untagged are sRGB
}

void testBanding(const fs::path& dir) {
  const int W = 2048;
  std::vector<std::uint16_t> g16, g8;
  for (int x = 0; x < W; ++x) {
    const int code = 13000 + int(std::lround(double(x) / (W - 1) * 6500.0));  // ~0.20 .. 0.30 of full scale
    const std::uint16_t v8 = std::uint16_t(((code + 128) / 257) * 257);
    g16.insert(g16.end(), {std::uint16_t(code), std::uint16_t(code), std::uint16_t(code)});
    g8.insert(g8.end(), {v8, v8, v8});
  }
  writePng(dir / "g16.png", W, 1, 16, g16, {cicp(12, 13)});
  writePng(dir / "g8.png", W, 1, 8, g8, {cicp(12, 13)});
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  EditParams strong;
  strong.contrast = 80.f;
  strong.exposure = 0.7f;
  auto levels = [&](const char* name) {
    const ImageF o = renderF(strong, cam, nullptr, decodeFile(dir / name));
    std::set<int> s;
    for (int x = 0; x < o.width; ++x) s.insert(int(std::lround(o.row(0)[3 * x + 1] * 4095.f)));
    return int(s.size());
  };
  const int n16 = levels("g16.png"), n8 = levels("g8.png");
  std::printf("  gradient levels after a strong edit (12-bit quantised): 16-bit %d, 8-bit %d\n", n16, n8);
  CHECK(n8 <= 40);
  CHECK(n16 >= 6 * n8);
  CHECK(n16 >= 300);
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / ("dapple_wide_" + testUniqueTag());
  fs::create_directories(dir);
  struct T {
    const char* name;
    void (*fn)(const fs::path&);
  };
  const T tests[] = {{"profile parser", [](const fs::path&) { testParser(); }}, {"P3 patch", testP3Patch}, {"sRGB paths", testSrgbPaths},
                     {"unknown profile fallback", testFallback}, {"JPEG ICC", testJpegIcc}, {"16-bit banding", testBanding}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn(dir);
    std::printf("[%s] %s\n", g_failures == before ? " ok " : "FAIL", t.name);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
