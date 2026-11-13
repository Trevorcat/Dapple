// Ultra HDR (gain-map JPEG) tests: container structure, gain map maths, the session export (docs/engine/pipeline-architecture.md section 34).
// The C API is compiled in (as in film_tests). `dapple_uhdr_tests evidence <outDir> <raw>...` writes the export, the gain-map PNG, a side-by-side PNG and the
// metadata text of each file; `explore <raw>...` prints the headroom decision (neither is a ctest).

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dapple/dapple_c.h"
#include "dapple/encode.h"
#include "dapple/ultrahdr.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::uhdr;

static int g_fail = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                  \
      ++g_fail;                                                                   \
    }                                                                             \
  } while (0)

static Bytes readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// decoded JPEG (any component count) as 8-bit with `want` channels
struct Dec {
  int w = 0, h = 0, comps = 0;
  std::vector<std::uint8_t> px;
};
static bool decode(const Bytes& j, int want, Dec& d) {
  int c = 0;
  if (!stbi_info_from_memory(j.data(), int(j.size()), &d.w, &d.h, &c)) return false;
  d.comps = c;
  std::uint8_t* p = stbi_load_from_memory(j.data(), int(j.size()), &d.w, &d.h, &c, want);
  if (!p) return false;
  d.px.assign(p, p + std::size_t(d.w) * d.h * want);
  stbi_image_free(p);
  return true;
}

// ---- synthetic scene: linear scene value x(px) in SDR-white units (a sky ramp with a sun), rendered like the SDR path does ----
static float shoulderFn(float x) {
  constexpr float knee = 0.3f;
  return x <= knee ? x : knee + (1.f - knee) * (1.f - std::exp(-(x - knee) / (1.f - knee)));
}
static std::uint8_t enc8(float lin) { return std::uint8_t(std::lround(std::min(1.f, std::max(0.f, linearToSrgb(std::max(0.f, lin)))) * 255.f)); }
static void synth(int w, int h, float H, Image8& sdr, Image8& bright) {
  sdr.width = bright.width = w;
  sdr.height = bright.height = h;
  sdr.data.resize(std::size_t(w) * h * 3);
  bright.data.resize(sdr.data.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      const float dx = u - 0.7f, dy = v - 0.3f;
      float s = 0.05f + 0.9f * u * u + 6.f * std::exp(-(dx * dx + dy * dy) / 0.004f);  // scene luminance
      if (u > 0.2f && u < 0.3f && v > 0.55f && v < 0.7f) s = 5.f;                      // a bright rectangle: a hard edge
      if (x > w / 2 && y > h * 3 / 4) s = 0.04f;                                       // a dark patch next to the bright part (edge)
      const float col[3] = {1.f, 0.95f, 0.8f + 0.2f * v};
      for (int c = 0; c < 3; ++c) {
        const float xc = s * col[c];
        sdr.data[3 * (std::size_t(y) * w + x) + c] = enc8(shoulderFn(xc));
        bright.data[3 * (std::size_t(y) * w + x) + c] = enc8(shoulderFn(xc / std::exp2(H)));
      }
    }
}

