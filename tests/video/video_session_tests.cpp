// VideoFrameSession (core/engine/video, docs/engine/pipeline-architecture.md section 30): a Motion Photo clip through the same node graph as the still.
//   convert  the frame -> working space step (grey stays grey, rotation, aspect crop, half floats)
//   oracle   frame N through the session == the still pipeline (the oracle) on the same pixels and edit
//   hold     film match / tone analysis / solves are held from the reference frame, never per frame
//   depth10  10-bit in, 10-bit out: distinct code values survive the graph (needs the desktop FFmpeg for the fixture decode)
//   flicker  static clip, grain off: frame-to-frame difference is zero
//   grain    grain on: it changes per frame, is deterministic per frame, the mean stays put
//   perf     ms per 1080p frame on the CPU executor (and the GPU executor when the build has it)
//   e2e      decode a fixture clip (S-Log3, HLG, 8-bit), render every frame with film, check ranges
//   verify   <file.mkv>: decode the CLI output back and count distinct code values (used by the ctest of `dapple motion render`)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "engine/video/video_frame_session.h"

#ifdef DAPPLE_HAVE_VIDEO_HOST
#include "dapple_video.h"
#endif

using namespace dapple;
using namespace dapple::engine;
using namespace dapple::engine::video;

static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- helpers --------------------------------------------------------------------------------------------------------------------
static std::uint16_t toHalf(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, 4);
  const std::uint32_t s = (b >> 16) & 0x8000u;
  int e = int((b >> 23) & 0xff) - 127 + 15;
  std::uint32_t m = b & 0x7fffffu;
  if (e <= 0) {
    if (e < -10) return std::uint16_t(s);
    m |= 0x800000u;
    const std::uint32_t t = std::uint32_t(14 - e);
    return std::uint16_t(s | ((m >> t) + ((m >> (t - 1)) & 1u)));
  }
  if (e >= 31) return std::uint16_t(s | 0x7bffu);
  std::uint32_t r = s | (std::uint32_t(e) << 10) | (m >> 13);
  if (m & 0x1000u) ++r;
  return std::uint16_t(r);
}

static float hash01(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return float(x & 0xffffff) / float(0x1000000);
}

// A synthetic photograph: sky gradient, a darker foreground, a few colour patches and a grey card, fine texture; `shift` moves the scene a few pixels
// (the next frame of a slow pan), `lum` scales the light (a cloud passes).
static std::vector<std::uint16_t> makeFrame(int w, int h, int shift = 0, float lum = 1.f, float noise = 0.01f, std::uint32_t seed = 1) {
  std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x + shift) / float(w), v = float(y) / float(h);
      float r, g, b;
      if (v < 0.55f) {  // sky
        const float t = v / 0.55f;
        r = 0.25f + 0.35f * t;
        g = 0.45f + 0.25f * t;
        b = 0.95f - 0.25f * t;
      } else {  // ground, darker, with a warm band
        const float t = (v - 0.55f) / 0.45f;
        r = 0.22f * (1.f - 0.6f * t) + 0.08f * std::sin(u * 9.f);
        g = 0.17f * (1.f - 0.6f * t);
        b = 0.09f * (1.f - 0.5f * t);
      }
      const int px0 = int(u * 8.f), py0 = int(v * 6.f);
      if (py0 == 4 && px0 >= 1 && px0 <= 6) {  // patches
        static const float P[6][3] = {{0.7f, 0.1f, 0.1f}, {0.1f, 0.6f, 0.15f}, {0.15f, 0.2f, 0.7f}, {0.9f, 0.85f, 0.1f}, {0.18f, 0.18f, 0.18f}, {0.8f, 0.8f, 0.8f}};
        r = P[px0 - 1][0], g = P[px0 - 1][1], b = P[px0 - 1][2];
      }
      const float n = noise * (hash01(std::uint32_t(x * 7919 + y * 104729) ^ seed) - 0.5f);
      std::uint16_t* p = px.data() + (std::size_t(y) * w + x) * 4;
      p[0] = toHalf(std::max(0.f, (r + n) * lum));
      p[1] = toHalf(std::max(0.f, (g + n) * lum));
      p[2] = toHalf(std::max(0.f, (b + n) * lum));
      p[3] = 0x3c00;
    }
  return px;
}

static Config baseConfig() {
  Config c;
  c.camera = VideoFrameSession::videoCamera();
  return c;
}

// DAPPLE_SRC overrides the compiled-in source dir (on a device: a directory holding core/data/film/dapple.dfp and tests/data/video)
static std::string srcDir() {
  const char* e = std::getenv("DAPPLE_SRC");
  return e && *e ? e : DAPPLE_SOURCE_DIR;
}

static bool loadFilm(const std::string& srcDir) {
  std::string err;
  const fs::path pack = fs::path(srcDir) / "core" / "data" / "film" / "dapple.dfp";
  return film::setLibraryPath(pack, &err);
}

static double meanAbsDiff8(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int* maxd = nullptr) {
  double s = 0;
  int m = 0;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 3) == 3) continue;  // alpha
    const int d = std::abs(int(a[i]) - int(b[i]));
    s += d;
    m = std::max(m, d);
  }
  if (maxd) *maxd = m;
  return s / (double(n) * 0.75);
}

