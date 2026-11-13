// Streaming (band) export: equality with the whole-frame export, memory bound, cancellation (docs/engine/pipeline-architecture.md section 42).
//
//   dapple_export_stream_tests <id>
//     region      region demosaic (the band source) == the same pixels of the full demosaic, Bayer and X-Trans, every orientation: bit-identical
//     reductions  the analysis pass (P256 / P512 / further long edges / modern samples / raster guide capture / shadow floor decision) ==
//                 the same quantities computed on the whole picture: bit-identical (normal and near-black frames)
//     encoder     JpegStripEncoder fed in bands == encodeJpeg of the whole picture, byte for byte
//     synthetic   streamed render == whole-frame render (both TiledExport tile 512) over the edit matrix on a synthetic 24 MP Bayer mosaic,
//                 engine level (no file): bit-identical; and vs the whole-frame path's own tile: within the tile tolerance (max 1 code)
//     peak        synthetic 60 MP mosaic: the streamed export adds at most 300 MB to the footprint (6 threads, a phone's pool)
//     capi        C API on a real RAW (build/rawsamples, skip 77 without): DAPPLE_EXPORT_STREAM=on vs off over the edit matrix (incl. film, Ultra
//                 HDR, P3, raster mask): files compared (byte-identical expected; decoded pixels otherwise); GPU executor when
//                 DAPPLE_STREAM_TEST_GPU=1 (ctest: SwiftShader)
//     cancel      a progress callback cancels at the third band: DAPPLE_ERROR "cancelled", nothing at the path, no temp file left
//   dapple_export_stream_tests mem <raw> <preset> [gpu] [stream=on|off|auto] [threads=N]   one open FULL + export, memory per stage (one process each)
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"
#include "dapple/mosaic.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#include "engine/nodes/demosaic/demosaic_session.h"
#include "export_stream.h"
#include "local_eval.h"
#include "mem_probe.h"

namespace fs = std::filesystem;
using namespace dapple;

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

static void setEnv(const char* k, const char* v) {
#if defined(_WIN32)
  _putenv_s(k, v ? v : "");
#else
  if (v) setenv(k, v, 1);
  else unsetenv(k);
#endif
}

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static bool loadFilm() {
  static int ok = -1;
  if (ok < 0) ok = dapple_film_load_pack((root() / "core" / "data" / "film" / "dapple.dfp").string().c_str()) == DAPPLE_OK ? 1 : 0;
  return ok == 1;
}

// A raster mask plane (a soft disc, 256 x 171) under "stream_r".
static void setRaster() {
  const int w = 256, h = 171;
  std::vector<std::uint8_t> px(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float dx = (x - 160.f) / 60.f, dy = (y - 70.f) / 50.f, r = std::sqrt(dx * dx + dy * dy);
      px[std::size_t(y) * w + x] = std::uint8_t(std::clamp(255.f * (1.5f - r), 0.f, 255.f));
    }
  dapple_mask_raster_set("stream_r", w, h, px.data(), w);
}

// ---- the edit matrix ---------------------------------------------------------------------------------------------------------------------------
struct Preset {
  const char* id;
  const char* params;
  bool hdr = false;  // Ultra HDR export
  bool p3 = false;   // Display P3
};
static const char* kLocal =
    R"({"local":{"v":1,"items":[)"
    R"({"id":"a","strength":100,"mask":[{"kind":"linear","op":"add","x0":0.5,"y0":0.0,"x1":0.5,"y1":0.5}],"adjust":{"exposure":-20,"clarity":40}},)"
    R"({"id":"b","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.3,"cy":0.6,"rx":0.2,"ry":0.15,"angle":20,"feather":40}],"adjust":{"exposure":30,"texture":30}},)"
    R"({"id":"c","strength":80,"mask":[{"kind":"luminance","op":"add","lo":60,"hi":100,"feather":20}],"adjust":{"highlights":-40}},)"
    R"({"id":"d","strength":100,"mask":[{"kind":"colour","op":"add","hue":220,"range":40,"feather":20,"satMin":10}],"adjust":{"saturation":30}},)"
    R"({"id":"e","strength":100,"mask":[{"kind":"brush","op":"add","strokes":[{"size":0.05,"hardness":50,"flow":100,"pts":[0.1,0.1,0.3,0.2,0.5,0.25]}]}],"adjust":{"exposure":20,"sharpen":40}},)"
    R"({"id":"f","strength":100,"mask":[{"kind":"raster","ref":"stream_r","refine":60}],"adjust":{"exposure":15,"dehaze":30}}]}})";
