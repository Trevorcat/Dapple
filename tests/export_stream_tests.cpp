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
#include "dapple/film.h"
#include "engine/nodes/film/film_node_internal.h"
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
      {"film_slide", R"({"film":"fujifilm_velvia_100","film_grain":100,"film_halation":100})"},
      {"film_cine", R"({"film":"kodak_vision3_500t","film_grain":100,"film_halation":100})"},
      {"film_local_geo", R"({"film":"kodak_portra_400","film_grain":100,"film_halation":100,"crop":"4:3","rot":90,"straighten":5,"local":{"v":1,"items":[)"
                         R"({"id":"a","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.3,"cy":0.6,"rx":0.2,"ry":0.15,"angle":20,"feather":40}],"adjust":{"exposure":30,"clarity":40}},)"
                         R"({"id":"f","strength":100,"mask":[{"kind":"raster","ref":"stream_r","refine":60}],"adjust":{"exposure":-15}}]}})"},
      {"film_uhdr", R"({"film":"kodak_portra_400","film_grain":100})", true, false},
      {"film_p3", R"({"film":"kodak_vision3_500t"})", false, true},
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
  setEnv("DAPPLE_EXPORT_STREAM_TILE", "512");  // the band height as given (no widening for a large halo): the same tiles as the whole render
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
  if (std::strstr(p.params, "\"film\":\"kodak") || std::strstr(p.params, "\"film\":\"fuji")) loadFilm();
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
  setEnv("DAPPLE_EXPORT_STREAM_TILE", "512");
  setEnv("DAPPLE_FILM_TILES", "1");     // the whole-frame film export in the same tiles (it runs whole by default: section 42.6)
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
  setEnv("DAPPLE_EXPORT_STREAM_TILE", nullptr);
  setEnv("DAPPLE_FILM_TILES", nullptr);
  setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  return 0;
}

// ---- filmgpu: where does a film export differ between the whole frame and bands, GPU and CPU (42.6) -------------------------------------------
// The band export runs the film on the CPU (the GPU film keeps frame-sized images), the whole-frame export on the GPU executor: the difference of
// the two is the GPU / CPU film parity, not a band dependence. Four exports per preset: whole / bands x GPU / CPU; bands (GPU on) must equal the
// CPU exports (film on the CPU in both; the demosaic differs only by its own GPU / CPU parity), and the whole-frame GPU export must stay within
// the film GPU / CPU parity bound of the CPU one.
// mean |a - b| in codes after a 4 x 4 box downscale (grain differs per pixel between the executors; the parity budget of the film GPU tests is on the box)
static double meanAbs4x(const Image8& a, const Image8& b) {
  if (a.width != b.width || a.height != b.height) return 999;
  double sum = 0;
  std::size_t n = 0;
  for (int y = 0; y + 4 <= a.height; y += 4)
    for (int x = 0; x + 4 <= a.width; x += 4)
      for (int c = 0; c < 3; ++c) {
        double ma = 0, mb = 0;
        for (int j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i) {
            const std::size_t k = (std::size_t(y + j) * a.width + x + i) * 3 + c;
            ma += a.data[k], mb += b.data[k];
          }
        sum += std::fabs(ma - mb) / 16.0, ++n;
      }
  return n ? sum / double(n) : 0;
}