static double meanOf(const ImageF& im) {
  double s = 0;
  for (float v : im.data) s += v;
  return s / double(im.data.size());
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------
static int testConvert() {
  Config cfg = baseConfig();
  // grey stays grey and keeps its level in ProPhoto
  {
    std::vector<std::uint16_t> px(16 * 16 * 4, 0);
    for (int i = 0; i < 16 * 16; ++i) px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = toHalf(0.18f), px[4 * i + 3] = 0x3c00;
    InputFrame f{px.data(), 16, 16, ColourKind::Standard, 0};
    SourceImage s;
    CHECK(VideoFrameSession::toSource(f, cfg, s), "toSource");
    const float* p = s.linear.row(5) + 3 * 5;
    CHECK(std::fabs(p[0] - 0.18f) < 2e-3f && std::fabs(p[1] - 0.18f) < 2e-3f && std::fabs(p[2] - 0.18f) < 2e-3f, "grey 0.18 -> %.4f %.4f %.4f", p[0], p[1], p[2]);
    CHECK(!s.sceneReferred, "SDR video is display referred like a JPEG");
    f.kind = ColourKind::Slog3;
    SourceImage s2;
    VideoFrameSession::toSource(f, cfg, s2);
    CHECK(s2.sceneReferred, "S-Log3 is scene referred like a RAW");
    CHECK(halfToFloat(toHalf(0.18f)) == halfToFloat(px[0]), "half table");
    CHECK(halfToFloat(0x3c00) == 1.f && halfToFloat(0xc000) == -2.f && halfToFloat(0x0001) > 0.f, "half table special values");
  }
  // rotation: a red marker at the top-left of a 32x16 frame ends at the top-right of the 16x32 result when rotated 90 degrees clockwise
  {
    const int w = 32, h = 16;
    std::vector<std::uint16_t> px(std::size_t(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i) px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = toHalf(0.05f);
    px[0] = toHalf(1.f);  // (0,0) red channel high
    for (int rot : {90, 180, 270}) {
      InputFrame f{px.data(), w, h, ColourKind::Standard, rot};
      SourceImage s;
      CHECK(VideoFrameSession::toSource(f, cfg, s), "rotate toSource");
      const int dw = s.linear.width, dh = s.linear.height;
      CHECK((rot == 180 && dw == w && dh == h) || (rot != 180 && dw == h && dh == w), "rotated size %dx%d", dw, dh);
      int bx = -1, by = -1;
      float best = 0;
      for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
          const float r = s.linear.row(y)[3 * x];
          if (r > best) best = r, bx = x, by = y;
        }
      const int ex = rot == 90 ? dw - 1 : rot == 180 ? dw - 1 : 0, ey = rot == 90 ? 0 : rot == 180 ? dh - 1 : dh - 1;
      CHECK(bx == ex && by == ey, "marker after %d deg at (%d,%d), expected (%d,%d)", rot, bx, by, ex, ey);
    }
  }
  // aspect fit: a 16:9 frame against a 4:3 still is centre cropped, never stretched
  {
    const int w = 160, h = 90;
    auto px = makeFrame(w, h);
    Config c2 = cfg;
    c2.stillAspect = 4.f / 3.f;
    InputFrame f{px.data(), w, h, ColourKind::Standard, 0};
    SourceImage s;
    CHECK(VideoFrameSession::toSource(f, c2, s), "aspect toSource");
    CHECK(s.linear.height == 90 && s.linear.width == 120, "4:3 crop of 160x90 -> %dx%d", s.linear.width, s.linear.height);
    c2.maxLongEdge = 60;
    SourceImage s2;
    VideoFrameSession::toSource(f, c2, s2);
    CHECK(std::max(s2.linear.width, s2.linear.height) == 60, "max long edge -> %dx%d", s2.linear.width, s2.linear.height);
  }
  std::printf("convert: ok\n");
  return 0;
}

struct EditCase {
  const char* name;
  EditParams p;
};
static std::vector<EditCase> editCases(bool filmOk) {
  std::vector<EditCase> v;
  EditParams d;
  v.push_back({"default", d});
  EditParams t;
  t.exposure = 0.4f;
  t.contrast = 20.f;
  t.shadows = 30.f;
  t.vibrance = 20.f;
  t.crop = "16:9";
  v.push_back({"tone+crop", t});
  EditParams det;
  det.dehaze = 25.f;
  det.clarity = 20.f;
  det.sharpen = 30.f;
  v.push_back({"dehaze+clarity+sharpen", det});
  if (filmOk) {
    EditParams f;
    f.filmStock = "kodak_portra_400";
    v.push_back({"film portra", f});
    EditParams g = f;
    g.filmStock = "kodak_ektachrome_e100";
    g.rotate = 90;
    v.push_back({"film slide + rotate", g});
  }
  return v;
}

static int testOracle(bool filmOk) {
  const int W = 320, H = 180;
  for (const EditCase& ec : editCases(filmOk)) {
    Config cfg = baseConfig();
    cfg.params = ec.p;
    auto A = makeFrame(W, H, 0, 1.f, 0.01f, 1);
    InputFrame fa{A.data(), W, H, ColourKind::Standard, 0};
    // session: reference = frame A, render frame A as 8-bit
    VideoFrameSession s(cfg);
    CHECK(s.setReference(fa), "%s: reference", ec.name);
    std::vector<std::uint8_t> got;
    FrameInfo fi;
    std::string err;
    CHECK(s.render(fa, 0, PackFormat::Rgba8, got, &fi, &err), "%s: render %s", ec.name, err.c_str());
    // the still pipeline on the same pixels (the oracle): conversion shared, everything else independent
    SourceImage still;
    VideoFrameSession::toSource(fa, cfg, still);
    SourceImage proxy;
    proxy.sceneReferred = still.sceneReferred;
    proxy.linear = resizeToLongEdge(still.linear, 256);
    EditParams p = cfg.params;
    p.filmKeyEv = p.filmPrintEv = 0.f;
    if (!p.filmStock.empty()) {
      const FilmExposure ev = matchFilmExposure(p, cfg.camera, nullptr, proxy);
      p.filmKeyEv = ev.filmEv;
      p.filmPrintEv = ev.printEv;
    }
    Renderer renderer(p, cfg.camera, nullptr);
    renderer.setToneProxy(&proxy);
    EngineSession es;
    FrameJob job{p, renderer, still, &proxy, cfg.camera, nullptr, {}, {}, {}};
    job.request.bakeSize = 65;
    job.request.noise = true;
    job.fastOptions.spatial = SpatialQuality::Full;
    FrameResult res;
    CHECK(es.renderFrame(job, res), "%s: still frame %s", ec.name, res.error.c_str());
    std::vector<std::uint8_t> ref;
    if (res.rgba) ref.assign(res.rgba, res.rgba + std::size_t(res.w) * res.h * 4);
    std::free(res.rgba);
    CHECK(fi.width == res.w && fi.height == res.h && got.size() == ref.size(), "%s: size %dx%d vs %dx%d", ec.name, fi.width, fi.height, res.w, res.h);
    int maxd = 0;
    const double mean = meanAbsDiff8(got, ref, &maxd);
    // The session's float result is rounded to 8 bit; the engine's own pack of the same float differs by at most one code.
    CHECK(maxd <= 1, "%s: session vs still pipeline max diff %d (mean %.4f)", ec.name, maxd, mean);
    // against the exact variant (per pixel tone + colour): the baked path's own approximation, not the session's
    EngineSession es2;
    Image8 exact;
    std::string e2;
    double meanExact = -1;
    int maxExact = 0;
    if (es2.renderExact(p, renderer, still, EngineSession::ExactOptions{}, exact, &e2)) {
      std::vector<std::uint8_t> ex(std::size_t(exact.width) * exact.height * 4, 255);
      for (std::size_t i = 0, n = std::size_t(exact.width) * exact.height; i < n; ++i)
        for (int c = 0; c < 3; ++c) ex[4 * i + c] = exact.data[3 * i + c];
      if (ex.size() == got.size()) meanExact = meanAbsDiff8(got, ex, &maxExact);
    }
    std::printf("oracle %-26s: %dx%d  session vs still pipeline max %d (mean %.4f)   vs exact variant mean %.3f max %d  [%s]\n", ec.name, fi.width, fi.height, maxd, mean, meanExact,
                maxExact, fi.executor.c_str());
    CHECK(meanExact < 0 || meanExact < 1.2, "%s: baked vs exact mean %.3f", ec.name, meanExact);
  }
  return 0;
}

static int testHold(bool filmOk) {
  const int W = 320, H = 180;
  Config cfg = baseConfig();
  cfg.params.dehaze = 30.f;
  cfg.params.toneStrength = 60.f;
  if (filmOk) cfg.params.filmStock = "kodak_portra_400";
  auto A = makeFrame(W, H, 0, 1.f);
  // frame B: the light drops (a cloud): a per-frame film match / tone analysis would move, the held one must not
  auto B = makeFrame(W, H, 6, 0.35f, 0.01f, 2);
  InputFrame fa{A.data(), W, H, ColourKind::Standard, 0}, fb{B.data(), W, H, ColourKind::Standard, 0};
  VideoFrameSession s(cfg);
  CHECK(s.setReference(fa), "reference");
  const float key0 = s.heldParams().filmKeyEv, print0 = s.heldParams().filmPrintEv;
  const auto st0 = s.engine().stats();
  ImageF out;
  FrameInfo fi;
  std::string err;
  CHECK(s.renderFloat(fb, 1, out, &fi, &err), "frame B %s", err.c_str());
  const auto st1 = s.engine().stats();
  CHECK(fi.tablesHit, "frame B must hit the cached tables (latticeBake %llu -> %llu)", (unsigned long long)st0.latticeBake, (unsigned long long)st1.latticeBake);
  CHECK(st1.latticeBake == st0.latticeBake && st1.analysisBuilds == st0.analysisBuilds && st1.toneTableBuilds == st0.toneTableBuilds,
        "no table was rebuilt for frame B (lattice %llu->%llu, analysis %llu->%llu, tone %llu->%llu)", (unsigned long long)st0.latticeBake, (unsigned long long)st1.latticeBake,
        (unsigned long long)st0.analysisBuilds, (unsigned long long)st1.analysisBuilds, (unsigned long long)st0.toneTableBuilds, (unsigned long long)st1.toneTableBuilds);
  CHECK(s.heldParams().filmKeyEv == key0 && s.heldParams().filmPrintEv == print0, "film EV changed per frame");
  std::printf("hold: frame-global solves: reference built %llu (hits %llu); frame B built %llu more, hit %llu\n", (unsigned long long)st0.solveBuilds, (unsigned long long)st0.solveHits,
              (unsigned long long)(st1.solveBuilds - st0.solveBuilds), (unsigned long long)(st1.solveHits - st0.solveHits));
  CHECK(st1.solveBuilds == st0.solveBuilds, "the dehaze / detail solve must not be rebuilt for frame B (%llu -> %llu)", (unsigned long long)st0.solveBuilds, (unsigned long long)st1.solveBuilds);
  // a fresh pipeline that analysed frame B itself would differ: this is what 'held' means
  Config cfg2 = cfg;
  VideoFrameSession s2(cfg2);
  CHECK(s2.setReference(fb), "reference B");
  ImageF outFresh;
  CHECK(s2.renderFloat(fb, 1, outFresh, nullptr, &err), "fresh B");
  const double mHeld = meanOf(out), mFresh = meanOf(outFresh);
  std::printf("hold: film EV key %.3f print %.3f held for the clip; frame B mean %.4f held vs %.4f re-analysed (a cloud: the held clip follows the light, not the meter); "
              "solveHits %llu\n", key0, print0, mHeld, mFresh, (unsigned long long)s.engine().stats().solveHits);
  CHECK(std::fabs(mHeld - mFresh) > 1e-4 || !filmOk, "held and re-analysed frames should differ for a lighting change");
  // the held result is exactly what an engine session that rendered A first and then B produces
  CHECK(out.width == outFresh.width && out.height == outFresh.height, "size");
  return 0;
}

static int testDepth10() {
#ifdef DAPPLE_HAVE_VIDEO_HOST
  const std::string dir = srcDir() + "/tests/data/video/";
  auto distinctLuma = [](const std::vector<std::uint8_t>& p010, int w, int h) {
    std::set<int> s;
    const auto* y = reinterpret_cast<const std::uint16_t*>(p010.data());
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) s.insert(y[i] >> 6);
    return int(s.size());
  };
  char err[256] = {0};
  DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
  DvInfo info{};
  DvVideo* v = dv_open((dir + "gradient_hevc_main10.mp4").c_str(), &oo, &info, err, sizeof err);
  CHECK(v != nullptr, "open gradient: %s", err);
  if (!v) return 0;
  int w = 0, h = 0;
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_P010, 0, &w, &h) == 0, "begin p010");
  std::vector<std::uint8_t> src(std::size_t(dv_frame_bytes(v)));
  CHECK(dv_next(v, src.data(), std::int64_t(src.size())) >= 0, "next p010");
  const int srcDistinct = distinctLuma(src, w, h);
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBAF16, 0, &w, &h) == 0, "begin f16");
  std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
  CHECK(dv_next(v, reinterpret_cast<std::uint8_t*>(px.data()), std::int64_t(px.size()) * 2) >= 0, "next f16");
  dv_close(v);
  Config cfg = baseConfig();  // default edit: the whole graph runs (tone, colour, ...), only nothing is dialled in
  VideoFrameSession s(cfg);
  InputFrame f{px.data(), w, h, static_cast<ColourKind>(info.colorKind), info.rotation};
  std::vector<std::uint8_t> out10, out8;
  std::string e;
  CHECK(s.render(f, 1, PackFormat::P010, out10, nullptr, &e), "render p010 %s", e.c_str());
  CHECK(s.render(f, 1, PackFormat::I420, out8, nullptr, &e), "render i420 %s", e.c_str());
  const int d10 = distinctLuma(out10, s.outputWidth(), s.outputHeight());
  std::set<int> d8;
  for (std::size_t i = 0; i < std::size_t(s.outputWidth()) * s.outputHeight(); ++i) d8.insert(out8[i]);
  std::printf("depth10: source %d distinct 10-bit luma codes (%dx%d, %d-bit stream); graph output P010 %d distinct codes; 8-bit control %zu\n", srcDistinct, w, h, info.bitDepth, d10,
              d8.size());
  CHECK(info.bitDepth == 10, "fixture is 10-bit");
  CHECK(srcDistinct >= 600, "fixture has %d distinct codes", srcDistinct);
  CHECK(d10 >= int(0.9 * srcDistinct), "10-bit output keeps %d of %d distinct codes", d10, srcDistinct);
  CHECK(d10 > 256 + 100, "more codes than any 8-bit path can carry (%d)", d10);
  CHECK(d8.size() <= 220, "the 8-bit control path is limited to 8 bits (%zu)", d8.size());
  // the second fixture: a 4:2:2 ramp
  {
    DvVideo* v2 = dv_open((dir + "ramp_h264_422_10.mp4").c_str(), &oo, &info, err, sizeof err);
    CHECK(v2 != nullptr, "open ramp");
    if (v2) {
      int rw = 0, rh = 0;
      dv_begin(v2, 0, 0, 0, 0, DV_RGBAF16, 0, &rw, &rh);
      std::vector<std::uint16_t> rp(std::size_t(rw) * rh * 4);
      dv_next(v2, reinterpret_cast<std::uint8_t*>(rp.data()), std::int64_t(rp.size()) * 2);
      dv_close(v2);
      VideoFrameSession s2(cfg);
      InputFrame rf{rp.data(), rw, rh, static_cast<ColourKind>(info.colorKind), info.rotation};
      std::vector<std::uint8_t> o;
      CHECK(s2.render(rf, 1, PackFormat::P010, o, nullptr, &e), "ramp render");
      const int rd = distinctLuma(o, s2.outputWidth(), s2.outputHeight());
      std::printf("depth10: 4:2:2 ramp (%d distinct codes in the stream) -> %d distinct codes out\n", 876, rd);
      CHECK(rd >= 600, "ramp keeps %d codes", rd);
    }
  }