static float lumaLin(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

static void testContainer() {
  Image8 sdr, bright;
  synth(320, 240, 3.f, sdr, bright);
  const GainMap gm = makeGainMap(sdr, bright, 3.f);
  CHECK(gm.meta.gainMax > 1.f && gm.meta.gainMax <= 3.f + 0.01f);
  auto base = encodeJpeg(sdr);
  CHECK(base.has_value());
  auto file = assemble(*base, gm);
  CHECK(file.has_value());
  if (!base || !file) return;
  auto p = parse(*file);
  CHECK(p.has_value());
  if (!p) return;
  std::printf("  file %zu bytes: primary %zu, gain map %zu (%dx%d, %d component)\n", file->size(), p->primaryEnd, p->gainSize, p->gainWidth, p->gainHeight, p->gainComponents);
  // MPF: two entries, sizes and the absolute offset of the second image (SOI)
  CHECK(p->mpfCount == 2);
  CHECK(p->mpfPrimarySize == p->primaryEnd);
  CHECK(p->primaryEnd + p->gainSize == file->size());
  CHECK(p->gainOffset == p->primaryEnd);
  CHECK((*file)[p->gainOffset] == 0xFF && (*file)[p->gainOffset + 1] == 0xD8);
  CHECK((*file)[file->size() - 2] == 0xFF && (*file)[file->size() - 1] == 0xD9);
  // XMP of the primary: container directory, hdrgm version, the gain-map item with the right length
  CHECK(p->primaryXmp.find("hdrgm:Version=\"1.0\"") != std::string::npos);
  CHECK(p->primaryXmp.find("Container:Directory") != std::string::npos);
  CHECK(p->primaryXmp.find("Item:Semantic=\"Primary\"") != std::string::npos);
  CHECK(p->gainSemantic == "GainMap" && p->directoryLength == p->gainSize);
  // XMP of the gain map: all fields, and the values read back equal what was written
  for (const char* k : {"hdrgm:Version", "hdrgm:GainMapMin", "hdrgm:GainMapMax", "hdrgm:Gamma", "hdrgm:OffsetSDR", "hdrgm:OffsetHDR", "hdrgm:HDRCapacityMin",
                        "hdrgm:HDRCapacityMax", "hdrgm:BaseRenditionIsHDR"})
    CHECK(p->gainXmp.find(k) != std::string::npos);
  CHECK(std::fabs(p->meta.gainMax - gm.meta.gainMax) < 1e-4f && p->meta.gainMin == 0.f && p->meta.gamma == 1.f);
  CHECK(std::fabs(p->meta.offsetSdr - kOffset) < 1e-6f && std::fabs(p->meta.capacityMax - gm.meta.capacityMax) < 1e-4f);
  // ISO 21496-1: present in both images, same numbers
  CHECK(p->primaryHasIso && p->gainHasIso);
  CHECK(std::fabs(p->isoMeta.gainMax - p->meta.gainMax) < 1e-5f && std::fabs(p->isoMeta.capacityMax - p->meta.capacityMax) < 1e-5f &&
        std::fabs(p->isoMeta.offsetHdr - kOffset) < 1e-6f && p->isoMeta.gamma == 1.f);
  // the base JPEG is untouched: removing the added segments gives the input stream back
  CHECK(p->base == *base);
  // both JPEGs decode; the gain map is a single component of the stated size
  Dec b, g;
  CHECK(decode(Bytes(file->begin(), file->begin() + std::ptrdiff_t(p->primaryEnd)), 3, b) && b.w == 320 && b.h == 240);
  CHECK(decode(p->gainJpeg, 1, g) && g.w == gm.width && g.h == gm.height && g.comps == 1 && p->gainComponents == 1);
  int maxd = 0;
  if (g.px.size() == gm.data.size())
    for (std::size_t i = 0; i < gm.data.size(); ++i) maxd = std::max(maxd, std::abs(int(g.px[i]) - int(gm.data[i])));
  std::printf("  gain-map JPEG round trip: max level difference %d of 255\n", maxd);
  CHECK(maxd <= 16);  // ringing at the hard edges of the synthetic scene
  // a file whose gain-map length is wrong must not parse
  Bytes cut(file->begin(), file->end() - 10);
  CHECK(!parse(cut).has_value());
}

static void testReconstruct() {
  // full-size pair, gain map from its quarter-size box-filtered copy: what the export does
  const int W = 640, H = 480;
  Image8 sdr, bright;
  synth(W, H, 3.f, sdr, bright);
  auto box = [&](const Image8& a) {
    Image8 o;
    o.width = W / 4;
    o.height = H / 4;
    o.data.resize(std::size_t(o.width) * o.height * 3);
    for (int y = 0; y < o.height; ++y)
      for (int x = 0; x < o.width; ++x)
        for (int c = 0; c < 3; ++c) {
          float s = 0.f;
          for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) s += srgbToLinear(a.data[3 * (std::size_t(4 * y + j) * W + 4 * x + i) + c] / 255.f);
          o.data[3 * (std::size_t(y) * o.width + x) + c] = enc8(s / 16.f);
        }
    return o;
  };
  const GainMap gm = makeGainMap(box(sdr), box(bright), 3.f);
  const ImageF ref = hdrLinear(sdr, bright, 3.f);
  const ImageF rec = reconstruct(sdr, gm);
  // error: in stops of luminance over the pixels that carry gain, and the worst pixel anywhere; mid-tones (reference gain 0) must be exactly SDR
  double sum = 0, worst = 0;
  std::size_t n = 0, midBad = 0, rimN = 0;
  double rimWorst = 0;
  float peakRef = 0.f;
  for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
    const float yr = lumaLin(&ref.data[3 * i]), yc = lumaLin(&rec.data[3 * i]);
    const float ys = lumaLin(std::array<float, 3>{srgbToLinear(sdr.data[3 * i] / 255.f), srgbToLinear(sdr.data[3 * i + 1] / 255.f), srgbToLinear(sdr.data[3 * i + 2] / 255.f)}.data());
    peakRef = std::max(peakRef, yr);
    const double e = std::fabs(std::log2((yc + kOffset) / (yr + kOffset)));
    const int px = int(i % std::size_t(W)), py = int(i / std::size_t(W));
    const bool rim = px > int(0.2f * (W - 1)) - 8 && px < int(0.3f * (W - 1)) + 8 && py > int(0.55f * (H - 1)) - 8 && py < int(0.7f * (H - 1)) + 8 &&
                     !(px > int(0.2f * (W - 1)) + 8 && px < int(0.3f * (W - 1)) - 8 && py > int(0.55f * (H - 1)) + 8 && py < int(0.7f * (H - 1)) - 8);
    if (yr < ys * 1.0001f + 1e-6f && yc > ys * 1.06f + 0.002f) ++midBad;  // reference has no gain, the reconstruction lights it by > 0.08 stop (halo)
    if (rim) {
      rimWorst = std::max(rimWorst, e);
      ++rimN;
      continue;
    }
    if (yr > ys * 1.02f + 0.001f) {
      sum += e;
      ++n;
    }
    worst = std::max(worst, e);
  }
  std::printf("  reference peak %.2f, %zu pixels with gain; error in stops away from the hard edge: mean %.4f, worst %.4f; within 8 px of the hard edge (%zu px): worst %.3f; "
              "no-gain pixels lit by the up-scaled map: %zu\n", double(peakRef), n, n ? sum / double(n) : 0.0, worst, rimN, rimWorst, midBad);
  CHECK(n > 1000 && peakRef > 4.f);
  CHECK(n && sum / double(n) < 0.12);
  CHECK(worst < 0.5);
  CHECK(rimWorst < 2.5);
  CHECK(midBad * 200 < std::size_t(W) * H);  // < 0.5 % of the picture (the rim of an edge)
  // the SDR-only case: bright == SDR scaled gives a gain map of zeros and capacity 0
  Image8 flatS, flatB;
  synth(64, 48, 3.f, flatS, flatB);
  for (auto& v : flatS.data) v = std::min<std::uint8_t>(v, 120);  // nothing above mid-tones
  for (auto& v : flatB.data) v = std::min<std::uint8_t>(v, 60);
  const GainMap z = makeGainMap(flatS, flatB, 3.f);
  bool zeros = true;
  for (auto v : z.data) zeros = zeros && v == 0;
  CHECK(zeros && z.meta.gainMax == 0.f && z.meta.capacityMax == 0.f);
}