static int testFilmGpu() {
  const fs::path raw = std::getenv("DAPPLE_STREAM_TEST_RAW") ? fs::path(std::getenv("DAPPLE_STREAM_TEST_RAW")) : sampleRaw();
  if (raw.empty() || !fs::exists(raw)) {
    std::printf("  SKIP: no RAW sample under build/rawsamples\n");
    return 77;
  }
  if (dapple_gpu_set_enabled(1) != 1) {
    std::printf("  SKIP: no GPU executor here\n");
    return 77;
  }
  const fs::path dir = root() / "build" / "exp-out" / "filmgpu";
  fs::create_directories(dir);
  setEnv("DAPPLE_ENGINE_TILE", "512");
  setEnv("DAPPLE_EXPORT_STREAM_TILE", "512");
  setEnv("DAPPLE_FILM_TILES", "1");
  const Preset cases[] = {{"film", R"({"film":"kodak_portra_400","film_grain":100,"film_halation":100})"},
                          {"film_nograin", R"({"film":"kodak_portra_400","film_grain":0,"film_halation":0})"},
                          {"film_cine", R"({"film":"kodak_vision3_500t","film_grain":100,"film_halation":100})"}};
  const char* only = std::getenv("DAPPLE_STREAM_TEST_PRESETS");
  for (const Preset& p : cases) {
    if (only && !std::strstr(only, p.id)) continue;
    Image8 im[4];  // whole GPU, bands GPU, whole CPU, bands CPU
    bool ok = true;
    for (int k = 0; k < 4; ++k) {
      dapple_gpu_set_enabled(k < 2 ? 1 : 0);
      setEnv("DAPPLE_EXPORT_STREAM", (k & 1) ? "on" : "off");
      const fs::path f = dir / (std::string(p.id) + "_" + std::to_string(k) + ".jpg");
      ok = ok && exportOnce(raw.string(), p, f.string()) == DAPPLE_OK;
      im[k] = decodeJpeg(readAll(f));
    }
    CHECK(ok);
    if (!ok) continue;
    const Diff wc_bc = diff(im[2], im[3]), wg_bg = diff(im[0], im[1]), wg_wc = diff(im[0], im[2]), bg_bc = diff(im[1], im[3]);
    const double mg = meanAbs4x(im[0], im[1]), mw = meanAbs4x(im[0], im[2]), mb = meanAbs4x(im[1], im[3]);
    std::printf("  %-13s CPU whole vs bands: max %d (%zu px) | bands (GPU demosaic, film on the CPU) vs CPU bands: max %d, 4x mean %.3f | whole GPU vs bands: max %d, 4x mean %.3f | "
                "whole GPU vs whole CPU: max %d, 4x mean %.3f\n",
                p.id, wc_bc.maxCode, wc_bc.differing, bg_bc.maxCode, mb, wg_bg.maxCode, mg, wg_wc.maxCode, mw);
    CHECK(wc_bc.sizeOk && wc_bc.maxCode == 0);  // bands are tile-exact on the CPU
    // the band export runs the film on the CPU (42.6), the whole-frame one on the GPU executor: their difference is the film GPU / CPU parity
    // (the same on the CPU and the bands, within the budget of film_gpu_tests: whole film frame mean <= 0.3 after a 4 x downscale), not a band effect
    CHECK(mg <= 1.0 && mw <= 1.0 && std::fabs(mg - mw) <= 0.25);
  }
  dapple_gpu_set_enabled(0);
  for (const char* v : {"DAPPLE_ENGINE_TILE", "DAPPLE_EXPORT_STREAM_TILE", "DAPPLE_FILM_TILES", "DAPPLE_EXPORT_STREAM"}) setEnv(v, nullptr);
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

// ---- policy: the budget fallback and the trim before an export --------------------------------------------------------------------------
static int testPolicy() {
  // 1. the available-memory query fails: a budget from the physical memory, within [256 MB, 1 GB]
  setEnv("DAPPLE_EXPORT_AVAILABLE_FAILS", "1");
  std::string how;
  const std::size_t b = xstream::budgetBytes(&how);
  setEnv("DAPPLE_EXPORT_AVAILABLE_FAILS", nullptr);
  std::printf("  available() failing: budget %.0f MB (%s); physical %.0f MB\n", double(b) / 1048576.0, how.c_str(), double(mem::physical()) / 1048576.0);
  CHECK(how.find("unknown") != std::string::npos && b >= (std::size_t(256) << 20) && b <= (std::size_t(1) << 30));
  std::string how2;
  const std::size_t b2 = xstream::budgetBytes(&how2);
  std::printf("  available() working: budget %.0f MB (%s), available %.0f MB\n", double(b2) / 1048576.0, how2.c_str(), double(mem::available()) / 1048576.0);
  CHECK(how2.rfind("available memory", 0) == 0 && b2 > 0);
  // 2. a RAW the band path cannot read (linear DNG) that does not fit is refused cleanly at the open (auto mode)
  const fs::path lin = root() / "build/rawsamples/Apple/iPhone-12-Pro/IMG_1361.DNG";
  if (fs::exists(lin)) {
    setEnv("DAPPLE_EXPORT_STREAM", "auto");
    setEnv("DAPPLE_EXPORT_BUDGET_MB", "300");
    DappleSession* sl = dapple_session_open(lin.string().c_str(), DAPPLE_QUALITY_FULL, 0);
    const std::string e = sl ? "" : dapple_last_error();
    std::printf("  12 MP linear DNG, budget 300 MB: %s\n", sl ? "opened (WRONG)" : e.c_str());
    CHECK(!sl && e.find("cannot be read in bands") != std::string::npos);
    if (sl) dapple_session_close(sl);
    setEnv("DAPPLE_EXPORT_BUDGET_MB", "600");
    sl = dapple_session_open(lin.string().c_str(), DAPPLE_QUALITY_FULL, 0);
    CHECK(sl != nullptr);
    if (sl) dapple_session_close(sl);
    setEnv("DAPPLE_EXPORT_BUDGET_MB", nullptr);
    setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  }
  // 3. an edit the open did not foresee: a plain-sized FULL session (whole frame) exporting a film stock that does not fit switches to bands
  if (const fs::path xr = sampleRaw(); !xr.empty()) {
    loadFilm();
    const fs::path out = root() / "build" / "exp-out" / "demote.jpg";
    fs::create_directories(out.parent_path());
    setEnv("DAPPLE_EXPORT_STREAM", "auto");
    setEnv("DAPPLE_EXPORT_BUDGET_MB", "600");  // a plain 26 MP export fits (472 MB), a film one does not
    DappleSession* sd = dapple_session_open(xr.string().c_str(), DAPPLE_QUALITY_FULL, 0);
    CHECK(sd && dapple_session_is_streaming(sd) == 0);
    if (sd) {
      CHECK(dapple_session_export_jpeg(sd, R"({"film":"kodak_portra_400"})", nullptr, 0, 92, out.string().c_str()) == DAPPLE_OK);
      std::printf("  whole-frame session + film edit over budget: %s\n", dapple_session_is_streaming(sd) == 1 ? "switched to bands" : "NOT switched");
      CHECK(dapple_session_is_streaming(sd) == 1);
      dapple_session_close(sd);
    }
    setEnv("DAPPLE_EXPORT_BUDGET_MB", nullptr);
    setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  }
  // 4. the trim: another photo's cached mosaic is released, this photo's is kept
  const fs::path a = root() / "build/rawsamples/Sony/ILCE-6700/DSC00002.ARW", c = sampleRaw();
  if (!fs::exists(a) || c.empty()) {
    std::printf("  SKIP trim: samples missing\n");
    return 77;
  }
  DappleSession* sa = dapple_session_open(a.string().c_str(), DAPPLE_QUALITY_PREVIEW, 1024);  // caches the mosaic of a
  CHECK(sa != nullptr);
  if (sa) dapple_session_close(sa);
  std::string why;
  auto z = openFullLazy(c, DecodeOptions{}, &why);  // the mosaic of c
  CHECK(bool(z));
  if (!z) return 1;
  unsigned long long hits0 = 0, miss0 = 0, hits1 = 0, miss1 = 0;
  mosaicCacheCounters(&hits0, &miss0);
  const std::size_t f0 = mem::footprint();
  const std::size_t freed = xstream::trimForExport(z->mosaic->key);
  const std::size_t f1 = mem::footprint();
  auto again = openFullLazy(c, DecodeOptions{}, &why);  // still cached
  mosaicCacheCounters(&hits1, &miss1);
  std::printf("  trim released %.0f MB of cache (footprint %.0f -> %.0f MB); this photo's mosaic still cached: %s\n", double(freed) / 1048576.0, double(f0) / 1048576.0,
              double(f1) / 1048576.0, hits1 > hits0 && miss1 == miss0 ? "yes" : "NO");
  CHECK(freed >= std::size_t(20) << 20);  // the 33 MP mosaic of a (66 MB)
  CHECK(hits1 > hits0 && miss1 == miss0);
  return 0;
}

// ---- budget: the export memory policy on phones (pipeline-architecture.md 42.9), the decision table -----------------------------------------
static int testBudget() {
  constexpr std::size_t MB = std::size_t(1) << 20;
  struct Dev {
    const char* name;
    std::size_t total;  // MemTotal as the phones report it
    std::size_t pool;   // GPU pool budget (vk_device: min(max(deviceLocal / 4, 256 MB), 512 MB) on unified memory)
    bool lowRam;
  } devs[] = {{"3 GB", 2816 * MB, 256 * MB, false}, {"6 GB", 5632 * MB, 512 * MB, false}, {"8 GB", 7578 * MB, 512 * MB, false},
              {"12 GB", 11360 * MB, 512 * MB, false}, {"16 GB", 15258 * MB, 512 * MB, false}};
  struct Pic {
    const char* name;
    int w, h;
  } pics[] = {{"26 MP", 6240, 4160}, {"40 MP", 7752, 5178}, {"61 MP", 9568, 6376}};
  // a quiet phone (MemAvailable = 55 % of the device) with the app idle at 450 MB, and a busy one (MemAvailable = 25 %)
  for (double availShare : {0.55, 0.25}) {
    std::printf("  MemAvailable = %.0f %% of the device, app footprint 450 MB, GPU on (unified memory):\n", availShare * 100);
    std::printf("  %-6s %-8s %-8s %-8s | %-22s | %-22s | %-22s\n", "device", "budget", "ceiling", "room", pics[0].name, pics[1].name, pics[2].name);
    for (const Dev& d : devs) {
      xstream::BudgetInputs in;
      in.available = std::size_t(double(d.total) * availShare);
      in.total = d.total;
      in.footprint = 450 * MB;
      in.lowRam = d.lowRam;
      in.platform = 1;
      const xstream::Budget b = xstream::computeBudget(in);
      std::string cells;
      for (const Pic& p : pics) {
        char c[64];
        const std::size_t std0 = xstream::predictWholePeak(p.w, p.h, false) + xstream::predictGpuExtra(p.w, p.h, false, d.pool);
        const std::size_t film = xstream::predictWholePeak(p.w, p.h, true) + xstream::predictGpuExtra(p.w, p.h, true, d.pool);
        std::snprintf(c, sizeof c, "| %-5s %4.0f / %-5s %4.0f ", std0 <= b.bytes ? "whole" : "bands", double(std0) / MB, film <= b.bytes ? "whole" : "bands",
                      double(film) / MB);
        cells += c;
      }
      std::printf("  %-6s %-8.0f %-8.0f %-8.0f %s\n", d.name, double(b.bytes) / MB, double(b.ceiling) / MB, double(b.room) / MB, cells.c_str());
      if (availShare > 0.5) {
        if (d.total >= 11000 * MB) {
          CHECK(b.bytes == 1536 * MB);  // the ceiling, not 2.6 GB from MemAvailable
          CHECK(xstream::predictWholePeak(7752, 5178, true) > b.bytes);  // the Note10+ case: 40 MP film streams
          CHECK(xstream::predictWholePeak(7752, 5178, false) + xstream::predictGpuExtra(7752, 5178, false, d.pool) <= b.bytes);  // 40 MP std stays whole (X-Trans: bands cost 1.4-2x)
        }
        if (d.total < 3000 * MB) CHECK(xstream::predictWholePeak(7752, 5178, false) > b.bytes);  // a 3 GB phone streams 40 MP
      }
    }
  }
  // the old rule (no ceiling) on the desktop: unchanged
  xstream::BudgetInputs desk;
  desk.available = 8000 * MB, desk.total = 32000 * MB, desk.footprint = 500 * MB;
  const xstream::Budget bd = xstream::computeBudget(desk);
  CHECK(bd.bytes == 4000 * MB - 96 * MB);
  // low-RAM devices: 384 MB at most, the row cache 96 MB
  xstream::BudgetInputs lr;
  lr.available = 1500 * MB, lr.total = 2000 * MB, lr.footprint = 200 * MB, lr.lowRam = true, lr.platform = 1;
  const xstream::Budget bl = xstream::computeBudget(lr);
  std::printf("  low-RAM 2 GB: %s\n", bl.how.c_str());
  CHECK(bl.bytes <= 250 * MB && bl.rowCacheCap == 96 * MB);
  return 0;
}

// ---- gpuoom: a GPU allocation failure during a whole-frame export switches the export to bands (42.9) ------------------------------------------------
static int testGpuOom() {
  const fs::path xr = sampleRaw();
  if (xr.empty()) {
    std::printf("  SKIP: sample missing\n");
    return 77;
  }
  if (dapple_gpu_set_enabled(1) != 1) {
    std::printf("  SKIP: no GPU executor here\n");
    return 77;
  }
  const fs::path out = root() / "build" / "exp-out" / "gpuoom.jpg";
  fs::create_directories(out.parent_path());
  fs::remove(out);
  setEnv("DAPPLE_EXPORT_STREAM", "auto");
  setEnv("DAPPLE_EXPORT_BUDGET_MB", "20000");  // the whole frame fits the CPU budget: only the GPU failure can switch it
  DappleSession* s = dapple_session_open(xr.string().c_str(), DAPPLE_QUALITY_FULL, 0);
  CHECK(s && dapple_session_is_streaming(s) == 0);
  if (s) {
    setEnv("DAPPLE_EXPORT_GPU_OOM", "1");
    const int rc = dapple_session_export_jpeg(s, "{}", nullptr, 0, 92, out.string().c_str());
    setEnv("DAPPLE_EXPORT_GPU_OOM", nullptr);
    std::printf("  whole-frame session, GPU allocation fails: rc %d, %s, file %s\n", rc, dapple_session_is_streaming(s) == 1 ? "switched to bands" : "NOT switched",
                fs::exists(out) ? "written" : "MISSING");
    CHECK(rc == DAPPLE_OK && dapple_session_is_streaming(s) == 1 && fs::exists(out) && fs::file_size(out) > 100000);
    dapple_session_close(s);
  }
  setEnv("DAPPLE_EXPORT_BUDGET_MB", nullptr);
  setEnv("DAPPLE_EXPORT_STREAM", nullptr);
  return 0;
}

// ---- memtrim: the memory report and the trim levels (dapple_memory_report_json / dapple_memory_trim) -------------------------------------------------
static int testMemTrim() {
  const fs::path a = sampleRaw();
  if (a.empty()) {
    std::printf("  SKIP: sample missing\n");
    return 77;
  }
  CHECK(dapple_memory_configure(R"({"totalBytes":12000000000,"memoryClassMb":256,"largeMemoryClassMb":512,"lowRam":false})") == DAPPLE_OK);
  DappleSession* s = dapple_session_open(a.string().c_str(), DAPPLE_QUALITY_PREVIEW, 1024);  // caches the mosaic
  CHECK(s != nullptr);
  auto report = [] {
    char* j = dapple_memory_report_json();
    std::string r = j ? j : "";
    dapple_free_string(j);
    return r;
  };
  const std::string r1 = report();
  std::printf("  open: %s\n", r1.c_str());
  CHECK(r1.find("\"sessions\":{\"count\":1") != std::string::npos);
  CHECK(r1.find("\"deviceTotal\":12000000000") != std::string::npos);
  if (s) dapple_session_close(s);
  const std::string r2 = report();
  CHECK(r2.find("\"sessions\":{\"count\":0") != std::string::npos);
  CHECK(r2.find("\"mosaicCache\":{\"bytes\":0") == std::string::npos);  // the mosaic is still cached after the close
  const long long f0 = dapple_memory_trim(0);
  const std::string r3 = report();
  std::printf("  trim 0 released %.0f MB: %s\n", double(f0) / 1048576.0, r3.c_str());
  CHECK(f0 >= 0);
  const long long f1 = dapple_memory_trim(1);
  const std::string r4 = report();
  std::printf("  trim 1 released %.0f MB: %s\n", double(f1) / 1048576.0, r4.c_str());
  CHECK(r4.find("\"mosaicCache\":{\"bytes\":0") != std::string::npos);
  CHECK(dapple_memory_trim(2) >= 0);
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

// ---- mixbands: film Gaussian mixtures in row bands == the whole-plane mixtures, bit for bit ---------------------------------------------
static int BANDH = 256;
static void testMixBands() {
  if (const char* e = std::getenv("BANDH")) BANDH = std::atoi(e);
  loadFilm();
  namespace fd = dapple::film::detail;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  for (float fscale : {0.25f, 1.f, 4.f})
  for (const auto& sz : {std::pair<int, int>{3001, 2003}, std::pair<int, int>{1333, 2777}}) {
    const int W = sz.first, H = sz.second;
    engine::filmnode::FilmFrameArgs fa;
    const auto lib = film::defaultLibrary();
    fa.lib = lib.get(), fa.stock = "kodak_portra_400", fa.paper = "kodak_portra_endura", fa.w = W, fa.h = H;
    fa.formatScale = fscale;  // a 61 MP pixel pitch on a smaller frame: the wide components reach level 5+
    engine::filmnode::FilmResolved r;
    CHECK(engine::filmnode::filmFrameResolved(fa, r));
    const auto pd = engine::filmnode::planOf(r);
    std::vector<float> src(std::size_t(W) * H), half(std::size_t((W + 1) / 2) * ((H + 1) / 2));
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) src[std::size_t(y) * W + x] = 0.3f + 0.2f * std::sin(x * 0.013f + y * 0.007f) + float(hash32(std::uint32_t(x * 7919 + y * 104729)) & 1023) * 1e-4f;
    for (std::size_t i = 0; i < half.size(); ++i) half[i] = src[i];
    struct Case { const fd::Mixture* m; bool half; const char* n; };
    const Case cases[] = {{&pd->plan.halo[0], false, "halo R"}, {&pd->plan.halo[2], false, "halo B"}, {&pd->plan.dir[0], true, "dir (half)"}};
    for (const Case& c : cases) {
      if (c.half && !fd::mixtureFitsHalf(*c.m)) continue;  // the film runs a half-resolution DIR field only for such mixtures (FilmPlanData::dirHalf)
      fd::Arena arena;
      const int iw = c.half ? (W + 1) / 2 : W, ih = c.half ? (H + 1) / 2 : H;
      const float* in = c.half ? half.data() : src.data();
      fd::Plane ip{const_cast<float*>(in), iw, ih, iw};
      std::vector<float> whole(std::size_t(W) * H), band(std::size_t(W) * H, -1.f);
      fd::Plane op{whole.data(), W, H, W};
      if (c.half) fd::applyMixtureHalf(K, arena, ip, op, *c.m);
      else fd::applyMixture(K, arena, ip, op, *c.m);
      {  // the whole-plane function is deterministic (a second run gives the same bits)
        std::vector<float> again(whole.size());
        fd::Plane op2{again.data(), W, H, W};
        fd::Arena arena2;
        if (c.half) fd::applyMixtureHalf(K, arena2, ip, op2, *c.m);
        else fd::applyMixture(K, arena2, ip, op2, *c.m);
        const bool det = std::memcmp(again.data(), whole.data(), whole.size() * 4) == 0;
        if (!det) std::printf("    whole-plane %s is NOT deterministic\n", c.n);
        CHECK(det);
      }
      fd::MixtureBands mb;
      mb.setup(*c.m, W, H, c.half);
      auto rowAt = [&](int y) -> const float* { return in + std::size_t(y) * iw; };
      if (mb.needsPrepare())
        for (int y = 0; y < ih; y += 301) mb.feed(K, y, std::min(ih, y + 301), rowAt);
      CHECK(mb.prepared());
      int maxIn = 0;
      for (int a = 0; a < H; a += BANDH) {
        const int b = std::min(H, a + BANDH);
        int ia, ib;
        mb.inputRange(a, b, ia, ib);
        maxIn = std::max(maxIn, ib - ia);
        mb.rows(K, a, b, [&](int y) -> const float* { CHECK(y >= ia && y < ib); return rowAt(y); }, [&](int y) { return band.data() + std::size_t(y) * W; });
      }
      const bool same = std::memcmp(whole.data(), band.data(), whole.size() * 4) == 0;
      if (!same) {
        double md = 0;
        int fy = -1, fx = -1;
        for (int y = 0; y < H; ++y)
          for (int x = 0; x < W; ++x) {
            const double d = std::fabs(double(whole[std::size_t(y) * W + x]) - band[std::size_t(y) * W + x]);
            if (std::memcmp(&whole[std::size_t(y) * W + x], &band[std::size_t(y) * W + x], 4) != 0 && fy < 0) fy = y, fx = x;
            md = std::max(md, d);
          }
        std::printf("    max |diff| %.3g, first at (%d, %d): whole %.6g band %.6g\n", md, fx, fy, whole[std::size_t(fy) * W + fx], band[std::size_t(fy) * W + fx]);
      }
      std::printf("  %dx%d %-10s: bands of 256 rows read <= %d input rows, levels kept %.1f MB: %s\n", W, H, c.n, maxIn, double(mb.wholeBytes()) / 1048576.0,
                  same ? "bit-identical" : "DIFFERENT");
      CHECK(same);
    }
  }
}

// ---- filmbands: the film in row bands == runFilmFrame of the whole frame, bit for bit ------------------------------------------------
static void testFilmBands() {
  loadFilm();
  const auto lib = film::defaultLibrary();
  struct S {
    const char* stock;
    const char* paper;
  };
  const S stocks[] = {{"kodak_portra_400", "kodak_portra_endura"}, {"fujifilm_velvia_100", "none"}, {"kodak_vision3_500t", "kodak_2383"}};
  for (const S& st : stocks)
    for (float fscale : {1.f, 3.f}) {
      const int W = 2400, H = 1601;
      engine::filmnode::FilmFrameArgs a;
      a.lib = lib.get(), a.stock = st.stock, a.paper = st.paper, a.w = W, a.h = H, a.formatScale = fscale;
      a.gain[0] = 1.8f, a.gain[1] = 1.f, a.gain[2] = 1.4f;
      a.quality = 2, a.grainPct = 100, a.halationPct = 100;
      std::vector<float> in(std::size_t(W) * H * 3), whole(in.size()), band(in.size(), -1.f);
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          const float u = float(x) / W, v = float(y) / H, n = float(hash32(std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) & 255) / 2550.f;
          float* p = in.data() + (std::size_t(y) * W + x) * 3;
          p[0] = 0.02f + 0.5f * u * u + n, p[1] = 0.03f + 0.4f * v + n, p[2] = 0.02f + 0.3f * (1 - u) * v + n;
          if ((u - 0.6f) * (u - 0.6f) + (v - 0.4f) * (v - 0.4f) < 0.002f) p[0] = p[1] = p[2] = 4.f;  // a highlight: halation
        }
      CHECK(engine::filmnode::renderFilmFrame(a, in.data(), std::size_t(W) * 3, whole.data(), std::size_t(W) * 3));
      engine::filmnode::FilmBands fb;
      std::string why;
      int inRows = 0;
      const bool ok = fb.init(a, [&](int y0, int y1, float* rgb) {
        if (!rgb) return true;  // prefetch of the source rows: nothing to do here
        inRows += y1 - y0;
        std::memcpy(rgb, in.data() + std::size_t(y0) * W * 3, std::size_t(y1 - y0) * W * 3 * 4);
        return true;
      }, &why);
      CHECK(ok);
      if (!ok) {
        std::printf("  %s: %s\n", st.stock, why.c_str());
        continue;
      }
      const int pre = inRows;
      for (int y = 0; y < H; y += 200) CHECK(fb.rows(y, std::min(H, y + 200), band.data() + std::size_t(y) * W * 3, std::size_t(W) * 3));
      const bool same = std::memcmp(whole.data(), band.data(), whole.size() * 4) == 0;
      std::size_t ndiff = 0;
      for (std::size_t i = 0; i < whole.size(); ++i) ndiff += std::memcmp(&whole[i], &band[i], 4) != 0;
      std::printf("  %-20s format x%.0f %dx%d: %d pre-pass(es) (%.1f input frames), render read %.2f input frames, kept %.1f MB: %s (%zu values differ)\n", st.stock,
                  double(fscale), W, H, fb.passes(), double(pre) / H, double(inRows - pre) / H, double(fb.keptBytes()) / 1048576.0, same ? "bit-identical" : "DIFFERENT", ndiff);
      CHECK(same);
    }
}