static const std::vector<Preset>& presets() {
  static const std::vector<Preset> p = {
      {"std", "{}"},
      {"film", R"({"film":"kodak_portra_400","film_grain":100,"film_halation":100})"},
      {"modern", R"({"film":"modern_vivid","film_strength":100})"},
      {"hdr", R"({"hdr":80,"exposure":10})"},
      {"detail", R"({"clarity":60,"texture":40,"sharpen":80,"nrLuma":40,"nrColor":40,"dehaze":30})"},
      {"geo", R"({"crop":"4:3","rot":90,"straighten":7.5})"},
      {"vignette", R"({"vignette":-60})"},
      {"local", kLocal},
      {"p3", R"({"saturation":40})", false, true},
      {"uhdr", R"({"exposure":20})", true, false},
      {"uhdr_modern", R"({"film":"modern_clean","film_strength":100,"exposure":10})", true, false},
  };
  return p;
}
static const Preset* findPreset(const std::string& id) {
  for (const Preset& p : presets())
    if (id == p.id) return &p;
  return nullptr;
}

// ---- synthetic mosaics -----------------------------------------------------------------------------------------------------------------------
static std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
  return x;
}
// A W x H mosaic with smooth gradients, blocks (edges for the neighbourhood stages), noise and a clipped highlight. `dark` = a near-black frame
// (the shadow-chroma floor applies). flip: 0 / 3 / 5 / 6.
static std::shared_ptr<MosaicImage> makeMosaic(int W, int H, bool xtrans, bool dark = false, int flip = 0) {
  auto m = std::make_shared<MosaicImage>();
  m->width = W, m->height = H;
  m->cfa = xtrans ? CfaKind::XTrans : CfaKind::Bayer;
  m->period = xtrans ? 6 : 2;
  static const std::uint8_t xt[36] = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1, 1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};
  static const std::uint8_t bayer[4] = {0, 1, 3, 2};
  std::memcpy(m->cfaIdx, xtrans ? xt : bayer, xtrans ? 36 : 4);
  m->blackRows = m->blackCols = m->period;
  m->black.assign(std::size_t(m->period * m->period), 512.f);
  m->white = 16383.f;
  m->blackMin = 512.f;
  m->wb[0] = 2.1f, m->wb[1] = 1.f, m->wb[2] = 1.6f, m->wb[3] = 1.f;
  const float M[9] = {0.80f, 0.15f, 0.05f, 0.05f, 0.90f, 0.05f, 0.02f, 0.10f, 0.88f};
  std::memcpy(m->camToWork, M, sizeof M);
  m->flip = flip;
  m->meta.make = "Synthetic";
  m->meta.model = "Mosaic";
  m->meta.iso = 400;
  m->meta.cfa = m->cfa;
  m->data.resize(std::size_t(W) * H);
  const float span = 16383.f - 512.f;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float u = float(x) / float(W), v = float(y) / float(H);
      const int c = m->cfaIdx[(y % m->period) * m->period + (x % m->period)];
      const float n = float(hash32(std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      const float tex = (((x / 37) + (y / 29)) & 1) ? 1.f : 0.8f;
      float val = c == 0 ? 0.05f + 0.6f * u * (1.f - 0.4f * v) : c == 2 ? 0.04f + 0.5f * (1.f - v) * (1.f - 0.5f * u) : 0.05f + 0.45f * (1.f - u) * v + 0.1f * u;
      if (c == 3) val *= 1.01f;
      val *= tex * (0.92f + 0.16f * n) * 0.45f;
      if ((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f) < 0.004f) val = 1.2f;  // clipped highlight
      if (dark) val *= 0.01f;
      m->data[std::size_t(y) * W + x] = std::uint16_t(std::clamp(512.f + val * span, 0.f, 16383.f));
    }
  return m;
}
static LazyFull lazyOf(const std::shared_ptr<MosaicImage>& m) {
  LazyFull z;
  z.mosaic = m;
  z.meta.meta = m->meta;
  z.meta.sceneReferred = true;
  demosaic::orientedSize(*m, &z.width, &z.height);
  return z;
}