static void testRule() {
  CHECK(autoHeadroom(1.0f) == 0.f && autoHeadroom(1.49f) == 0.f);
  CHECK(autoHeadroom(1.5f) == 1.f);
  CHECK(autoHeadroom(2.0f) == 1.f);
  CHECK(autoHeadroom(2.1f) == 1.5f);
  CHECK(autoHeadroom(4.0f) == 2.f);
  CHECK(autoHeadroom(8.0f) == 3.f && autoHeadroom(100.f) == 3.f);
  CHECK(std::fabs(srgbToLinear(linearToSrgb(0.37f)) - 0.37f) < 1e-5f);
}

// ---- session level ----
static fs::path root() {
#ifdef DAPPLE_SOURCE_DIR
  return fs::path(DAPPLE_SOURCE_DIR);
#else
  return fs::current_path();
#endif
}

struct Session {
  DappleSession* s = nullptr;
  explicit Session(const fs::path& f, int longEdge) {
    const std::string u = f.string();
    s = dapple_session_open(u.c_str(), DAPPLE_QUALITY_PREVIEW, longEdge);
  }
  ~Session() {
    if (s) dapple_session_close(s);
  }
};

static fs::path sample(const char* rel) {
  const fs::path p = root() / "build" / "rawsamples" / rel;
  return fs::exists(p) ? p : fs::path();
}