#else
  std::printf("depth10: SKIP (no desktop FFmpeg)\n");
  return 77;
#endif
  return 0;
}

// HDR policy (docs/engine/pipeline-architecture.md section 30.8): what transfer / primaries the clip is written in and that 10-bit survives.
static int testHdr(bool filmOk) {
#ifdef DAPPLE_HAVE_VIDEO_HOST
  const std::string dir = srcDir() + "/tests/data/video/";
  struct Case {
    const char* clip;
    const char* label;
    bool film;
    std::string crop;
    float exposure;
    bool allowHdr;
    OutputKind want;
    int prim, trc;
  };
  const Case cases[] = {
      {"hlg_ramp_hevc_420_10.mp4", "HLG, no film", false, "orig", 0.f, true, OutputKind::Hlg2020, 9, 18},
      {"pq_ramp_hevc_420_10.mp4", "PQ, no film", false, "orig", 0.f, true, OutputKind::Pq2020, 9, 16},
      {"hlg_ramp_hevc_420_10.mp4", "HLG, film", true, "orig", 0.f, true, OutputKind::Sdr709, 1, 1},
      {"hlg_ramp_hevc_420_10.mp4", "HLG, crop (geometry)", false, "1:1", 0.f, true, OutputKind::Hlg2020, 9, 18},
      {"hlg_ramp_hevc_420_10.mp4", "HLG, hdr off", false, "orig", 0.f, false, OutputKind::Sdr709, 1, 1},
      {"gradient_hevc_main10.mp4", "SDR 10-bit, no film", false, "orig", 0.f, true, OutputKind::Sdr709, 1, 1},
      {"slog3_h264_422_10.mp4", "S-Log3, no film", false, "orig", 0.f, true, OutputKind::Sdr709, 1, 1},
  };
  double meanLo = 0, meanHi = 0;
  for (const Case& c : cases) {
    if (c.film && !filmOk) continue;
    char err[256] = {0};
    DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
    DvInfo info{};
    DvVideo* v = dv_open((dir + c.clip).c_str(), &oo, &info, err, sizeof err);
    CHECK(v != nullptr, "open %s: %s", c.clip, err);
    if (!v) continue;
    int w = 0, h = 0;
    dv_begin(v, 0, info.durationUs, 0, 0, DV_RGBAF16, 0, &w, &h);
    std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
    CHECK(dv_next(v, reinterpret_cast<std::uint8_t*>(px.data()), std::int64_t(px.size()) * 2) >= 0, "next");
    dv_close(v);
    Config cfg = baseConfig();
    if (c.film) cfg.params.filmStock = "kodak_portra_400";
    cfg.params.crop = c.crop;
    cfg.params.exposure = c.exposure;
    cfg.allowHdr = c.allowHdr;
    VideoFrameSession s(cfg);
    InputFrame f{px.data(), w, h, static_cast<ColourKind>(info.colorKind), info.rotation};
    CHECK(s.setReference(f), "reference");
    std::vector<std::uint8_t> out;
    std::string e;
    CHECK(s.render(f, 1, PackFormat::P010Hdr, out, nullptr, &e), "%s: render %s", c.label, e.c_str());
    int tags[4];
    s.outputTags(tags);
    std::set<int> codes;
    int maxY = 0;
    const auto* y = reinterpret_cast<const std::uint16_t*>(out.data());
    const int W = s.outputWidth(), H = s.outputHeight();
    for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
      const int code = y[i] >> 6;
      codes.insert(code);
      maxY = std::max(maxY, code);
    }
    // source Y code at 94 % of the width (above SDR white for HLG / PQ) and the output at the same place (only meaningful without geometry)
    const int row = H / 2, col = int(0.94 * W);
    const int outY = y[std::size_t(row) * W + col] >> 6;
    std::printf("hdr %-24s -> %s | tags prim %d trc %d matrix %d | %zu distinct 10-bit luma codes, max %d, Y at 94%% width %d (%s)\n", c.label,
                s.outputKind() == OutputKind::Sdr709 ? "SDR BT.709" : s.outputKind() == OutputKind::Hlg2020 ? "HLG BT.2020" : "PQ BT.2020", tags[0], tags[1], tags[2], codes.size(),
                maxY, outY, s.outputNote());
    CHECK(s.outputKind() == c.want, "%s: output kind %d, wanted %d", c.label, int(s.outputKind()), int(c.want));
    CHECK(tags[0] == c.prim && tags[1] == c.trc, "%s: tags %d / %d, wanted %d / %d", c.label, tags[0], tags[1], c.prim, c.trc);
    CHECK(int(codes.size()) >= W / 2 || std::string(c.clip).find("slog3") != std::string::npos, "%s: only %zu distinct codes (width %d)", c.label, codes.size(), W);
    if (c.want == OutputKind::Hlg2020 && std::string(c.clip).find("hlg") == 0 && c.crop == "orig") {
      CHECK(maxY > 850, "HLG highlight headroom lost: max code %d", maxY);   // the ramp reaches 100 % signal = 3.8 x SDR white
      meanHi = outY;
      CHECK(std::abs(outY - (64 + 876 * col / (w - 1))) < 25, "HLG highlight round trip: source code %d, output %d", 64 + 876 * col / (w - 1), outY);
    }
    if (std::string(c.label) == "HLG, hdr off") meanLo = outY;
  }
  (void)meanLo;
  // exposure moves the HDR picture too
  {
    char err[256] = {0};
    DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
    DvInfo info{};
    DvVideo* v = dv_open((dir + "hlg_ramp_hevc_420_10.mp4").c_str(), &oo, &info, err, sizeof err);
    if (v) {
      int w = 0, h = 0;
      dv_begin(v, 0, info.durationUs, 0, 0, DV_RGBAF16, 0, &w, &h);
      std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
      dv_next(v, reinterpret_cast<std::uint8_t*>(px.data()), std::int64_t(px.size()) * 2);
      dv_close(v);
      auto meanY = [&](float ev) {
        Config cfg = baseConfig();
        cfg.params.exposure = ev;
        VideoFrameSession s(cfg);
        InputFrame f{px.data(), w, h, ColourKind::Hlg, 0};
        std::vector<std::uint8_t> out;
        s.render(f, 1, PackFormat::P010Hdr, out);
        double m = 0;
        const auto* y = reinterpret_cast<const std::uint16_t*>(out.data());
        for (std::size_t i = 0; i < std::size_t(s.outputWidth()) * s.outputHeight(); ++i) m += y[i] >> 6;
        return m / double(std::size_t(s.outputWidth()) * s.outputHeight());
      };
      const double m0 = meanY(0.f), m1 = meanY(1.f);
      std::printf("hdr exposure +1 EV on the HLG ramp: mean Y code %.1f -> %.1f\n", m0, m1);
      CHECK(m1 > m0 + 10, "exposure must act on the HDR output (%.1f -> %.1f)", m0, m1);
    }
  }