static bool sameImage(const ImageF& a, const ImageF& b) {
  return a.width == b.width && a.height == b.height && a.data.size() == b.data.size() &&
         std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0;
}
static ImageF crop(const ImageF& im, int x0, int y0, int x1, int y1) {
  ImageF o(x1 - x0, y1 - y0);
  for (int y = y0; y < y1; ++y) std::memcpy(o.row(y - y0), im.row(y) + 3 * x0, std::size_t(x1 - x0) * 3 * sizeof(float));
  return o;
}

// ---- region ---------------------------------------------------------------------------------------------------------------------------------
static void testRegion() {
  for (bool xt : {false, true})
    for (int flip : {0, 3, 5, 6}) {
      auto m = makeMosaic(611, 437, xt, false, flip);
      const ImageF full = demosaic::demosaicFull(*m);
      int W, H;
      demosaic::orientedSize(*m, &W, &H);
      CHECK(full.width == W && full.height == H);
      int bad = 0;
      const int rects[][4] = {{0, 0, W, 37}, {0, 101, W, 359}, {17, 23, 211, 77}, {W - 50, H - 41, W, H}, {0, 0, W, H}};
      for (const auto& r : rects) {
        bool fb = false;
        const ImageF w = demosaic::demosaicOrientedRegionOn(*m, r[0], r[1], r[2], r[3], false, &fb);
        if (!sameImage(w, crop(full, r[0], r[1], r[2], r[3]))) ++bad;
      }
      std::printf("  %s flip %d: %d of 5 windows differ from the full demosaic\n", xt ? "X-Trans" : "Bayer  ", flip, bad);
      CHECK(bad == 0);
    }
}

// ---- reductions ----------------------------------------------------------------------------------------------------------------------------
static void testReductions() {
  for (int k = 0; k < 3; ++k) {
    const bool xt = k == 1, dark = k == 2;
    auto m = makeMosaic(2400, 1601, xt, dark, k == 1 ? 6 : 0);
    const LazyFull z = lazyOf(m);
    const SourceImage whole = materializeFull(z);
    xstream::BandSource bs(m);
    xstream::Wants w;
    w.longEdges = {1000, 777, 600};
    w.guideGrids = {{64, 43}, {101, 67}};
    w.guide = true;
    xstream::Analyses a;
    std::string err;
    CHECK(xstream::analyse(bs, z.meta, w, a, {}, &err));
    std::printf("  %s%s %dx%d: %d pass(es), shadow floor %d (%.0f ms)\n", xt ? "X-Trans" : "Bayer", dark ? " near-black" : "", z.width, z.height, a.passes, a.shadow ? 1 : 0, a.ms);
    CHECK(a.shadow == dark);
    CHECK(a.p256 && sameImage(a.p256->linear, resizeToLongEdge(whole.linear, 256)));
    CHECK(a.p512 && sameImage(a.p512->linear, resizeToLongEdge(whole.linear, 512)));
    for (int le : w.longEdges) CHECK(a.at(le) && sameImage(a.at(le)->linear, resizeToLongEdge(whole.linear, le)));
    // modern end-point samples
    int gw, gh;
    Renderer::modernGridSize(z.width, z.height, gw, gh);
    bool gridOk = a.grid && a.grid->linear.width == gw && a.grid->linear.height == gh;
    for (int j = 0; gridOk && j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        int x, y;
        Renderer::modernGridPixel(z.width, z.height, i, j, x, y);
        gridOk &= std::memcmp(a.grid->linear.row(j) + 3 * i, whole.linear.row(y) + 3 * x, 3 * sizeof(float)) == 0;
      }
    CHECK(gridOk);
    // raster guide: the 64 x 64 samples and the area means (guideGrid's arithmetic)
    bool gOk = a.guide && a.guide->samples.size() == 64 * 64 * 3;
    for (int j = 0; gOk && j < 64; ++j)
      for (int i = 0; i < 64; ++i) {
        int x, y;
        local::guideSamplePos(z.width, z.height, i, j, x, y);
        gOk &= std::memcmp(a.guide->samples.data() + 3 * (j * 64 + i), whole.linear.row(y) + 3 * x, 3 * sizeof(float)) == 0;
      }
    for (const auto& g : a.guide->grids) {
      for (int j = 0; gOk && j < g.gh; ++j) {
        const int W = z.width, H = z.height;
        const int y0 = int(std::floor(double(j) * H / g.gh)), y1 = std::max(y0 + 1, int(std::floor(double(j + 1) * H / g.gh)));
        for (int i = 0; i < g.gw; ++i) {
          const int x0 = int(std::floor(double(i) * W / g.gw)), x1 = std::max(x0 + 1, int(std::floor(double(i + 1) * W / g.gw)));
          double acc[3] = {0, 0, 0};
          for (int y = y0; y < std::min(y1, H); ++y)
            for (int x = x0; x < std::min(x1, W); ++x)
              for (int c = 0; c < 3; ++c) acc[c] += whole.linear.row(y)[3 * x + c];
          const double n = double((std::min(y1, H) - y0) * (std::min(x1, W) - x0));
          for (int c = 0; c < 3; ++c) gOk &= g.mean[(std::size_t(j) * g.gw + i) * 3 + c] == float(acc[c] / n);
        }
      }
    }
    CHECK(gOk);
  }
}