// Error of the reconstruction against the HDR rendition seen at 8 x 8 pixel scale (what the eye resolves of a highlight; a glint of one or two pixels cannot
// survive a quarter-resolution map): rms and worst of log2 luminance ratio over the blocks whose reference average is above the SDR picture by 5 %, and the share
// of the extra light (sum of HDR - SDR luminance) the reconstruction carries.
struct BlockStats {
  double rms = 0, worst = 0, energy = 0;
  std::size_t blocks = 0;
};
static BlockStats blockStats(const ImageF& rec, const ImageF& ref, const Image8& sdr) {
  BlockStats r;
  const float* t = nullptr;
  (void)t;
  double s = 0, eRef = 0, eRec = 0;
  for (int by = 0; by + 8 <= rec.height; by += 8)
    for (int bx = 0; bx + 8 <= rec.width; bx += 8) {
      double yr = 0, yc = 0, ys = 0;
      for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
          const std::size_t k = std::size_t(by + j) * rec.width + bx + i;
          yr += lumaLin(&ref.data[3 * k]);
          yc += lumaLin(&rec.data[3 * k]);
          const float c[3] = {srgbToLinear(sdr.data[3 * k] / 255.f), srgbToLinear(sdr.data[3 * k + 1] / 255.f), srgbToLinear(sdr.data[3 * k + 2] / 255.f)};
          ys += lumaLin(c);
        }
      yr /= 64;
      yc /= 64;
      ys /= 64;
      if (yr < ys * 1.05 + 0.002) continue;
      const double e = std::log2((yc + kOffset) / (yr + kOffset));
      s += e * e;
      r.worst = std::max(r.worst, std::fabs(e));
      eRef += yr - ys;
      eRec += yc - ys;
      ++r.blocks;
    }
  r.rms = r.blocks ? std::sqrt(s / double(r.blocks)) : 0.0;
  r.energy = eRef > 0 ? eRec / eRef : 1.0;
  return r;
}

static Image8 toImage8(const Dec& d) {
  Image8 im;
  im.width = d.w;
  im.height = d.h;
  im.data = d.px;
  return im;
}

extern "C" float dapple_internal_modern_white_ev(DappleSession* session, const char* params_json);