#else
  (void)filmOk;
  std::printf("hdr: SKIP (no desktop FFmpeg)\n");
  return 77;
#endif
  return 0;
}

static int testAnchor() {
  const CameraProfile c = VideoFrameSession::videoCamera();
  CHECK(c.baselineExposureEv == VideoFrameSession::kVideoBaselineEv && VideoFrameSession::kVideoBaselineEv == 0.f, "explicit video anchor 0 EV");
  CHECK(c.baselineContrast == 0.f && !c.colorMatch.enabled(), "no RAW colour baseline for video");
  // the session ignores the still's camera: a Config carrying a profile with a baseline gives the same frame
  const int W = 160, H = 90;
  auto A = makeFrame(W, H);
  InputFrame f{A.data(), W, H, ColourKind::Slog3, 0};
  Config c1, c2;
  c2.camera.baselineExposureEv = 1.5f;
  c2.camera.id = "sony.something";
  ImageF o1, o2;
  VideoFrameSession s1(c1), s2(c2);
  CHECK(s1.renderFloat(f, 1, o1) && s2.renderFloat(f, 1, o2), "render");
  CHECK(o1.data == o2.data, "a still's camera baseline must not move a video frame");
  std::printf("anchor: video baseline %.1f EV, explicit, independent of the still's camera\n", c.baselineExposureEv);
  return 0;
}