static Image8 decodeJpeg(const std::vector<std::uint8_t>& b);
static int diff8(const Image8& a, const Image8& b) {
  if (a.width != b.width || a.height != b.height || a.data.size() != b.data.size() || a.data.empty()) return 999;
  int m = 0;
  for (std::size_t i = 0; i < a.data.size(); ++i) m = std::max(m, std::abs(int(a.data[i]) - int(b.data[i])));
  return m;
}

// ---- encoder --------------------------------------------------------------------------------------------------------------------------------
static void testEncoder() {
  for (const auto& wh : {std::pair<int, int>{1500, 1400}, std::pair<int, int>{2100, 1025}, std::pair<int, int>{640, 480}, std::pair<int, int>{9504, 1100}}) {
    Image8 im;
    im.width = wh.first, im.height = wh.second;
    im.data.resize(std::size_t(im.width) * im.height * 3);
    for (int y = 0; y < im.height; ++y)
      for (int x = 0; x < im.width; ++x)
        for (int c = 0; c < 3; ++c) im.data[(std::size_t(y) * im.width + x) * 3 + c] = std::uint8_t((x * (c + 1) + y * 3 + (hash32(std::uint32_t(x * 7 + y * 131 + c)) & 15)) & 255);
    JpegOptions o;
    o.quality = 90;
    const auto ref = encodeJpeg(im, o);
    CHECK(bool(ref));
    for (int band : {512, 300, 77}) {
      JpegStripEncoder enc(im.width, im.height, o);
      for (int y = 0; y < im.height; y += band) CHECK(enc.push(im.data.data() + std::size_t(y) * im.width * 3, std::min(band, im.height - y)));
      const auto got = enc.finish();
      CHECK(got && ref && *got == *ref);
    }
    {  // the strips decode to the pixels of one single-threaded encode (restarts change bytes only)
      std::vector<std::uint8_t> single;
      stbi_write_jpg_to_func([](void* c, void* d, int n) { auto* v = static_cast<std::vector<std::uint8_t>*>(c); v->insert(v->end(), (const std::uint8_t*)d, (const std::uint8_t*)d + n); },
                             &single, im.width, im.height, 3, im.data.data(), o.quality);
      CHECK(ref && diff8(decodeJpeg(*ref), decodeJpeg(single)) == 0);
    }
    std::printf("  %dx%d: streamed encode == encodeJpeg (%zu bytes, %s)\n", im.width, im.height, ref ? ref->size() : 0, im.width * im.height >= 2000000 ? "strips" : "single");
  }
}