// exports `params` three ways from one session and checks the contract; returns the info
static DappleHdrInfo exportAndCheck(DappleSession* s, const char* params, int longEdge, const fs::path& dir, const std::string& tag, bool expectGain, Bytes* fileOut = nullptr) {
  const fs::path sdrPath = dir / (tag + "_sdr.jpg"), hdrPath = dir / (tag + "_hdr.jpg");
  CHECK(dapple_session_export_jpeg(s, params, "", longEdge, 92, sdrPath.string().c_str()) == DAPPLE_OK);
  DappleHdrInfo info{};
  CHECK(dapple_session_export_jpeg_hdr(s, params, "", longEdge, 92, hdrPath.string().c_str(), 0.f, &info) == DAPPLE_OK);
  const Bytes sdr = readAll(sdrPath), hdr = readAll(hdrPath);
  std::printf("  %s: written=%d reason=%d H=%.1f peak=%.2f gainMax=%.3f cap=%.3f  (%zu -> %zu bytes)\n", tag.c_str(), info.written, info.reason, info.headroom_stops,
              info.peak, info.gain_max, info.capacity_max, sdr.size(), hdr.size());
  if (fileOut) *fileOut = hdr;
  CHECK((info.written != 0) == expectGain);
  if (!info.written) {
    CHECK(hdr == sdr);  // the option falls back to the plain JPEG, byte for byte the SDR export
    CHECK(!parse(hdr).has_value());
    return info;
  }
  auto p = parse(hdr);
  CHECK(p.has_value());
  if (!p) return info;
  CHECK(p->base == sdr);  // base stream = today's export, only segments added
  CHECK(p->mpfCount == 2 && p->primaryEnd + p->gainSize == hdr.size());
  CHECK(std::fabs(p->meta.gainMax - info.gain_max) < 1e-4f && p->meta.gainMin == 0.f);
  CHECK(info.headroom_stops >= 0.5f && info.headroom_stops <= kMaxHeadroomStops);
  Dec b, g;
  CHECK(decode(Bytes(hdr.begin(), hdr.begin() + std::ptrdiff_t(p->primaryEnd)), 3, b));
  CHECK(decode(p->gainJpeg, 1, g) && g.comps == 1);
  CHECK(std::abs(g.w * 4 - b.w) <= 4 && std::abs(g.h * 4 - b.h) <= 4);  // quarter resolution
  // reconstruct SDR * 2^gain and compare with the HDR rendition of the same size (bright rendition at full size, fused as the definition says)
  if (b.w && g.w) {
    GainMap gm;
    gm.width = g.w;
    gm.height = g.h;
    gm.data = g.px;
    gm.meta = p->meta;
    const Image8 sdrImg = toImage8(b);
    const ImageF rec = reconstruct(sdrImg, gm);
    // reference: the same edit with exposure - H at the base size
    std::string pj = params;
    // exposure is a flat JSON number: rewrite it
    char buf[64];
    float e0 = 0.f;
    if (const char* k = std::strstr(params, "\"exposureEv\"")) e0 = float(std::atof(std::strchr(k, ':') + 1));
    // a modern rendering: the bright rendition is the standard rendering lowered so that the picture's own white point sits at SDR white too
    std::snprintf(buf, sizeof buf, "{\"exposureEv\":%.4f}", e0 - info.headroom_stops - dapple_internal_modern_white_ev(s, params));
    const fs::path brPath = dir / (tag + "_bright.jpg");
    CHECK(dapple_session_export_jpeg(s, buf, "", longEdge, 100, brPath.string().c_str()) == DAPPLE_OK);
    Dec br;
    CHECK(decode(readAll(brPath), 3, br) && br.w == b.w && br.h == b.h);
    if (b.w == br.w && b.w) {  // colour of the excess: the gain map is one channel, so the displayed highlight is the SDR pixel scaled: its colour is the SDR (family) colour
      double spread = 0, hueMax = 0;
      std::size_t n = 0;
      for (std::size_t k = 0; k < std::size_t(b.w) * b.h; ++k) {
        const float c[3] = {srgbToLinear(sdrImg.data[3 * k] / 255.f), srgbToLinear(sdrImg.data[3 * k + 1] / 255.f), srgbToLinear(sdrImg.data[3 * k + 2] / 255.f)};
        const float yS = lumaLin(c), yR = lumaLin(&rec.data[3 * k]);
        if (yR < 1.4f * yS || yS < 0.2f) continue;
        float mx = 0.f, mn = 1e9f;
        for (int q = 0; q < 3; ++q) {
          const float r = (rec.data[3 * k + q] + 1e-4f) / (c[q] + 1e-4f);
          mx = std::max(mx, r), mn = std::min(mn, r);
        }
        spread += mx / mn - 1.f;
        hueMax = std::max<double>(hueMax, mx / mn - 1.f);
        ++n;
      }
      std::printf("  %s: colour of the excess, %zu highlight pixels: per-channel ratio rec / SDR differs by %.4f on average, %.4f at most (0 = same colour as the SDR rendition)\n", tag.c_str(), n, n ? spread / double(n) : 0.0, hueMax);
    }
    if (br.w == b.w) {
      const ImageF ref = hdrLinear(sdrImg, toImage8(br), info.headroom_stops);
      const BlockStats bs = blockStats(rec, ref, sdrImg);
      std::printf("  %s: reconstruction vs HDR rendition at 8 px scale, %zu blocks with extra light: rms %.3f stop, worst %.3f stop, extra light carried %.0f %%\n", tag.c_str(),
                  bs.blocks, bs.rms, bs.worst, bs.energy * 100.0);
      CHECK(bs.blocks > 0 && bs.rms < 0.15 && bs.worst < 0.6 && bs.energy > 0.8 && bs.energy < 1.05);
    }
  }
  return info;
}