static int testFlicker(bool filmOk) {
  const int W = 320, H = 180;
  Config cfg = baseConfig();
  if (filmOk) {
    cfg.params.filmStock = "kodak_portra_400";
    cfg.params.filmGrain = 0.f;
  }
  cfg.params.contrast = 10.f;
  auto A = makeFrame(W, H, 0, 1.f);
  InputFrame f{A.data(), W, H, ColourKind::Standard, 0};
  VideoFrameSession s(cfg);
  CHECK(s.setReference(f), "reference");
  ImageF first;
  double lumMin = 1e9, lumMax = -1e9;
  float maxDiff = 0;
  for (int i = 1; i <= 10; ++i) {
    ImageF o;
    std::string e;
    CHECK(s.renderFloat(f, std::uint32_t(i), o, nullptr, &e), "frame %d: %s", i, e.c_str());
    const double m = meanOf(o);
    lumMin = std::min(lumMin, m), lumMax = std::max(lumMax, m);
    if (i == 1) {
      first = o;
    } else {
      for (std::size_t k = 0; k < o.data.size(); ++k) maxDiff = std::max(maxDiff, std::fabs(o.data[k] - first.data[k]));
    }
  }
  std::printf("flicker: 10 identical frames, film %s grain off: max frame-to-frame difference %.3g, mean luminance range %.3g\n", filmOk ? "on" : "off", maxDiff, lumMax - lumMin);
  CHECK(maxDiff == 0.f, "static clip, grain off: frames differ by %g", maxDiff);
  CHECK(lumMax - lumMin == 0.0, "mean luminance moves by %g", lumMax - lumMin);
  return 0;
}