// ---- engine-level streamed render vs whole -----------------------------------------------------------------------------------------------
static const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("SONY", "ILCE-7M4");
}
struct Diff {
  int maxCode = 0;
  std::size_t differing = 0;
  bool sizeOk = true;
};
static Diff diff(const Image8& a, const Image8& b) {
  Diff d;
  if (a.width != b.width || a.height != b.height || a.data.size() != b.data.size()) {
    d.sizeOk = false;
    d.maxCode = 999;
    return d;
  }
  for (std::size_t i = 0; i < a.data.size(); i += 3) {
    int m = 0;
    for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a.data[i + c]) - int(b.data[i + c])));
    d.maxCode = std::max(d.maxCode, m);
    d.differing += m != 0;
  }
  return d;
}
// The streamed render of a lazy picture at the engine level (what the C layer's streamRender does on the CPU).
static bool streamed(const LazyFull& z, const EditParams& p, const xstream::Analyses& a, int tile, Image8& out, std::string& err) {
  Renderer r(p, cam(), nullptr);
  r.setAnalysisSources(a.p256.get(), a.grid.get(), a.p512.get());
  const xstream::GuideScope gs(a.guide.get());
  xstream::BandSource bs(z.mosaic);
  bs.setShadowFloor(a.shadow);
  engine::EngineSession es;
  const SourceImage virt = xstream::sizeOnly(z.meta, z.width, z.height);
  return xstream::renderBands(
      es, p, r, virt, bs, tile, false,
      [&](int w, int h) {
        out.width = w, out.height = h;
        out.data.assign(std::size_t(w) * h * 3, 0);
      },
      [&](int y0, int n, const std::uint8_t* rgb) {
        std::memcpy(out.data.data() + std::size_t(y0) * out.width * 3, rgb, std::size_t(n) * out.width * 3);
        return true;
      },
      &err);
}
static bool wholeRender(const SourceImage& src, const EditParams& p, int tile, Image8& out, std::string& err) {
  Renderer r(p, cam(), nullptr);
  engine::EngineSession es;
  engine::EngineSession::ExactOptions o;
  o.mode = engine::ExecMode::TiledExport;
  o.tile = tile;  // 0 = the whole-frame path's own choice (memory budget)
  return es.renderExact(p, r, src, o, out, &err);
}

static void testSynthetic() {
  setRaster();
  auto m = makeMosaic(6000, 4000, false);
  const LazyFull z = lazyOf(m);
  const SourceImage whole = materializeFull(z);
  struct C {
    const char* id;
    const char* json;
  };
  const C cases[] = {{"std", "{}"},
                     {"modern", R"({"film":"modern_vivid","film_strength":100})"},
                     {"hdr", R"({"hdr":80,"exposure":10})"},
                     {"detail", R"({"clarity":60,"texture":40,"sharpen":80,"nrLuma":40,"nrColor":40,"dehaze":30})"},
                     {"geo", R"({"crop":"4:3","rot":90,"straighten":7.5})"},
                     {"vignette", R"({"vignette":-60})"},
                     {"local", kLocal},
                     {"p3", R"({"saturation":40,"outputSpace":1})"}};
  for (const C& c : cases) {
    auto pp = fromJson(c.json);
    CHECK(bool(pp));
    if (!pp) continue;
    EditParams p = *pp;
    if (std::strcmp(c.id, "p3") == 0) p.outputSpace = 1;
    xstream::Wants w;
    w.guideGrids = local::guideGridSizes(p.local, z.width, z.height, false);
    w.guide = !w.guideGrids.empty();
    xstream::BandSource bs(m);
    xstream::Analyses a;
    std::string err;
    CHECK(xstream::analyse(bs, z.meta, w, a, {}, &err));
    Image8 s, wt, wn;
    const auto t0 = Clock::now();
    const bool okS = streamed(z, p, a, 512, s, err);
    const double msS = msSince(t0);
    if (!okS) std::printf("  %s: streamed failed: %s\n", c.id, err.c_str());
    const auto t1 = Clock::now();
    const bool okW = wholeRender(whole, p, 512, wt, err);
    const double msW = msSince(t1);
    const bool okN = wholeRender(whole, p, 0, wn, err);
    CHECK(okS && okW && okN);
    const Diff d = diff(s, wt), dn = diff(s, wn);
    std::printf("  %-8s %dx%d  vs whole (tile 512): max %d, %zu px differ  |  vs whole (own tile): max %d, %zu px differ   (stream %.0f ms, whole %.0f ms)\n", c.id,
                s.width, s.height, d.maxCode, d.differing, dn.maxCode, dn.differing, msS, msW);
    CHECK(d.sizeOk && d.maxCode == 0);
    CHECK(dn.sizeOk && dn.maxCode <= 1);
  }
}