static void testSession() {
  const fs::path f = sample("Canon/EOS-R6-Mark-II/canon-r6m2-img_0346-raw-normal-full.cr3");
  if (f.empty()) {
    std::printf("  SKIP no build/rawsamples\n");
    std::exit(77);
  }
  const fs::path dir = root() / "build" / "uhdr" / "test";
  fs::create_directories(dir);
  Session se(f, 1600);
  CHECK(se.s != nullptr);
  if (!se.s) return;
  // the sample as it is (if it has headroom the export carries a gain map; either way the contract holds)
  DappleHdrInfo probe{};
  CHECK(dapple_session_hdr_headroom(se.s, "{}", "", &probe) == DAPPLE_OK);
  exportAndCheck(se.s, "{}", 1200, dir, "default", probe.written != 0);
  // darker exposure: the highlights go below SDR white, no headroom worth encoding -> plain JPEG, identical to the SDR export
  exportAndCheck(se.s, "{\"exposureEv\":-4}", 1200, dir, "dark", false);
  // film: a print, paper white is the top -> plain JPEG
  const DappleHdrInfo fi = exportAndCheck(se.s, "{\"film\":\"kodak_gold_200\"}", 1200, dir, "film", false);
  CHECK(fi.reason == 2);
  // a modern rendering is no film: headroom is measured above its own white point (section 37); the base stays the SDR export byte for byte
  for (const char* id : {"modern_clean", "modern_vivid", "modern_soft"}) {
    const std::string pj = std::string("{\"film\":\"") + id + "\"}";
    DappleHdrInfo mp{};
    CHECK(dapple_session_hdr_headroom(se.s, pj.c_str(), "", &mp) == DAPPLE_OK);
    CHECK(mp.reason != 2);
    const DappleHdrInfo me = exportAndCheck(se.s, pj.c_str(), 1200, dir, id, mp.written != 0);
    std::printf("  %s: written %d reason %d H %.2f peak %.2f gain max %.3f\n", id, me.written, me.reason, double(me.headroom_stops), double(me.peak), double(me.gain_max));
  }
  {
    DappleHdrInfo mp{};
    const std::string pj = "{\"film\":\"modern_clean\",\"exposureEv\":1.0}";
    CHECK(dapple_session_hdr_headroom(se.s, pj.c_str(), "", &mp) == DAPPLE_OK);
    const DappleHdrInfo me = exportAndCheck(se.s, pj.c_str(), 1200, dir, "modern_bright", mp.written != 0);
    CHECK(me.written != 0);
    std::printf("  modern_clean +1 EV: written %d H %.2f peak %.2f gain max %.3f\n", me.written, double(me.headroom_stops), double(me.peak), double(me.gain_max));
  }
  // an explicit headroom of 2 stops on a bright exposure
  DappleHdrInfo info{};
  const fs::path out = dir / "explicit.jpg";
  CHECK(dapple_session_export_jpeg_hdr(se.s, "{\"exposureEv\":1.0}", "", 1200, 92, out.string().c_str(), 2.f, &info) == DAPPLE_OK);
  if (info.written) CHECK(std::fabs(info.headroom_stops - 2.f) < 1e-4f);
  // a failing call reports an error
  CHECK(dapple_session_export_jpeg_hdr(nullptr, "{}", "", 1200, 92, out.string().c_str(), 0.f, &info) == DAPPLE_ERROR);
}

static void testJpegSource() {
  const fs::path dir = root() / "build" / "uhdr" / "test";
  fs::create_directories(dir);
  Image8 im;
  im.width = 640;
  im.height = 480;
  im.data.resize(640 * 480 * 3);
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 640; ++x)
      for (int c = 0; c < 3; ++c) im.data[3 * (std::size_t(y) * 640 + x) + c] = std::uint8_t(255 * x / 639);  // up to 255: display referred, white is the top
  const fs::path src = dir / "ramp.jpg";
  CHECK(writeJpeg(im, src));
  Session se(src, 0);
  CHECK(se.s != nullptr);
  if (!se.s) return;
  exportAndCheck(se.s, "{}", 0, dir, "jpegsrc", false);
}