static int testGrain(bool filmOk) {
  if (!filmOk) {
    std::printf("grain: SKIP (no film pack)\n");
    return 77;
  }
  const int W = 320, H = 180;
  auto A = makeFrame(W, H, 0, 1.f, 0.f);
  InputFrame f{A.data(), W, H, ColourKind::Standard, 0};
  Config on = baseConfig();
  on.params.filmStock = "kodak_portra_400";
  on.params.filmGrain = 100.f;
  Config off = on;
  off.params.filmGrain = 0.f;
  VideoFrameSession sOn(on), sOff(off);
  CHECK(sOn.setReference(f), "ref on");
  CHECK(sOff.setReference(f), "ref off");
  ImageF clean;
  CHECK(sOff.renderFloat(f, 1, clean), "clean");
  std::vector<ImageF> fr(6);
  std::string e;
  for (int i = 0; i < 6; ++i) CHECK(sOn.renderFloat(f, std::uint32_t(i + 1), fr[i], nullptr, &e), "grain frame %d %s", i, e.c_str());
  // Independent grain makes f1 - f2 and f3 - f4 uncorrelated (any static part of the film look cancels in a difference); identical grain would give 0 and a
  // repeated seed would give a perfect correlation.
  auto diff = [&](const ImageF& a, const ImageF& b) {
    std::vector<float> r(a.data.size());
    for (std::size_t k = 0; k < r.size(); ++k) r[k] = a.data[k] - b.data[k];
    return r;
  };
  const auto r1 = diff(fr[0], fr[1]), r2 = diff(fr[2], fr[3]);
  double s11 = 0, s22 = 0, s12 = 0;
  for (std::size_t k = 0; k < r1.size(); ++k) s11 += double(r1[k]) * r1[k], s22 += double(r2[k]) * r2[k], s12 += double(r1[k]) * r2[k];
  const double rms1 = std::sqrt(s11 / double(r1.size())) / std::sqrt(2.0), corr = s12 / std::sqrt(s11 * s22 + 1e-30);
  double sc = 0;
  for (std::size_t k = 0; k < clean.data.size(); ++k) sc += double(fr[0].data[k] - clean.data[k]) * (fr[0].data[k] - clean.data[k]);
  const double grainVsClean = std::sqrt(sc / double(clean.data.size()));
  // mean of grain over frames stays put
  double meanClean = meanOf(clean), meanGrain = 0;
  for (auto& x : fr) meanGrain += meanOf(x);
  meanGrain /= double(fr.size());
  std::printf("grain: animated grain rms %.4f display units per frame (a frame vs the grain-off render also differs by the static dye-cloud blur: %.4f), correlation of (f1-f2) with (f3-f4): %.4f, mean %.5f with grain vs %.5f clean (diff %.5f)\n", rms1, grainVsClean, corr, meanGrain, meanClean,
              meanGrain - meanClean);
  CHECK(rms1 > 1e-3, "grain should be visible (rms %g)", rms1);
  CHECK(std::fabs(corr) < 0.1, "grain of consecutive frames must be decorrelated (%.3f)", corr);
  CHECK(std::fabs(meanGrain - meanClean) < 0.004, "mean level moves by %.5f", meanGrain - meanClean);
  // deterministic per frame index
  ImageF again;
  sOn.renderFloat(f, 3, again);
  CHECK(again.data == fr[2].data, "the same frame index must give a bit-identical frame");
  return 0;
}

static int testPerf(bool filmOk) {
  const int W = 1920, H = 1080;
  auto A = makeFrame(W, H, 0, 1.f);
  auto B = makeFrame(W, H, 3, 0.95f, 0.01f, 2);
  InputFrame fa{A.data(), W, H, ColourKind::Standard, 0}, fb{B.data(), W, H, ColourKind::Standard, 0};
  struct Row {
    const char* name;
    EditParams p;
  };
  std::vector<Row> rows;
  EditParams d;
  d.exposure = 0.3f;
  d.contrast = 15.f;
  d.vibrance = 15.f;
  rows.push_back({"tone+colour", d});
  EditParams sp = d;
  sp.clarity = 20.f;
  sp.sharpen = 30.f;
  sp.nrLuma = 20.f;
  rows.push_back({"tone+colour+spatial", sp});
  if (filmOk) {
    EditParams f;
    f.filmStock = "kodak_portra_400";
    rows.push_back({"film portra", f});
  }
  for (const Row& r : rows) {
    Config cfg = baseConfig();
    cfg.params = r.p;
    VideoFrameSession s(cfg);
    const auto t0 = Clock::now();
    CHECK(s.setReference(fa), "reference");
    const double refMs = msSince(t0);
    std::vector<std::uint8_t> o;
    FrameInfo fi;
    std::string e;
    double conv = 0, ren = 0, pack = 0, tot = 0;
    const int N = 5;
    for (int i = 0; i < N; ++i) {
      const auto t = Clock::now();
      CHECK(s.render(i & 1 ? fb : fa, std::uint32_t(i + 1), PackFormat::P010, o, &fi, &e), "render %s", e.c_str());
      tot += msSince(t);
      conv += fi.convertMs, ren += fi.renderMs, pack += fi.packMs;
    }
    std::printf("perf 1080p %-22s CPU: %.0f ms/frame (convert %.0f, graph %.0f, P010 pack %.0f); reference frame %.0f ms once; tablesHit=%d executor=%s\n", r.name, tot / N, conv / N,
                ren / N, pack / N, refMs, fi.tablesHit ? 1 : 0, fi.executor.c_str());
  }
  return 0;
}