// ---- peak ------------------------------------------------------------------------------------------------------------------------------------
static void testPeak() {
  dapple_perf_configure(6, 0);  // a phone's pool: the per-thread demosaic / tile scratch scales with the thread count
  auto m = makeMosaic(9504, 6336, false);
  const LazyFull z = lazyOf(m);
  const std::size_t base = mem::footprint();
  mem::Sampler smp;
  mem::current() = &smp;
  smp.stage("analyse");
  xstream::BandSource bs(m);
  xstream::Analyses a;
  std::string err;
  EditParams p = *fromJson(R"({"clarity":40,"sharpen":60,"nrLuma":30,"vignette":-30,"hdr":50})");
  CHECK(xstream::analyse(bs, z.meta, xstream::Wants{}, a, {}, &err));
  std::size_t rows = 0;
  JpegStripEncoder* encp = nullptr;
  std::unique_ptr<JpegStripEncoder> enc;
  Renderer r(p, cam(), nullptr);
  r.setAnalysisSources(a.p256.get(), a.grid.get(), a.p512.get());
  xstream::BandSource bs2(m);
  engine::EngineSession es;
  const SourceImage virt = xstream::sizeOnly(z.meta, z.width, z.height);
  const auto t0 = Clock::now();
  smp.stage("render");
  const bool ok = xstream::renderBands(
      es, p, r, virt, bs2, 512, false,
      [&](int w, int h) {
        enc = std::make_unique<JpegStripEncoder>(w, h, JpegOptions{});
        encp = enc.get();
      },
      [&](int, int n, const std::uint8_t* rgb) {
        rows += std::size_t(n);
        return encp->push(rgb, n);
      },
      &err);
  const auto bytes = ok && enc ? enc->finish() : Result<std::vector<std::uint8_t>>(fail("render failed"));
  if (!bytes) std::printf("  encode: %s\n", bytes.error().message.c_str());
  smp.finish();
  mem::current() = nullptr;
  const double added = double(smp.peak() - std::min(smp.peak(), base)) / 1048576.0;
  std::printf("  largest source window %.0f MB, %llu windows, analysis pass %.0f ms; stage peaks (MB, absolute): %s\n", double(bs2.peakWindowBytes()) / 1048576.0,
              (unsigned long long)bs2.windows(), a.ms, smp.summary().c_str());
  std::printf("  60 MP (9504x6336) streamed: %s in %.1f s, %zu rows, JPEG %.1f MB; mosaic %.0f MB; footprint added at the peak %.0f MB (bound 300)\n", ok ? "ok" : err.c_str(),
              msSince(t0) / 1000.0, rows, bytes ? double(bytes->size()) / 1048576.0 : 0.0, double(m->data.size() * 2) / 1048576.0, added);
  CHECK(ok && rows == 6336 && bytes);
  CHECK(added <= 300.0);
  dapple_perf_configure(0, 0);
}

// ---- C API on a real RAW ------------------------------------------------------------------------------------------------------------------
static std::vector<std::uint8_t> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static Image8 decodeJpeg(const std::vector<std::uint8_t>& b) {
  Image8 o;
  int c = 0;
  std::uint8_t* d = stbi_load_from_memory(b.data(), int(b.size()), &o.width, &o.height, &c, 3);
  if (d) o.data.assign(d, d + std::size_t(o.width) * o.height * 3);
  stbi_image_free(d);
  return o;
}

// Open FULL + export one preset. Returns the C API code.
static int exportOnce(const std::string& raw, const Preset& p, const std::string& out, double* ms = nullptr, mem::Sampler* smp = nullptr, bool* streaming = nullptr) {
  if (std::strstr(p.params, "\"film\":\"kodak")) loadFilm();
  if (std::strstr(p.params, "raster")) setRaster();
  const auto t0 = Clock::now();
  if (smp) smp->stage("open");
  DappleSession* s = dapple_session_open(raw.c_str(), DAPPLE_QUALITY_FULL, 0);
  if (!s) {
    std::printf("  open failed: %s\n", dapple_last_error());
    return DAPPLE_ERROR;
  }
  if (streaming) *streaming = dapple_session_is_streaming(s) == 1;
  if (smp) smp->stage("export");
  const std::string ej = p.p3 ? R"({"colorSpace":"p3"})" : "{}";
  int rc;
  if (p.hdr) {
    DappleHdrInfo info{};
    rc = dapple_session_export_jpeg_hdr_ex(s, p.params, nullptr, 0, 92, out.c_str(), 0.f, &info, ej.c_str());
  } else {
    rc = dapple_session_export_jpeg_ex(s, p.params, nullptr, 0, 92, out.c_str(), ej.c_str());
  }
  if (rc != DAPPLE_OK) std::printf("  export failed: %s\n", dapple_last_error());
  if (smp) smp->stage("close");
  dapple_session_close(s);
  if (ms) *ms = msSince(t0);
  return rc;
}