// ---- filmplan (diagnostics): the spatial plan of a stock at a frame size -------------------------------------------------------------
static int cmdFilmPlan(int argc, char** argv) {
  loadFilm();
  const char* stock = argc > 2 ? argv[2] : "kodak_portra_400";
  const char* paper = argc > 3 ? argv[3] : "kodak_portra_endura";
  const int w = argc > 4 ? std::atoi(argv[4]) : 9504, h = argc > 5 ? std::atoi(argv[5]) : 6336;
  engine::filmnode::FilmFrameArgs a;
  const auto lib = film::defaultLibrary();
  a.lib = lib.get();
  a.stock = stock;
  a.paper = paper;
  a.w = w, a.h = h;
  engine::filmnode::FilmResolved r;
  if (!engine::filmnode::filmFrameResolved(a, r)) {
    std::printf("cannot resolve %s / %s\n", stock, paper);
    return 1;
  }
  const auto pd = engine::filmnode::planOf(r);
  const auto& pl = pd->plan;
  std::printf("%s / %s %dx%d: err '%s' boost %d preMix %d haloMix %d dir %d dirHalf %d split %d scanStage %d glare %d Hs %d Hg %d Hd %d grain %d\n", stock, paper,
              w, h, pd->error.c_str(), pl.boost, pl.preMix, pl.haloMix, pl.dirDiffuse, pd->dirHalf, pd->split, pd->scanStage, pl.glare, pd->Hs, pd->Hg, pd->Hd,
              pl.grain.active);
  auto pm = [](const char* n, const film::detail::Mixture* m) {
    for (int c = 0; c < 3; ++c) {
      std::printf("  %s[%d] w0 %.3f:", n, c, m[c].w0);
      for (const auto& g : m[c].comps) std::printf(" (w %.3f s %.1f)", g.w, g.sigma);
      std::printf("\n");
    }
  };
  if (pl.preMix) pm("pre", pl.pre);
  if (pl.haloMix) pm("halo", pl.halo);
  if (pl.dirDiffuse) pm("dir", pl.dir);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "filmplan") == 0) return cmdFilmPlan(argc, argv);
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
  else if (id == "mixbands") testMixBands();
  else if (id == "filmbands") testFilmBands();
  else if (id == "synthetic") testSynthetic();
  else if (id == "peak") testPeak();
  else if (id == "capi") rc = testCapi();
  else if (id == "filmgpu") rc = testFilmGpu();
  else if (id == "cancel") rc = testCancel();
  else if (id == "policy") rc = testPolicy();
  else if (id == "budget") rc = testBudget();
  else if (id == "memtrim") rc = testMemTrim();
  else if (id == "gpuoom") rc = testGpuOom();
  else {
    std::printf("unknown test %s\n", id.c_str());
    return 2;
  }
  if (rc == 77) return 77;
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
  return g_fail ? 1 : rc;
}