static int testE2E(bool filmOk) {
#ifdef DAPPLE_HAVE_VIDEO_HOST
  const std::string dir = srcDir() + "/tests/data/video/";
  const char* clips[] = {"slog3_h264_422_10.mp4", "hlg_hevc_420_10.mp4", "sdr_hevc_422_10.mp4", "sdr_h264_420_8.mp4"};
  for (const char* c : clips) {
    char err[256] = {0};
    DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
    DvInfo info{};
    DvVideo* v = dv_open((dir + c).c_str(), &oo, &info, err, sizeof err);
    CHECK(v != nullptr, "open %s: %s", c, err);
    if (!v) continue;
    int w = 0, h = 0;
    dv_begin(v, 0, info.durationUs, 0, 0, DV_RGBAF16, 0, &w, &h);
    std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
    Config cfg = baseConfig();
    if (filmOk) cfg.params.filmStock = "kodak_portra_400";
    VideoFrameSession s(cfg);
    int n = 0;
    double lo = 1e9, hi = -1e9;
    bool shown = false;
    std::vector<std::uint8_t> o;
    std::string e;
    while (dv_next(v, reinterpret_cast<std::uint8_t*>(px.data()), std::int64_t(px.size()) * 2) >= 0) {
      InputFrame f{px.data(), w, h, static_cast<ColourKind>(info.colorKind), info.rotation};
      if (!shown) {
        shown = true;
        const std::uint16_t* c = px.data() + (std::size_t(h / 2) * w + w / 2) * 4;
        std::printf("e2e   first frame centre (linear F16): %.4f %.4f %.4f\n", halfToFloat(c[0]), halfToFloat(c[1]), halfToFloat(c[2]));
      }
      ImageF img;
      if (!s.renderFloat(f, std::uint32_t(n + 1), img, nullptr, &e)) {
        CHECK(false, "%s frame %d: %s", c, n, e.c_str());
        break;
      }
      const double m = meanOf(img);
      lo = std::min(lo, m), hi = std::max(hi, m);
      CHECK(std::isfinite(m) && m > 0.02 && m < 0.98, "%s: frame %d mean %f out of range", c, n, m);
      ++n;
    }
    dv_close(v);
    std::printf("e2e %-26s %dx%d %d-bit kind %d: %d frames through the graph%s, mean level %.3f..%.3f\n", c, w, h, info.bitDepth, info.colorKind, n, filmOk ? " with film" : "", lo, hi);
    CHECK(n >= 10, "%s: only %d frames", c, n);
  }
#else
  (void)filmOk;
  std::printf("e2e: SKIP (no desktop FFmpeg)\n");
  return 77;
#endif
  return 0;
}