static fs::path sampleRaw() {
  for (const char* f : {"build/rawsamples/Fujifilm/X-S10/Fujifilm-X-S10-lossless-compressed.RAF", "build/rawsamples/Sony/ILCE-7M4/ILCE-7M4_DSC06674_FullFrame-LossLess-Compressed-Large.ARW"})
    if (fs::exists(root() / f)) return root() / f;
  return {};
}

static int testCapi() {
  const fs::path raw = std::getenv("DAPPLE_STREAM_TEST_RAW") ? fs::path(std::getenv("DAPPLE_STREAM_TEST_RAW")) : sampleRaw();
  if (raw.empty() || !fs::exists(raw)) {
    std::printf("  SKIP: no RAW sample under build/rawsamples\n");
    return 77;
  }
  const bool gpu = std::getenv("DAPPLE_STREAM_TEST_GPU") && std::getenv("DAPPLE_STREAM_TEST_GPU")[0] == '1';
  if (gpu && dapple_gpu_set_enabled(1) != 1) {
    std::printf("  SKIP: no GPU executor here\n");
    return 77;
  }
  if (!gpu) dapple_gpu_set_enabled(0);
  const fs::path dir = root() / "build" / "exp-out" / (gpu ? "capi_gpu" : "capi_cpu");
  fs::create_directories(dir);
  const char* only = std::getenv("DAPPLE_STREAM_TEST_PRESETS");  // comma list (default: all)
  setEnv("DAPPLE_ENGINE_TILE", "512");  // the whole-frame CPU path at the band height: identical tiling, so identical pixels are expected
  for (const Preset& p : presets()) {
    if (only && !std::strstr(only, p.id)) continue;
    const fs::path a = dir / (std::string(p.id) + "_whole.jpg"), b = dir / (std::string(p.id) + "_stream.jpg");
    setEnv("DAPPLE_EXPORT_STREAM", "off");
    double msA = 0, msB = 0;
    bool stA = true, stB = false;
    const int ra = exportOnce(raw.string(), p, a.string(), &msA, nullptr, &stA);
    setEnv("DAPPLE_EXPORT_STREAM", "on");
    const int rb = exportOnce(raw.string(), p, b.string(), &msB, nullptr, &stB);
    CHECK(ra == DAPPLE_OK && rb == DAPPLE_OK && !stA && stB);
    const auto ba = readAll(a), bb = readAll(b);
    const bool same = !ba.empty() && ba == bb;
    Diff d;
    if (!same) d = diff(decodeJpeg(ba), decodeJpeg(bb));
    std::printf("  %-11s %s: whole %.0f ms, stream %.0f ms: %s", p.id, gpu ? "GPU" : "CPU", msA, msB, same ? "files byte-identical" : "files differ");
    if (!same) std::printf(" (primary image: max %d code, %zu px differ)", d.maxCode, d.differing);
    std::printf("\n");
    CHECK(same || (d.sizeOk && d.maxCode == 0));  // identical pixels; bytes may differ only in a gain map rounding (not expected)
  }
  setEnv("DAPPLE_ENGINE_TILE", nullptr);
  setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  return 0;
}