static int evidence(int argc, char** argv) {
  const fs::path dir = argv[2];
  fs::create_directories(dir);
  std::string meta;
  for (int i = 3; i < argc; ++i) {
    const fs::path f = argv[i];
    const std::string stem = f.stem().string();
    Session se(f, 3200);
    if (!se.s) {
      std::printf("open failed: %s\n", f.string().c_str());
      continue;
    }
    const fs::path out = dir / (stem + "_uhdr.jpg");
    DappleHdrInfo info{};
    if (dapple_session_export_jpeg_hdr(se.s, "{}", "", 2400, 92, out.string().c_str(), 0.f, &info) != DAPPLE_OK) {
      std::printf("export failed: %s: %s\n", stem.c_str(), dapple_last_error());
      continue;
    }
    char line[512];
    std::snprintf(line, sizeof line, "%s: written=%d reason=%d headroomStops=%.2f peakSdrWhite=%.2f", stem.c_str(), info.written, info.reason, info.headroom_stops, info.peak);
    meta += line;
    if (info.written) {
      const Bytes file = readAll(out);
      auto p = parse(file);
      if (!p) {
        meta += "  PARSE FAILED\n";
        continue;
      }
      std::snprintf(line, sizeof line,
                    "\n  file %zu bytes, primary %zu, gain map %zu bytes %dx%d (%d component)\n  hdrgm:Version=1.0 GainMapMin=%.4f GainMapMax=%.4f Gamma=%.3f OffsetSDR=%.6f OffsetHDR=%.6f "
                    "HDRCapacityMin=%.4f HDRCapacityMax=%.4f BaseRenditionIsHDR=False\n  ISO 21496-1: gainMax=%.4f capacityMax=%.4f baseHeadroom=0\n",
                    file.size(), p->primaryEnd, p->gainSize, p->gainWidth, p->gainHeight, p->gainComponents, p->meta.gainMin, p->meta.gainMax, p->meta.gamma, p->meta.offsetSdr,
                    p->meta.offsetHdr, p->meta.capacityMin, p->meta.capacityMax, p->isoMeta.gainMax, p->isoMeta.capacityMax);
      meta += line;
      Dec b, g;
      decode(Bytes(file.begin(), file.begin() + std::ptrdiff_t(p->primaryEnd)), 3, b);
      decode(p->gainJpeg, 1, g);
      // gain map PNG (0 = no gain, white = GainMapMax), nearest-neighbour x2 for visibility
      std::vector<std::uint8_t> gp(std::size_t(g.w) * g.h);
      std::copy(g.px.begin(), g.px.end(), gp.begin());
      stbi_write_png((dir / (stem + "_gainmap.png")).string().c_str(), g.w, g.h, 1, gp.data(), g.w);
      GainMap gm;
      gm.width = g.w;
      gm.height = g.h;
      gm.data = g.px;
      gm.meta = p->meta;
      const ImageF hdr = reconstruct(toImage8(b), gm);
      // side by side: SDR | HDR view with the highlights tone-compressed so that the extra range shows on an SDR screen (identity up to 0.5, soft roll-off to 1 at the peak)
      const float peak = std::exp2(p->meta.capacityMax) * 1.f;
      const float knee = 0.5f;
      std::vector<std::uint8_t> sb(std::size_t(2 * b.w) * b.h * 3);
      for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
          const std::size_t i = std::size_t(y) * b.w + x;
          for (int c = 0; c < 3; ++c) {
            sb[(std::size_t(y) * 2 * b.w + x) * 3 + c] = b.px[3 * i + c];
            float v = hdr.data[3 * i + c];
            if (v > knee) v = knee + (1.f - knee) * (1.f - std::exp(-(v - knee) / (peak - knee) * 2.2f));
            sb[(std::size_t(y) * 2 * b.w + b.w + x) * 3 + c] = enc8(v);
          }
        }
      stbi_write_png((dir / (stem + "_sdr_vs_hdr.png")).string().c_str(), 2 * b.w, b.h, 3, sb.data(), 2 * b.w * 3);
    } else
      meta += "  (no gain map: plain JPEG)\n";
  }
  std::ofstream(dir / "metadata.txt") << meta;
  std::printf("%s", meta.c_str());
  return 0;
}

static int explore(int argc, char** argv) {
  for (int i = 2; i < argc; ++i) {
    Session se(argv[i], 1600);
    DappleHdrInfo info{};
    if (!se.s || dapple_session_hdr_headroom(se.s, "{}", "", &info) != DAPPLE_OK) {
      std::printf("%s: failed\n", argv[i]);
      continue;
    }
    std::printf("%s: written=%d reason=%d H=%.1f peak=%.2f\n", argv[i], info.written, info.reason, info.headroom_stops, info.peak);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "evidence") == 0 && argc >= 4) return evidence(argc, argv);
  if (argc >= 2 && std::strcmp(argv[1], "explore") == 0) return explore(argc, argv);
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"container", testContainer}, {"reconstruct", testReconstruct}, {"rule", testRule}, {"jpeg_source", testJpegSource}, {"session", testSession}};
  if (argc < 2) {
    for (const T& t : tests) std::printf("%s\n", t.id);
    return 0;
  }
  if (std::strcmp(argv[1], "--list") == 0) {
    for (const T& t : tests) std::printf("%s\n", t.id);
    return 0;
  }
  for (const T& t : tests)
    if (std::strcmp(argv[1], t.id) == 0) {
      std::printf("[%s]\n", t.id);
      t.fn();
      std::printf("%s\n", g_fail ? "FAILED" : "ok");
      return g_fail ? 1 : 0;
    }
  std::printf("unknown test %s\n", argv[1]);
  return 2;
}