// HDR clip + geometry (crop / rotate / straighten / flip) stays HDR: the 10-bit packed output is HLG / PQ BT.2020, and a highlight patch above SDR white sits
// where the graph's own (SDR) render puts the patch, i.e. the float HDR source is warped the way the graph warps the frame.
static int testHdrGeometry() {
  struct Case {
    const char* label;
    const char* crop;
    int rotate;
    float straighten;
    bool flip;
  };
  const Case cases[] = {
      {"rotate 90", "orig", 90, 0.f, false},
      {"crop 1:1", "1:1", 0, 0.f, false},
      {"straighten 4", "orig", 0, 4.f, false},
      {"crop 16:9 + rotate 270 + straighten -3 + flip", "16:9", 270, -3.f, true},
  };
  const int w = 320, h = 180;
  std::vector<std::uint16_t> px(std::size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const bool patch = x >= 200 && x < 240 && y >= 60 && y < 100;
      const float v = patch ? 4.0f : 0.18f + 0.1f * float(x) / w;  // 4.0 = 4x SDR white (HLG ~ code 920)
      std::uint16_t* p = &px[(std::size_t(y) * w + x) * 4];
      p[0] = p[1] = p[2] = toHalf(v);
      p[3] = 0x3c00;
    }
  for (ColourKind kind : {ColourKind::Hlg, ColourKind::Pq})
    for (const Case& c : cases) {
      double cen[2][2] = {{0, 0}, {0, 0}};
      long cnt[2] = {0, 0};
      int maxCode[2] = {0, 0}, ow[2] = {0, 0}, oh[2] = {0, 0};
      OutputKind got = OutputKind::Sdr709;
      int tags[4] = {0, 0, 0, 0};
      for (int hdr = 1; hdr >= 0; --hdr) {  // 1 = HDR output, 0 = the graph's SDR render of the same edit (the geometry reference)
        Config cfg = baseConfig();
        cfg.params.crop = c.crop;
        cfg.params.rotate = c.rotate;
        cfg.params.straighten = c.straighten;
        cfg.params.flip = c.flip;
        cfg.allowHdr = hdr != 0;
        VideoFrameSession s(cfg);
        InputFrame f{px.data(), w, h, kind, 0};
        std::string e;
        CHECK(s.setReference(f, &e), "%s: reference %s", c.label, e.c_str());
        std::vector<std::uint8_t> out;
        CHECK(s.render(f, 1, PackFormat::P010Hdr, out, nullptr, &e), "%s: render %s", c.label, e.c_str());
        const int W = s.outputWidth(), H = s.outputHeight();
        ow[hdr] = W, oh[hdr] = H;
        if (hdr) got = s.outputKind(), s.outputTags(tags);
        const auto* y = reinterpret_cast<const std::uint16_t*>(out.data());
        // patch pixels = the bright ones (above 76 % of the code range: SDR content is lower, the SDR render of the patch is white)
        for (int j = 0; j < H; ++j)
          for (int i = 0; i < W; ++i) {
            const int code = y[std::size_t(j) * W + i] >> 6;
            maxCode[hdr] = std::max(maxCode[hdr], code);
            if (code > 640) cen[hdr][0] += i, cen[hdr][1] += j, ++cnt[hdr];
          }
        if (cnt[hdr] > 0) cen[hdr][0] /= double(cnt[hdr]), cen[hdr][1] /= double(cnt[hdr]);
      }
      const OutputKind want = kind == ColourKind::Hlg ? OutputKind::Hlg2020 : OutputKind::Pq2020;
      std::printf("hdr_geometry %s %-46s: out %dx%d (SDR ref %dx%d), tags %d/%d/%d, max code %d, patch %ld px at (%.1f, %.1f) vs SDR ref %ld px at (%.1f, %.1f)\n",
                  kind == ColourKind::Hlg ? "HLG" : "PQ ", c.label, ow[1], oh[1], ow[0], oh[0], tags[0], tags[1], tags[2], maxCode[1], cnt[1], cen[1][0], cen[1][1],
                  cnt[0], cen[0][0], cen[0][1]);
      CHECK(got == want, "%s: output kind %d", c.label, int(got));
      CHECK(tags[0] == 9 && tags[1] == (kind == ColourKind::Hlg ? 18 : 16) && tags[2] == 9, "%s: tags %d/%d/%d", c.label, tags[0], tags[1], tags[2]);
      CHECK(ow[0] == ow[1] && oh[0] == oh[1], "%s: HDR frame size differs from the graph's", c.label);
      CHECK(maxCode[1] > (kind == ColourKind::Hlg ? 800 : 650), "%s: highlight above SDR white lost (max code %d)", c.label, maxCode[1]);
      CHECK(cnt[1] > 0 && cnt[0] > 0, "%s: no patch found", c.label);
      if (cnt[1] > 0 && cnt[0] > 0) {
        CHECK(std::fabs(cen[1][0] - cen[0][0]) < 1.5 && std::fabs(cen[1][1] - cen[0][1]) < 1.5, "%s: patch position differs from the graph's (%.1f,%.1f) vs (%.1f,%.1f)",
              c.label, cen[1][0], cen[1][1], cen[0][0], cen[0][1]);
        CHECK(std::fabs(double(cnt[1]) - double(cnt[0])) < 0.15 * double(cnt[0]), "%s: patch size %ld vs %ld", c.label, cnt[1], cnt[0]);
      }
    }
  return 0;
}

static int testVerify(const char* path, int wantTrc, int wantPrim) {
#ifdef DAPPLE_HAVE_VIDEO_HOST
  char err[256] = {0};
  DvOpenOptions oo{0, DV_SRC_AUTO, nullptr};
  DvInfo info{};
  DvVideo* v = dv_open(path, &oo, &info, err, sizeof err);
  CHECK(v != nullptr, "open %s: %s", path, err);
  if (!v) return 1;
  int w = 0, h = 0;
  dv_begin(v, 0, info.durationUs, 0, 0, DV_P010, 0, &w, &h);
  std::vector<std::uint8_t> p(std::size_t(dv_frame_bytes(v)));
  int n = 0;
  std::set<int> codes;
  while (dv_next(v, p.data(), std::int64_t(p.size())) >= 0) {
    const auto* y = reinterpret_cast<const std::uint16_t*>(p.data());
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) codes.insert(y[i] >> 6);
    ++n;
  }
  dv_close(v);
  std::printf("verify %s: %dx%d %d-bit (%s) %d frames, %zu distinct 10-bit luma codes\n", path, w, h, info.bitDepth, info.codec, n, codes.size());
  CHECK(n > 0, "no frames");
  CHECK(info.bitDepth == 10, "output is %d-bit", info.bitDepth);
  CHECK(codes.size() > 300, "only %zu distinct codes", codes.size());
  if (wantTrc > 0) {
    std::printf("verify %s: stream tags primaries %d transfer %d matrix %d\n", path, info.primaries, info.trc, info.matrix);
    CHECK(info.trc == wantTrc && info.primaries == wantPrim, "stream tags prim %d trc %d, wanted %d / %d", info.primaries, info.trc, wantPrim, wantTrc);
  }
  return 0;
#else
  (void)path;
  return 77;
#endif
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: video_session_tests convert|oracle|hold|depth10|flicker|grain|perf|e2e|verify <file>\n");
    return 2;
  }
  const std::string id = argv[1];
  const bool filmOk = loadFilm(srcDir());
  if (!filmOk) std::printf("note: no film pack, film cases skipped\n");
  int rc = 0;
  if (id == "convert") rc = testConvert();
  else if (id == "oracle") rc = testOracle(filmOk);
  else if (id == "hold") rc = testHold(filmOk);
  else if (id == "depth10") rc = testDepth10();
  else if (id == "flicker") rc = testFlicker(filmOk);
  else if (id == "hdr") rc = testHdr(filmOk);
  else if (id == "hdr_geometry") rc = testHdrGeometry();
  else if (id == "anchor") rc = testAnchor();
  else if (id == "grain") rc = testGrain(filmOk);
  else if (id == "perf") rc = testPerf(filmOk);
  else if (id == "e2e") rc = testE2E(filmOk);
  else if (id == "verify" && argc > 2) rc = testVerify(argv[2], argc > 3 ? std::atoi(argv[3]) : 0, argc > 4 ? std::atoi(argv[4]) : 0);
  else {
    std::printf("unknown test %s\n", id.c_str());
    return 2;
  }
  if (rc == 77) return 77;
  if (g_fail) {
    std::printf("%s: %d check(s) FAILED\n", id.c_str(), g_fail);
    return 1;
  }
  std::printf("%s: PASS\n", id.c_str());
  return 0;
}