// ---- cancel -------------------------------------------------------------------------------------------------------------------------------
static int testCancel() {
  const fs::path raw = sampleRaw();
  if (raw.empty()) {
    std::printf("  SKIP: no RAW sample under build/rawsamples\n");
    return 77;
  }
  dapple_gpu_set_enabled(0);
  setEnv("DAPPLE_EXPORT_STREAM", "on");
  const fs::path out = root() / "build" / "exp-out" / "cancel.jpg";
  fs::create_directories(out.parent_path());
  std::error_code ec;
  fs::remove(out, ec);
  DappleSession* s = dapple_session_open(raw.string().c_str(), DAPPLE_QUALITY_FULL, 0);
  CHECK(s && dapple_session_is_streaming(s) == 1);
  if (!s) return 1;
  struct Ctr {
    int calls = 0;
  } ctr;
  dapple_session_set_export_progress(
      s, [](float, const char*, void* u) -> int32_t { return ++static_cast<Ctr*>(u)->calls > 3 ? 1 : 0; }, &ctr);
  const int rc = dapple_session_export_jpeg_ex(s, "{\"clarity\":30}", nullptr, 0, 92, out.string().c_str(), "{}");
  std::printf("  cancel at the 4th progress call: rc %d, error '%s', file %s, temp %s\n", rc, dapple_last_error(), fs::exists(out) ? "PRESENT" : "absent",
              fs::exists(out.string() + ".tmp") ? "PRESENT" : "absent");
  CHECK(rc == DAPPLE_ERROR && std::string(dapple_last_error()) == "cancelled");
  CHECK(!fs::exists(out) && !fs::exists(out.string() + ".tmp"));
  // the session is still usable: the same export without the callback succeeds
  dapple_session_set_export_progress(s, nullptr, nullptr);
  CHECK(dapple_session_export_jpeg_ex(s, "{\"clarity\":30}", nullptr, 0, 92, out.string().c_str(), "{}") == DAPPLE_OK && fs::exists(out));
  dapple_session_close(s);
  setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  return 0;
}

// ---- mem (one measurement per process) ---------------------------------------------------------------------------------------------------
static int cmdMem(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: mem <raw> <preset> [gpu] [stream=on|off|auto] [threads=N]\n");
    return 2;
  }
  const std::string raw = argv[2];
  const Preset* p = findPreset(argv[3]);
  if (!p) {
    std::printf("unknown preset %s\n", argv[3]);
    return 2;
  }
  bool gpu = false;
  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "gpu") == 0) gpu = true;
    if (std::strncmp(argv[i], "stream=", 7) == 0) setEnv("DAPPLE_EXPORT_STREAM", argv[i] + 7);
    if (std::strncmp(argv[i], "threads=", 8) == 0) dapple_perf_configure(std::atoi(argv[i] + 8), 0);
  }
  dapple_gpu_set_enabled(gpu ? 1 : 0);
  const fs::path out = root() / "build" / "exp-out" / (fs::path(raw).stem().string() + "_" + p->id + (gpu ? "_gpu" : "_cpu") + ".jpg");
  fs::create_directories(out.parent_path());
  double ms = 0;
  bool streaming = false;
  mem::Sampler smp;
  const int rc = exportOnce(raw, *p, out.string(), &ms, &smp, &streaming);
  smp.finish();
  const double MB = 1048576.0;
  std::printf("{\"file\":\"%s\",\"preset\":\"%s\",\"gpu\":%d,\"stream\":%d,\"rc\":%d,\"ms\":%.0f,\"baseMB\":%.0f,\"peakMB\":%.0f,\"aboveBaseMB\":%.0f,\"stages\":\"%s\"}\n",
              fs::path(raw).filename().string().c_str(), p->id, gpu ? 1 : 0, streaming ? 1 : 0, rc, ms, double(smp.base()) / MB, double(smp.peak()) / MB,
              double(smp.peak() - smp.base()) / MB, smp.summary().c_str());
  return rc == DAPPLE_OK ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "mem") == 0) return cmdMem(argc, argv);
  if (argc < 2) {
    std::printf("usage: dapple_export_stream_tests <region|reductions|encoder|synthetic|peak|capi|cancel> | mem <raw> <preset> ...\n");
    return 2;
  }
  const std::string id = argv[1];
  std::printf("== export_stream.%s\n", id.c_str());
  int rc = 0;
  if (id == "region") testRegion();
  else if (id == "reductions") testReductions();
  else if (id == "encoder") testEncoder();
  else if (id == "synthetic") testSynthetic();
  else if (id == "peak") testPeak();
  else if (id == "capi") rc = testCapi();
  else if (id == "cancel") rc = testCancel();
  else {
    std::printf("unknown test %s\n", id.c_str());
    return 2;
  }
  if (rc == 77) return 77;
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
  return g_fail ? 1 : rc;
}
