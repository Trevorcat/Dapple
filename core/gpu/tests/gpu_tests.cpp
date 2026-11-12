// GPU backend tests (ctest "gpu_parity", "gpu_parity_swiftshader"): per-stage parity with the CPU reference, tiled
// export seams, streamed source, downscale, region, steady-state allocation, blocklist. Prints a table (mean / p99 / max
// Delta E 2000); the pass criterion of a stage is mean <= 0.5.
//
// Env: DAPPLE_GPU_SOFTWARE=1 accepts a CPU Vulkan device (SwiftShader); DAPPLE_GPU_DEVICE=<index>.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/encode.h"
#include "dapple/film.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"

using namespace dapple;
using namespace dapple::gpu;
namespace eg = dapple::engine::gpu;

namespace {

int g_fail = 0;
#define CHECK(cond, ...)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                               \
      std::printf("\n");                                       \
      ++g_fail;                                               \
    }                                                         \
  } while (0)

struct Env {
  std::shared_ptr<eg::Context> be;
  CameraRegistry cams = CameraRegistry::withBuiltins();
};

// CPU Lanczos-3 resize, double precision, same footprint / normalisation as the GPU kernel (the reference of the
// downscale test).
ImageF cpuLanczos(const ImageF& in, int ow, int oh) {
  auto lanczos = [](double x) {
    x = std::fabs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = 3.14159265358979 * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
  };
  const double sx = double(in.width) / ow, sy = double(in.height) / oh;
  const double Sx = std::max(1.0, sx), Sy = std::max(1.0, sy);
  ImageF tmp(ow, in.height), out(ow, oh);
  for (int y = 0; y < in.height; ++y)
    for (int x = 0; x < ow; ++x) {
      const double c = (x + 0.5) * sx - 0.5;
      double acc[3] = {0, 0, 0}, ws = 0;
      for (int i = int(std::floor(c - 3 * Sx)) + 1; i <= int(std::floor(c + 3 * Sx)); ++i) {
        const double w = lanczos((i - c) / Sx);
        const float* p = in.row(y) + 3 * std::clamp(i, 0, in.width - 1);
        for (int k = 0; k < 3; ++k) acc[k] += w * p[k];
        ws += w;
      }
      for (int k = 0; k < 3; ++k) tmp.row(y)[3 * x + k] = float(acc[k] / ws);
    }
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) {
      const double c = (y + 0.5) * sy - 0.5;
      double acc[3] = {0, 0, 0}, ws = 0;
      for (int i = int(std::floor(c - 3 * Sy)) + 1; i <= int(std::floor(c + 3 * Sy)); ++i) {
        const double w = lanczos((i - c) / Sy);
        const float* p = tmp.row(std::clamp(i, 0, in.height - 1)) + 3 * x;
        for (int k = 0; k < 3; ++k) acc[k] += w * p[k];
        ws += w;
      }
      for (int k = 0; k < 3; ++k) out.row(y)[3 * x + k] = float(acc[k] / ws);
    }
  return out;
}

struct Stage {
  const char* name;
  std::function<void(EditParams&)> set;
  std::function<void(FastOptions&)> opt = nullptr;
  bool lut = false;  // the renderer carries the synthetic look LUT (testLut)
};

const Lut3D& testLut() { return syntheticLut(); }  // the look LUT of the film display-stage rows (gpu_rig.h)

EditParams baseParams() {
  EditParams p;
  p.exposure = 0.3f;
  p.contrast = 15;
  p.highlights = -25;
  p.shadows = 20;
  p.vibrance = 15;
  p.saturation = 10;
  return p;
}

const std::vector<Stage>& stages() {
  static const std::vector<Stage> s = {
      {"point (tone+lattice)", [](EditParams&) {}},
      {"geometry rot90", [](EditParams& p) { p.rotate = 90; }},
      {"geometry rot270+flip", [](EditParams& p) { p.rotate = 270; p.flip = true; }},
      {"geometry crop 16:9", [](EditParams& p) { p.crop = "16:9"; }},
      {"geometry straighten 4.5 + crop 4:5", [](EditParams& p) { p.straighten = 4.5f; p.crop = "4:5"; }},
      {"geometry rot90+straighten -7", [](EditParams& p) { p.rotate = 90; p.straighten = -7.f; }},
      {"vignette -40", [](EditParams& p) { p.vignette = -40; }},
      {"vignette +30", [](EditParams& p) { p.vignette = 30; }},
      {"noise reduction luma 50", [](EditParams& p) { p.nrLuma = 50; }},
      {"noise reduction chroma 60", [](EditParams& p) { p.nrColor = 60; }},
      {"dehaze +40", [](EditParams& p) { p.dehaze = 40; }},
      {"dehaze -40", [](EditParams& p) { p.dehaze = -40; }},
      {"clarity +40", [](EditParams& p) { p.clarity = 40; }},
      {"clarity -30", [](EditParams& p) { p.clarity = -30; }},
      {"texture +50", [](EditParams& p) { p.texture = 50; }},
      {"texture -40", [](EditParams& p) { p.texture = -40; }},
      {"sharpen 70 r1.0", [](EditParams& p) { p.sharpen = 70; p.sharpRadius = 10.f; }},
      {"sharpen 70 r2.5", [](EditParams& p) { p.sharpen = 70; p.sharpRadius = 25.f; }},
      {"grain 40", [](EditParams& p) { p.grain = 40; }},
      {"all spatial + geometry", [](EditParams& p) {
         p.nrLuma = 35; p.nrColor = 40; p.dehaze = 25; p.clarity = 30; p.texture = 25; p.sharpen = 60; p.grain = 25;
         p.straighten = 2.f; p.vignette = -20; }},
      {"interactive quality (no grain/NR)", [](EditParams& p) { p.nrLuma = 35; p.clarity = 30; p.sharpen = 60; p.grain = 25; },
       [](FastOptions& o) { o.spatial = SpatialQuality::Interactive; o.noise = false; }},
      {"zoom region 1:1", [](EditParams& p) { p.clarity = 20; p.sharpen = 40; },
       [](FastOptions& o) { o.region = RenderRegion{0.3f, 0.35f, 0.6f, 0.65f, 400}; }},
      // film frames on the GPU (pipeline-architecture.md section 20): the film chain, then vignette + window nodes
      {"film portra400, all effects", [](EditParams& p) { p.filmStock = "kodak_portra_400"; }},
      {"film velvia (slide), no grain", [](EditParams& p) { p.filmStock = "fujifilm_velvia_100"; p.filmGrain = 0; }},
      {"film + geometry + vignette", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.straighten = 3.f; p.crop = "4:5"; p.vignette = -30; }},
      {"film + NR, clarity, sharpen", [](EditParams& p) { p.filmStock = "kodak_ektar_100"; p.nrLuma = 30; p.clarity = 30; p.sharpen = 50; }},
      {"film interactive", [](EditParams& p) { p.filmStock = "kodak_portra_400"; },
       [](FastOptions& o) { o.spatial = SpatialQuality::Interactive; o.noise = false; }},
      {"film interactive, grain drag", [](EditParams& p) { p.filmStock = "kodak_portra_400"; },
       [](FastOptions& o) { o.spatial = SpatialQuality::Interactive; o.noise = false; o.keepGrain = true; o.fullLongEdge = 3000; }},
      // film coverage (no CPU fallback): strength < 100 blends the film with the normal render, display curves + look LUT follow the film
      {"film strength 60 (blend)", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.filmStrength = 60; }},
      {"film strength 0 (normal render only)", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.filmStrength = 0; }},
      {"film strength 30 + geometry + vignette + NR/sharpen", [](EditParams& p) {
         p.filmStock = "kodak_ektar_100"; p.filmStrength = 30; p.straighten = 2.f; p.crop = "4:5"; p.vignette = 25; p.nrLuma = 25; p.sharpen = 40; }},
      {"film + point curves (master, red)", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.toneCurve = {{0.f, 0.f}, {76.f, 56.f}, {178.f, 209.f}, {255.f, 255.f}}; p.toneCurveRed = {{0.f, 13.f}, {255.f, 242.f}}; }},
      {"film + parametric curves (master, blue)", [](EditParams& p) {
         p.filmStock = "fujifilm_velvia_100"; p.parametricShadows = 30; p.parametricHighlights = -25; p.channelParametric[2] = {0.f, 20.f, -10.f, 0.f}; }},
      {"film + look LUT 70%", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.lutAmount = 70; }, nullptr, true},
      {"film strength 70 + curves + LUT + grain", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.filmStrength = 70; p.toneCurve = {{0.f, 0.f}, {128.f, 107.f}, {255.f, 255.f}}; p.lutAmount = 55; p.filmGrain = 80; },
       nullptr, true},
      {"film strength 40 interactive + LUT", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.filmStrength = 40; p.lutAmount = 100; },
       [](FastOptions& o) { o.spatial = SpatialQuality::Interactive; o.noise = false; }, true},
  };
  return s;
}

struct Row {
  std::string name;
  DeStats s;
};

// one parity run: GPU vs CPU reference on `src`
bool runStage(Env& env, Rig& rig, const SourceImage& src, const Stage& st, Row& row, const char* tag, bool quiet = false) {
  EditParams p = baseParams();
  st.set(p);
  const CameraProfile& cam = env.cams.resolve("", "");
  Renderer r(p, cam, st.lut ? &testLut() : nullptr);
  FastOptions fo;
  fo.spatial = SpatialQuality::Full;
  if (st.opt) st.opt(fo);
  const auto lut = r.bake(src, 41);
  int w = 0, h = 0, gw = 0, gh = 0;
  SourceImage halfSrc;
  const SourceImage* refSrc = &src;
  if (std::getenv("DAPPLE_GPU_HALFREF")) {
    halfSrc = src;
    for (float& f : halfSrc.linear.data) {
      int e;
      const float m = std::frexp(f, &e);
      f = std::ldexp(std::nearbyint(m * 2048.f) / 2048.f, e);
    }
    refSrc = &halfSrc;
  }
  std::uint8_t* cpu = r.renderBakedRgba(*refSrc, *lut, fo, w, h);
  std::string err;
  std::uint8_t* gpu = rig.render(p, r, src, fo, 41, gw, gh, nullptr, err, 0, 0.f, st.lut ? &testLut() : nullptr);
  if (!cpu || !gpu || w != gw || h != gh) {
    std::printf("  %-38s %s: render failed: %s (cpu %dx%d gpu %dx%d)\n", st.name, tag, err.c_str(), w, h, gw, gh);
    std::free(cpu);
    std::free(gpu);
    return false;
  }
  row.name = st.name;
  row.s = compareRgba8(gpu, cpu, std::size_t(w) * h);
  if (std::getenv("DAPPLE_GPU_DIFFDUMP") && st.name[0] == 112) {  // temp
    int shown = 0;
    for (std::size_t i = 0; i < std::size_t(w) * h && shown < 12; i += 37)
      if (std::abs(int(gpu[4 * i]) - int(cpu[4 * i])) + std::abs(int(gpu[4 * i + 1]) - int(cpu[4 * i + 1])) > 0) {
        const float* s = src.linear.row(i / w) + 3 * (i % w);
        std::printf("  px %zu,%zu src %.5f %.5f %.5f  gpu %d %d %d  cpu %d %d %d\n", i % w, i / w, s[0], s[1], s[2], gpu[4 * i], gpu[4 * i + 1], gpu[4 * i + 2], cpu[4 * i], cpu[4 * i + 1], cpu[4 * i + 2]);
        ++shown;
      }
  }
  if (row.s.max > 30.0) {  // where is the worst pixel? (helps to spot a device specific numerical problem)
    const std::size_t i = row.s.maxAt;
    std::printf("      worst pixel (%zu, %zu): gpu %d %d %d  cpu %d %d %d\n", i % std::size_t(w), i / std::size_t(w), gpu[4 * i], gpu[4 * i + 1], gpu[4 * i + 2], cpu[4 * i], cpu[4 * i + 1], cpu[4 * i + 2]);
  }
  std::free(cpu);
  std::free(gpu);
  if (!quiet)
    std::printf("  %-38s %-9s %4dx%-4d  mean %.4f  p99 %.3f  max %.2f   (differing %.1f%%)\n", st.name, tag, w, h, row.s.mean, row.s.p99, row.s.max, row.s.differing * 100);
  return true;
}

void testStages(Env& env) {
  std::printf("== per-stage parity vs the CPU reference (dE2000) ==\n");
  struct Sz {
    int w, h;
  };
  for (Sz z : {Sz{640, 448}, Sz{1536, 1024}}) {
    const SourceImage src = makeTestImage(z.w, z.h);
    Rig rig(env.be, src, env.cams.resolve("", ""));
    char tag[32];
    std::snprintf(tag, sizeof tag, "%dx%d", z.w, z.h);
    for (const Stage& st : stages()) {
      Row row;
      if (!runStage(env, rig, src, st, row, tag)) {
        ++g_fail;
        continue;
      }
      CHECK(row.s.mean <= 0.5, "stage '%s' mean dE %.3f", st.name, row.s.mean);
      if (std::string(st.name).rfind("film", 0) == 0) CHECK(row.s.mean <= 0.1 && row.s.p99 <= 1.0, "film stage '%s': mean %.3f p99 %.3f", st.name, row.s.mean, row.s.p99);
      CHECK(row.s.max <= 30.0, "stage '%s' max dE %.2f", st.name, row.s.max);
    }
  }
}

// Error budget: how much of the GPU-vs-CPU difference is the RGBA16F storage of the source (11 bit mantissa)?
void testHalfBudget(Env& env) {
  std::printf("== error budget: CPU on the fp16-rounded source vs CPU on the float source ==\n");
  const SourceImage src = makeTestImage(640, 448);
  SourceImage half = src;
  for (float& f : half.linear.data) {
    int e;
    const float m = std::frexp(f, &e);
    f = std::ldexp(std::nearbyint(m * 2048.f) / 2048.f, e);
  }
  EditParams p = baseParams();
  Renderer r(p, env.cams.resolve("", ""), nullptr);
  const auto lut = r.bake(src, 41);
  FastOptions fo;
  fo.spatial = SpatialQuality::Off;
  int w, h;
  std::uint8_t* a = r.renderBakedRgba(src, *lut, fo, w, h);
  std::uint8_t* b = r.renderBakedRgba(half, *lut, fo, w, h);
  const DeStats s = compareRgba8(a, b, std::size_t(w) * h);
  std::printf("  mean %.4f  p99 %.3f  max %.2f  (differing %.1f%%)\n", s.mean, s.p99, s.max, s.differing * 100);
  std::free(a);
  std::free(b);
}

void testDownscale(Env& env) {
  std::printf("== Lanczos downscale (GPU resize vs CPU double precision Lanczos reference) ==\n");
  const SourceImage src = makeTestImage(1800, 1200);
  EditParams p = baseParams();
  p.sharpen = 30;
  p.straighten = 2.f;
  const CameraProfile& cam = env.cams.resolve("", "");
  Renderer r(p, cam, nullptr);
  for (int longEdge : {1200, 640, 300}) {
    SourceImage small;
    small.sceneReferred = true;
    int rw, rh;
    {
      const double s = double(longEdge) / 1800;
      rw = int(std::lround(1800 * s));
      rh = int(std::lround(1200 * s));
    }
    small.linear = cpuLanczos(src.linear, rw, rh);
    const auto lut = r.bake(small, 41);
    FastOptions fo;
    int w = 0, h = 0, gw = 0, gh = 0;
    std::uint8_t* cpu = r.renderBakedRgba(small, *lut, fo, w, h);
    Rig rig(env.be, src, cam);
    std::string err;
    std::uint8_t* gpu = rig.render(p, r, small, fo, 41, gw, gh, nullptr, err);  // tables and frame size from the CPU Lanczos copy; the GPU resizes the full source itself
    CHECK(cpu && gpu && w == gw && h == gh, "size %dx%d vs %dx%d: %s", w, h, gw, gh, err.c_str());
    if (cpu && gpu && w == gw && h == gh) {
      const DeStats s = compareRgba8(gpu, cpu, std::size_t(w) * h);
      std::printf("  long edge %4d (%dx%d)  mean %.4f  p99 %.3f  max %.2f\n", longEdge, w, h, s.mean, s.p99, s.max);
      CHECK(s.mean <= 0.5, "downscale mean %.3f", s.mean);
    }
    std::free(cpu);
    std::free(gpu);
  }
}

// Tiled export against the single-frame render of the same pipeline: with a sufficient halo and global analysis the
// result must not depend on the tile size (it is the same arithmetic per pixel).
void testTiling(Env& env, bool stream, int pw = 1500, int ph = 1000, std::vector<int> tiles = {128, 256, 400, 1024}) {
  std::printf("== tiled export seams%s, %dx%d picture: tile sizes vs the single frame ==\n", stream ? " (source streamed per tile)" : "", pw, ph);
  const SourceImage src = makeTestImage(pw, ph);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    int longEdge;
  };
  const std::vector<Case> cases = {
      {"all spatial", [](EditParams& p) { p.nrLuma = 35; p.nrColor = 40; p.dehaze = 25; p.clarity = 30; p.texture = 25; p.sharpen = 60; p.grain = 25; p.vignette = -20; }, 0},
      {"all spatial + straighten + crop", [](EditParams& p) { p.nrLuma = 30; p.nrColor = 50; p.dehaze = 30; p.clarity = 30; p.texture = 30; p.sharpen = 50; p.straighten = 3.f; p.crop = "4:5"; p.grain = 20; }, 0},
      {"rot90 + dehaze + downscale 900", [](EditParams& p) { p.rotate = 90; p.dehaze = 35; p.clarity = 25; p.sharpen = 40; p.nrLuma = 20; }, 900},
  };
  const CameraProfile& cam = env.cams.resolve("", "");
  for (const Case& c : cases) {
    EditParams p = baseParams();
    c.set(p);
    Renderer r(p, cam, nullptr);
    FastOptions fo;
    Rig rig(env.be, src, cam);
    std::string err;
    const float rsharp = c.longEdge ? 0.2f : 0.f;
    int w = 0, h = 0;
    std::uint8_t* whole = rig.render(p, r, src, fo, 41, w, h, nullptr, err, c.longEdge, rsharp);
    CHECK(whole != nullptr, "%s", err.c_str());
    if (!whole) continue;
    for (int tile : tiles) {
      std::vector<std::uint8_t> img(std::size_t(w) * h * 4, 0);
      std::size_t covered = 0;
      auto sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
        for (int j = 0; j < th; ++j) std::memcpy(img.data() + (std::size_t(y + j) * w + x) * 4, rgba + std::size_t(j) * stride, std::size_t(tw) * 4);
        covered += std::size_t(tw) * th;
        return true;
      };
      engine::GpuStats st;
      const bool ok = rig.exportTiled(p, r, src, c.longEdge, 41, tile, stream, rsharp, nullptr, sink, &st, err);
      CHECK(ok, "export: %s", err.c_str());
      if (!ok) continue;
      CHECK(covered == std::size_t(w) * h, "covered %zu of %zu", covered, std::size_t(w) * h);
      int maxd = 0;
      std::size_t nd = 0;
      for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(img[4 * i + k]) - int(whole[4 * i + k])));
        maxd = std::max(maxd, d);
        nd += d > 0;
      }
      std::printf("  %-34s tile %4d: %4d tiles-px  max channel diff %d/255, %zu differing px  (%d passes, %.0f ms)\n", c.name, tile, int(covered / 1000), maxd, nd, st.passes, st.wallMs);
      CHECK(maxd <= 1, "seam: max channel difference %d (tile %d, %s)", maxd, tile, c.name);
    }
    std::free(whole);
  }
}

// Tiled export of film frames against the whole-frame GPU render of the same job: exposure, mixtures and the DIR field run once over the
// frame, the develop / grain / print / scan chain per tile with the film halo, grain hashed on absolute coordinates, display stages
// and the blend with the normal branch per pixel. The tiles must reproduce the whole frame to the last bit of the 8-bit output.
void testFilmTiling(Env& env, int pw = 1500, int ph = 1000, std::vector<int> tiles = {128, 256, 400, 1024}) {
  std::printf("== tiled film export seams, %dx%d picture: 0 differing pixels vs the whole frame ==\n", pw, ph);
  const SourceImage src = makeTestImage(pw, ph);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    int longEdge;
    bool lut;
  };
  const std::vector<Case> cases = {
      {"film portra, all effects", [](EditParams& p) { p.filmStock = "kodak_portra_400"; }, 0, false},
      {"film ektar + NR, sharpen, grain, vignette", [](EditParams& p) {
         p.filmStock = "kodak_ektar_100"; p.nrLuma = 30; p.nrColor = 30; p.sharpen = 50; p.grain = 20; p.vignette = -25; }, 0, false},
      {"film strength 55 + curves + LUT + straighten/crop", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.filmStrength = 55; p.toneCurve = {{0.f, 0.f}, {96.f, 80.f}, {255.f, 255.f}}; p.toneCurveBlue = {{0.f, 10.f}, {255.f, 245.f}};
         p.lutAmount = 60; p.straighten = 3.f; p.crop = "4:5"; p.sharpen = 30; }, 0, true},
      {"film velvia + dehaze, clarity (global solves)", [](EditParams& p) {
         p.filmStock = "fujifilm_velvia_100"; p.dehaze = 30; p.clarity = 30; p.sharpen = 40; }, 0, false},
      {"film strength 80 + rot90 + downscale 900", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.filmStrength = 80; p.rotate = 90; p.dehaze = 25; p.sharpen = 40; }, 900, false},
  };
  const CameraProfile& cam = env.cams.resolve("", "");
  for (const Case& c : cases) {
    EditParams p = baseParams();
    c.set(p);
    const Lut3D* lut = c.lut ? &testLut() : nullptr;
    Renderer r(p, cam, lut);
    FastOptions fo;
    Rig rig(env.be, src, cam);
    std::string err;
    const float rsharp = c.longEdge ? 0.2f : 0.f;
    int w = 0, h = 0;
    std::uint8_t* whole = rig.render(p, r, src, fo, 41, w, h, nullptr, err, c.longEdge, rsharp, lut);
    CHECK(whole != nullptr, "%s: %s", c.name, err.c_str());
    if (!whole) continue;
    for (int tile : tiles) {
      std::vector<std::uint8_t> img(std::size_t(w) * h * 4, 0);
      std::size_t covered = 0;
      auto sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
        for (int j = 0; j < th; ++j) std::memcpy(img.data() + (std::size_t(y + j) * w + x) * 4, rgba + std::size_t(j) * stride, std::size_t(tw) * 4);
        covered += std::size_t(tw) * th;
        return true;
      };
      engine::GpuStats st;
      const bool ok = rig.exportTiled(p, r, src, c.longEdge, 41, tile, false, rsharp, nullptr, sink, &st, err, lut);
      CHECK(ok, "%s tile %d: export: %s", c.name, tile, err.c_str());
      if (!ok) continue;
      CHECK(covered == std::size_t(w) * h, "covered %zu of %zu", covered, std::size_t(w) * h);
      int maxd = 0;
      std::size_t nd = 0;
      for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(img[4 * i + k]) - int(whole[4 * i + k])));
        maxd = std::max(maxd, d);
        nd += d > 0;
      }
      std::printf("  %-48s tile %4d: %3d tiles  max channel diff %d/255, %zu differing px  (%d passes, %.0f ms)\n", c.name, tile, st.tiles, maxd, nd,
                  st.passes, st.wallMs);
      CHECK(nd == 0, "film seam: %zu differing pixels (tile %d, %s)", nd, tile, c.name);
    }
    std::free(whole);
  }
}

// EngineSession::renderExact (the exact variant: ToneColourExact) on the GPU executor against the CPU reference renderer. The device evaluates the
// exact colour stage through a 65^3 lattice baked with the exact analysis: the budget is the lattice's, not bit identity.
void testExact(Env& env) {
  std::printf("== renderExact on the GPU (ToneColourExact via the dense lattice) vs Renderer::render ==\n");
  const SourceImage src = makeTestImage(900, 600);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool tiled;
    bool lut;
  };
  const std::vector<Case> cases = {
      {"tone + colour", [](EditParams& p) { p.contrast = 20; p.saturation = 15; p.vibrance = 20; p.highlights = -30; p.shadows = 25; }, false, false},
      {"HSL + grading + curves", [](EditParams& p) {
         p.hsl[kRed].hue = 20; p.hsl[kBlue].saturation = -30; p.gradeShadows.hue = 220; p.gradeShadows.saturation = 25;
         p.toneCurve = {{0.f, 0.f}, {96.f, 80.f}, {255.f, 255.f}}; }, false, false},
      {"look LUT + spatial", [](EditParams& p) { p.lutAmount = 80; p.sharpen = 40; p.nrLuma = 20; p.dehaze = 20; p.vignette = -20; }, false, true},
      {"film portra", [](EditParams& p) { p.filmStock = "kodak_portra_400"; }, false, false},
      {"film strength 60 + curves", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.filmStrength = 60; p.toneCurve = {{0.f, 0.f}, {128.f, 107.f}, {255.f, 255.f}}; },
       false, false},
      {"tiled: tone + colour + spatial", [](EditParams& p) { p.contrast = 20; p.saturation = 15; p.sharpen = 40; p.clarity = 25; p.dehaze = 20; }, true, false},
      {"tiled: film strength 70 + LUT", [](EditParams& p) { p.filmStock = "kodak_ektar_100"; p.filmStrength = 70; p.lutAmount = 60; p.sharpen = 30; }, true, true},
  };
  const CameraProfile& cam = env.cams.resolve("", "");
  for (const Case& c : cases) {
    EditParams p = baseParams();
    c.set(p);
    const Lut3D* lut = c.lut ? &testLut() : nullptr;
    Renderer r(p, cam, lut);
    Rig rig(env.be, src, cam);
    Image8 gpu;
    std::string err;
    engine::EngineSession::ExactOptions o;
    o.mode = c.tiled ? engine::ExecMode::TiledExport : engine::ExecMode::Whole;
    o.tile = c.tiled ? 256 : 0;
    const bool ok = rig.es.renderExact(p, r, src, o, gpu, &err);
    const Image8 cpu = r.render(src);
    CHECK(ok, "%s: %s", c.name, err.c_str());
    CHECK(rig.es.stats().gpuFrames == 1, "%s: not rendered by the GPU executor (gpuFrames %llu)", c.name, (unsigned long long)rig.es.stats().gpuFrames);
    if (!ok || gpu.data.size() != cpu.data.size()) {
      CHECK(false, "%s: size %zu vs %zu", c.name, gpu.data.size(), cpu.data.size());
      continue;
    }
    std::vector<std::uint8_t> a(std::size_t(gpu.width) * gpu.height * 4), b(a.size());
    for (std::size_t i = 0, n = std::size_t(gpu.width) * gpu.height; i < n; ++i)
      for (int k = 0; k < 3; ++k) {
        a[4 * i + k] = gpu.data[3 * i + k];
        b[4 * i + k] = cpu.data[3 * i + k];
        a[4 * i + 3] = b[4 * i + 3] = 255;
      }
    const DeStats d = compareRgba8(a.data(), b.data(), std::size_t(gpu.width) * gpu.height);
    std::printf("  %-34s %dx%d  mean %.4f  p99 %.3f  max %.2f\n", c.name, gpu.width, gpu.height, d.mean, d.p99, d.max);
    CHECK(d.mean <= 0.1 && d.p99 <= 1.0, "exact %s: mean %.3f p99 %.3f", c.name, d.mean, d.p99);
  }
}

// Coverage: no edit the app can produce on a film frame may come back Unsupported from the GPU executor (the rig forces the executor, so an
// unsupported plan is an error). Film strength 0..100 x look LUT x display curves x tiled export x exact variant, every stock of the library
// at both quality tiers, with geometry / vignette / the window nodes on top.
void testNoFallback(Env& env) {
  std::printf("== no film edit falls back to the CPU: strength x LUT x curves x tiled x exact, every stock x quality ==\n");
  const SourceImage src = makeTestImage(320, 224);
  const CameraProfile& cam = env.cams.resolve("", "");
  int total = 0, onGpu = 0;
  auto note = [&](const char* what, bool ok, bool gpu, const std::string& err) {
    ++total;
    onGpu += gpu ? 1 : 0;
    if (!(ok && gpu)) {
      CHECK(false, "%s: %s", what, ok ? "rendered by the CPU" : err.c_str());
    }
  };
  {
    Rig rig(env.be, src, cam);
    const float strengths[] = {0.f, 1.f, 25.f, 50.f, 75.f, 99.f, 100.f};
    for (float st : strengths)
      for (int lutOn = 0; lutOn < 2; ++lutOn)
        for (int curvesOn = 0; curvesOn < 2; ++curvesOn)
          for (int tiled = 0; tiled < 2; ++tiled)
            for (int exact = 0; exact < 2; ++exact) {
              EditParams p = baseParams();
              p.filmStock = "kodak_portra_400";
              p.filmStrength = st;
              p.sharpen = 20;
              p.vignette = -10;
              if (lutOn) p.lutAmount = 65;
              if (curvesOn) {
                p.toneCurve = {{0.f, 0.f}, {100.f, 84.f}, {255.f, 255.f}};
                p.toneCurveGreen = {{0.f, 8.f}, {255.f, 250.f}};
              }
              const Lut3D* lut = lutOn ? &testLut() : nullptr;
              Renderer r(p, cam, lut);
              char name[160];
              std::snprintf(name, sizeof name, "strength %.0f lut %d curves %d tiled %d exact %d", st, lutOn, curvesOn, tiled, exact);
              std::string err;
              const std::uint64_t before = rig.es.stats().gpuFrames;
              bool ok = false;
              if (exact) {
                Image8 out;
                engine::EngineSession::ExactOptions o;
                o.mode = tiled ? engine::ExecMode::TiledExport : engine::ExecMode::Whole;
                o.tile = tiled ? 128 : 0;
                ok = rig.es.renderExact(p, r, src, o, out, &err) && !out.data.empty();
              } else if (tiled) {
                std::size_t covered = 0;
                int w = 0, h = 0;
                ok = rig.exportTiled(p, r, src, 0, 41, 128, false, 0.f, [&](int ow, int oh) { w = ow; h = oh; },
                                     [&](int, int, int tw, int th, const std::uint8_t*, std::size_t) { covered += std::size_t(tw) * th; return true; }, nullptr, err, lut);
                ok = ok && covered == std::size_t(w) * h;
              } else {
                FastOptions fo;
                fo.spatial = SpatialQuality::Full;
                int w = 0, h = 0;
                std::uint8_t* px = rig.render(p, r, src, fo, 41, w, h, nullptr, err, 0, 0.f, lut);
                ok = px != nullptr;
                std::free(px);
              }
              note(name, ok, rig.es.stats().gpuFrames == before + 1, err);
            }
  }
  {
    Rig rig(env.be, src, cam);
    const auto lib = film::defaultLibrary();
    int stocks = 0;
    for (const std::string& id : lib->names()) {
      const film::FilmStock* st = lib->find(id);
      if (!st || st->isPrintMedium()) continue;
      ++stocks;
      for (int tier = 0; tier < 2; ++tier)
        for (float strength : {100.f, 45.f}) {
          EditParams p = baseParams();
          p.filmStock = id;
          p.filmStrength = strength;
          p.straighten = 1.5f;
          Renderer r(p, cam, nullptr);
          FastOptions fo;
          fo.spatial = tier ? SpatialQuality::Full : SpatialQuality::Interactive;
          fo.noise = tier != 0;
          char name[160];
          std::snprintf(name, sizeof name, "stock %s %s strength %.0f", id.c_str(), tier ? "final" : "interactive", strength);
          std::string err;
          int w = 0, h = 0;
          const std::uint64_t before = rig.es.stats().gpuFrames;
          std::uint8_t* px = rig.render(p, r, src, fo, 41, w, h, nullptr, err);
          note(name, px != nullptr, rig.es.stats().gpuFrames == before + 1, err);
          std::free(px);
        }
    }
    std::printf("  %d film stocks checked\n", stocks);
  }
  std::printf("  %d cases, %d rendered by the GPU executor\n", total, onGpu);
  CHECK(total == onGpu, "%d of %d cases did not run on the GPU", total - onGpu, total);
}

// Timings of the film frames that used to fall back to the CPU: "before" = the CPU engine (what the app did), "after" = the GPU executor.
// 1600 px frames (steady state, wall time incl. readback) and a 24 MP export (tiled GPU export vs the CPU film export, which ran whole).
// Interactive film frames through the session path (what the app does while a slider moves): per-frame wall time split into the executor's
// phases (upload = film tables / plan / display on the device, record, submit, wait = GPU + fence, readback copy) for a still frame and
// for strength / exposure / grain / halation drags. `--film-interactive [long edge]`.
void testFilmInteractive(Env& env, int longEdge) {
  using Clk = std::chrono::steady_clock;
  auto ms = [](Clk::time_point t0) { return std::chrono::duration<double, std::milli>(Clk::now() - t0).count(); };
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
  };
  const CameraProfile& cam = env.cams.resolve("", "");
  const int sw = 2000, sh = 1500;
  const SourceImage src = makeTestImage(sw, sh);
  struct Case {
    const char* name;
    std::function<void(EditParams&, int)> set;  // frame index i -> parameter value
    bool keepGrain;
  };
  const std::vector<Case> cases = {
      {"still frame", [](EditParams&, int) {}, false},
      {"film strength drag", [](EditParams& p, int i) { p.filmStrength = 100.f - 3.f * float(i % 10); }, false},
      {"film exposure drag", [](EditParams& p, int i) { p.filmExposure = 0.05f * float(i % 10); }, false},
      {"exposure drag (normal slider)", [](EditParams& p, int i) { p.exposure = 0.3f + 0.05f * float(i % 10); }, false},
      {"grain drag (keepGrain)", [](EditParams& p, int i) { p.filmGrain = 100.f - 4.f * float(i % 10); }, true},
      {"halation drag", [](EditParams& p, int i) { p.filmHalation = 100.f - 4.f * float(i % 10); }, false},
      {"film warmth drag", [](EditParams& p, int i) { p.filmWarmth = 4.f * float(i % 10); }, false},
  };
  std::printf("== film interactive frames through the session, long edge %d (source %dx%d): ms, medians of 12 ==\n", longEdge, sw, sh);
  std::printf("  %-32s %7s %6s %7s %7s %7s %7s %7s %6s %s\n", "case", "wall", "gpu", "upload", "record", "submit", "wait", "copy", "passes", "(rebuild Renderer)");
  Rig rig(env.be, src, cam);
  for (const Case& c : cases) {
    std::vector<double> wall, gpu, up, rec, sub, wt, cp, rb;
    int passes = 0;
    for (int i = 0; i < 14; ++i) {
      EditParams p = baseParams();
      p.filmStock = "kodak_portra_400";
      c.set(p, i);
      auto t0 = Clk::now();
      Renderer r(p, cam, nullptr);
      const double rbMs = ms(t0);
      FastOptions fo;
      fo.spatial = SpatialQuality::Interactive;
      fo.noise = false;
      fo.keepGrain = c.keepGrain;
      fo.fullLongEdge = c.keepGrain ? sw : 0;
      engine::GpuStats st;
      std::string err;
      int w = 0, h = 0;
      t0 = Clk::now();
      std::uint8_t* px = rig.render(p, r, src, fo, 41, w, h, &st, err, longEdge, 0.f, nullptr);
      const double wl = ms(t0);
      CHECK(px != nullptr, "%s: %s", c.name, err.c_str());
      if (!px) break;
      std::free(px);
      if (i == 0 && c.keepGrain == false && std::string(c.name) == "still frame")
        std::printf("  first film frame of the session: wall %.1f ms (upload %.1f = tables + plan + display, record %.1f incl. pipeline creation, wait %.1f)\n", wl, st.uploadMs, st.recordMs, st.waitMs);
      if (i >= 2) {
        wall.push_back(wl), gpu.push_back(st.gpuMs), up.push_back(st.uploadMs), rec.push_back(st.recordMs), sub.push_back(st.submitMs);
        wt.push_back(st.waitMs), cp.push_back(st.readbackMs), rb.push_back(rbMs);
        passes = st.passes;
      }
    }
    std::printf("  %-32s %7.2f %6.2f %7.2f %7.2f %7.2f %7.2f %7.2f %6d (%.2f)\n", c.name, median(wall), median(gpu), median(up), median(rec), median(sub),
                median(wt), median(cp), passes, median(rb));
  }
}

void testFilmTiming(Env& env, bool with24mp) {
  using Clk = std::chrono::steady_clock;
  auto ms = [](Clk::time_point t0) { return std::chrono::duration<double, std::milli>(Clk::now() - t0).count(); };
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
  };
  const CameraProfile& cam = env.cams.resolve("", "");
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool lut;
  };
  const std::vector<Case> cases = {
      {"film strength 100 (reference, was GPU)", [](EditParams& p) { p.filmStock = "kodak_portra_400"; }, false},
      {"film strength 60 (Blend)", [](EditParams& p) { p.filmStock = "kodak_portra_400"; p.filmStrength = 60; }, false},
      {"film + curves + look LUT", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.toneCurve = {{0.f, 0.f}, {96.f, 80.f}, {255.f, 255.f}}; p.lutAmount = 70; }, true},
      {"film strength 60 + curves + LUT", [](EditParams& p) {
         p.filmStock = "kodak_portra_400"; p.filmStrength = 60; p.toneCurve = {{0.f, 0.f}, {96.f, 80.f}, {255.f, 255.f}}; p.lutAmount = 70; }, true},
  };
  std::printf("== film timings, 1600 px frame (final tier): CPU engine vs GPU executor ==\n");
  {
    const SourceImage src = makeTestImage(1600, 1067);
    for (const Case& c : cases) {
      EditParams p = baseParams();
      c.set(p);
      const Lut3D* lut = c.lut ? &testLut() : nullptr;
      Renderer r(p, cam, lut);
      FastOptions fo;
      fo.spatial = SpatialQuality::Full;
      Rig rig(env.be, src, cam);
      engine::EngineSession cpuEs;  // no executor override and no provider in this binary: the CPU executor
      std::vector<double> tg, tc;
      std::string err;
      for (int i = 0; i < 6; ++i) {
        int w = 0, h = 0;
        auto t0 = Clk::now();
        std::uint8_t* px = rig.render(p, r, src, fo, 41, w, h, nullptr, err, 0, 0.f, lut);
        const double g = ms(t0);
        std::free(px);
        if (i >= 1) tg.push_back(g);
        if (i < 3) {
          engine::FrameJob job{p, r, src, nullptr, cam, lut, {}, {}, {}};
          job.request.bakeSize = 41;
          job.fastOptions = fo;
          engine::FrameResult res;
          t0 = Clk::now();
          cpuEs.renderFrame(job, res);
          if (i >= 1) tc.push_back(ms(t0));
          std::free(res.rgba);
        }
      }
      std::printf("  %-40s CPU %8.1f ms   GPU %7.1f ms   (x%.1f)\n", c.name, median(tc), median(tg), median(tc) / std::max(1e-3, median(tg)));
    }
  }
  if (!with24mp) return;
  std::printf("== film timings, 24 MP (6000x4000) export: CPU film export vs tiled GPU export ==\n");
  const SourceImage big = makeTestImage(6000, 4000);
  for (const Case& c : cases) {
    EditParams p = baseParams();
    c.set(p);
    const Lut3D* lut = c.lut ? &testLut() : nullptr;
    Renderer r(p, cam, lut);
    Rig rig(env.be, big, cam);
    std::string err;
    int w = 0, h = 0;
    double gpuMs[2] = {0, 0};
    for (int rep = 0; rep < 2; ++rep) {  // the second run has the device source and film tables warm
      auto t0 = Clk::now();
      std::vector<std::uint8_t> sink8;
      const bool ok = rig.exportTiled(p, r, big, 0, 65, 2048, false, 0.f, [&](int ow, int oh) { w = ow; h = oh; sink8.assign(std::size_t(ow) * oh * 4, 0); },
                                      [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
                                        for (int j = 0; j < th; ++j) std::memcpy(sink8.data() + (std::size_t(y + j) * w + x) * 4, rgba + std::size_t(j) * stride, std::size_t(tw) * 4);
                                        return true;
                                      },
                                      nullptr, err, lut);
      gpuMs[rep] = ms(t0);
      CHECK(ok, "%s: GPU export: %s", c.name, err.c_str());
      if (!ok) break;
    }
    engine::EngineSession cpuEs;
    Image8 out;
    engine::EngineSession::ExactOptions o;
    auto t0 = Clk::now();
    const bool okc = cpuEs.renderExact(p, r, big, o, out, &err);
    const double cpuMs = ms(t0);
    CHECK(okc, "%s: CPU export: %s", c.name, err.c_str());
    std::printf("  %-40s CPU %9.0f ms   GPU %8.0f ms (first %8.0f ms)   (x%.1f)\n", c.name, cpuMs, gpuMs[1], gpuMs[0], cpuMs / std::max(1e-3, gpuMs[1]));
    std::printf("      transient pool kept after the export: %.0f MB (%.0f bytes per pixel)\n", double(env.be->pooledBytes()) / 1048576.0,
                double(env.be->pooledBytes()) / (6000.0 * 4000.0));
  }
}

// The device keeps the frame-global solves: a frame that only changes parameters downstream of the dehaze (sharpen) is planned without the
// accumulation sweep and the solve, and renders the same pixels as a session that has to solve.
void testSolveCache(Env& env) {
  std::printf("== device solve cache: dehaze solve kept between frames ==\n");
  const SourceImage src = makeTestImage(900, 600);
  const CameraProfile& cam = env.cams.resolve("", "");
  FastOptions fo;
  fo.spatial = SpatialQuality::Full;
  auto render = [&](Rig& rig, int sharpen, engine::GpuStats* st) {
    EditParams p = baseParams();
    p.dehaze = 35;
    p.sharpen = float(sharpen);
    Renderer r(p, cam, nullptr);
    int w = 0, h = 0;
    std::string err;
    std::uint8_t* px = rig.render(p, r, src, fo, 41, w, h, st, err);
    CHECK(px != nullptr, "render: %s", err.c_str());
    return std::make_pair(px, std::size_t(w) * h * 4);
  };
  Rig warm(env.be, src, cam), cold(env.be, src, cam);
  engine::GpuStats s1, s2, s3;
  auto a = render(warm, 20, &s1);   // solves (miss), stores the grid
  auto b = render(warm, 45, &s2);   // sharpen changed: the dehaze solve is a hit
  auto c = render(cold, 45, &s3);   // fresh session: solves
  std::printf("  passes: first %d, solve kept %d, fresh session %d\n", s1.passes, s2.passes, s3.passes);
  CHECK(s2.passes < s3.passes, "the kept solve must save passes (%d vs %d)", s2.passes, s3.passes);
  if (b.first && c.first) {
    const DeStats d = compareRgba8(b.first, c.first, b.second / 4);
    std::printf("  kept solve vs fresh solve: mean %.5f max %.3f\n", d.mean, d.max);
    CHECK(std::memcmp(b.first, c.first, b.second) == 0, "a kept solve must give the same pixels as solving again");
  }
  std::free(a.first);
  std::free(b.first);
  std::free(c.first);
  // the guided filter coefficients (clarity) are kept too: the same frame again needs neither accumulation sweep nor solve
  {
    Rig rig(env.be, src, cam);
    engine::GpuStats f1, f2;
    EditParams p = baseParams();
    p.dehaze = 25;
    p.clarity = 35;
    p.sharpen = 30;
    Renderer r(p, cam, nullptr);
    int w = 0, h = 0;
    std::string err;
    std::uint8_t* x1 = rig.render(p, r, src, fo, 41, w, h, &f1, err);
    std::uint8_t* x2 = rig.render(p, r, src, fo, 41, w, h, &f2, err);
    CHECK(x1 && x2, "render: %s", err.c_str());
    std::printf("  dehaze + clarity, same frame twice: passes %d -> %d\n", f1.passes, f2.passes);
    CHECK(f2.passes < f1.passes, "both solves kept: %d vs %d passes", f2.passes, f1.passes);
    if (x1 && x2) CHECK(std::memcmp(x1, x2, std::size_t(w) * h * 4) == 0, "kept solves must reproduce the frame");
    std::free(x1);
    std::free(x2);
  }
}

void testSteadyState(Env& env) {
  std::printf("== steady state: no allocation after warm-up ==\n");
  const SourceImage src = makeTestImage(1024, 700);
  EditParams p = baseParams();
  p.nrLuma = 30;
  p.clarity = 30;
  p.sharpen = 50;
  p.dehaze = 20;
  const CameraProfile& cam = env.cams.resolve("", "");
  Rig rig(env.be, src, cam);
  std::string err;
  std::size_t pooled = 0;
  for (int i = 0; i < 12; ++i) {
    p.exposure = 0.1f * float(i % 5);  // parameters change every frame: new bake, new lattice, same sizes
    Renderer r(p, cam, nullptr);
    int w, h;
    engine::GpuStats st;
    std::uint8_t* o = rig.render(p, r, src, FastOptions{}, 41, w, h, &st, err);
    CHECK(o != nullptr, "%s", err.c_str());
    std::free(o);
    if (i == 3) pooled = env.be->pooledBytes();
    if (i > 3) CHECK(env.be->pooledBytes() == pooled, "pool grew in frame %d: %zu -> %zu", i, pooled, env.be->pooledBytes());
    if (i == 11) std::printf("  frame %d: %d passes, %d barriers, %.2f ms wall, pool %.1f MB\n", i, st.passes, st.barriers, st.wallMs, env.be->pooledBytes() / 1048576.0);
  }
}

// The C API hook (what the app uses): the same session rendered with the flag off and on.
void testCApiHook(Env& env) {
  std::printf("== C API hook: dapple_session_render_frame / dapple_session_render, GPU flag off vs on ==\n");
  const SourceImage scene = makeTestImage(1400, 933);
  EditParams base = baseParams();
  const CameraProfile& cam = env.cams.resolve("", "");
  Renderer r0(base, cam, nullptr);
  const Image8 jpg = r0.render(scene);
  const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/dapple_gpu_hook_test.jpg";
  if (!writeJpeg(jpg, path)) {
    std::printf("  cannot write the test jpeg\n");
    ++g_fail;
    return;
  }
  DappleSession* s = dapple_session_open(path.c_str(), 0, 0);
  CHECK(s != nullptr, "open");
  if (!s) return;
  EditParams p = baseParams();
  p.sharpen = 70;  // the jpeg source is display referred: sharpen 40 / noise color 25 are the neutral levels
  p.nrColor = 60;
  p.clarity = 30;
  p.dehaze = 20;
  p.vignette = -20;
  p.straighten = 2.f;
  const std::string json = toJson(p);
  struct Req {
    const char* name;
    int flags;
    int longEdge;
    float region[4];
    int regionLong;
  };
  const Req reqs[] = {{"interactive 1000 px", DAPPLE_FRAME_INTERACTIVE, 1000, {0, 0, 0, 0}, 0},
                      {"full quality 1000 px", 0, 1000, {0, 0, 0, 0}, 0},
                      {"zoom region", DAPPLE_FRAME_INTERACTIVE, 0, {0.2f, 0.2f, 0.5f, 0.5f}, 700}};
  for (const Req& q : reqs) {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = q.longEdge;
    rq.flags = q.flags;
    std::memcpy(rq.region, q.region, sizeof rq.region);
    rq.region_long_edge = q.regionLong;
    DappleImage cpu{}, gpu{};
    dapple_gpu_set_enabled(0);
    CHECK(dapple_session_render_frame(s, json.c_str(), nullptr, &rq, &cpu, nullptr) == DAPPLE_OK, "cpu frame");
    dapple_gpu_set_enabled(1);
    CHECK(dapple_session_render_frame(s, json.c_str(), nullptr, &rq, &gpu, nullptr) == DAPPLE_OK, "gpu frame");
    if (cpu.rgba && gpu.rgba && cpu.width == gpu.width && cpu.height == gpu.height) {
      const DeStats d = compareRgba8(gpu.rgba, cpu.rgba, std::size_t(cpu.width) * cpu.height);
      std::printf("  render_frame %-22s %4dx%-4d mean %.4f p99 %.3f max %.2f  (differing %.1f%%)\n", q.name, cpu.width, cpu.height, d.mean, d.p99, d.max, d.differing * 100);
      // a downscaled frame is resampled differently (GPU: Lanczos-3, CPU: box average), on a texture-dense test picture
      CHECK(d.mean <= (q.longEdge ? 2.0 : 0.5), "hook frame mean %.3f", d.mean);
      CHECK(!(d.mean == 0 && d.max == 0), "GPU path was not taken (identical to the CPU result)");
    } else {
      std::printf("  render_frame %s: size mismatch %dx%d vs %dx%d\n", q.name, cpu.width, cpu.height, gpu.width, gpu.height);
      ++g_fail;
    }
    dapple_free_image(&cpu);
    dapple_free_image(&gpu);
  }
  // export path (exact CPU reference vs the baked, tiled GPU render)
  DappleImage cpu{}, gpu{};
  dapple_gpu_set_enabled(0);
  CHECK(dapple_session_render(s, json.c_str(), nullptr, 0, &cpu) == DAPPLE_OK, "cpu render");
  dapple_gpu_set_enabled(1);
  CHECK(dapple_session_render(s, json.c_str(), nullptr, 0, &gpu) == DAPPLE_OK, "gpu render");
  if (cpu.rgba && gpu.rgba && cpu.width == gpu.width && cpu.height == gpu.height) {
    const DeStats d = compareRgba8(gpu.rgba, cpu.rgba, std::size_t(cpu.width) * cpu.height);
    std::printf("  export (exact CPU vs baked GPU)  %dx%d mean %.4f p99 %.3f max %.2f\n", cpu.width, cpu.height, d.mean, d.p99, d.max);
    CHECK(d.mean <= 1.0, "hook export mean %.3f", d.mean);
  } else {
    ++g_fail;
  }
  dapple_free_image(&cpu);
  dapple_free_image(&gpu);
  char* st = dapple_gpu_status_json();
  std::printf("  status: %s\n", st);
  dapple_free_string(st);
  dapple_gpu_set_enabled(-1);
  dapple_session_close(s);
  std::remove(path.c_str());
}

void testBlocklist() {
  std::printf("== blocklist ==\n");
  eg::ContextOptions o;
  o.device.allowSoftware = true;
  o.selfTest = false;
  // rule matching every device
  o.device.blocklist = "name=;reason=test rule";
  std::string err;
  auto b = eg::Context::create(o, err);
  CHECK(b == nullptr, "a match-all rule must block the device");
  std::printf("  match-all rule  -> %s\n", err.c_str());
  CHECK(err.find("test rule") != std::string::npos, "reason missing: %s", err.c_str());
  o.device.blocklist = "vendor=0xdead;reason=other vendor|name=NoSuchDevice;reason=other name";
  b = eg::Context::create(o, err);
  CHECK(b != nullptr, "non matching rules must not block: %s", err.c_str());
  // software devices are refused unless allowed
  eg::ContextOptions o2;
  o2.selfTest = false;
  auto b2 = eg::Context::create(o2, err);
  if (b2) {
    const bool soft = b2->caps().software;
    CHECK(!soft, "a CPU device was accepted without allowSoftware");
  } else {
    std::printf("  software device refused by default: %s\n", err.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = argc > 1 && std::string(argv[1]) == "--quick";
  {  // the film library: without it a film stock of an edit is ignored and the film rows would render the normal pipeline
    std::string ferr;
    namespace fs = std::filesystem;
    const char* envData = std::getenv("DAPPLE_FILM_DATA");  // on a phone: the pushed folder holding dapple.dfp
    const fs::path data = envData ? fs::path(envData) : fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film";
    std::shared_ptr<film::FilmLibrary> lib;
    if (fs::exists(data / "dapple.dfp")) lib = film::FilmLibrary::loadPack(data / "dapple.dfp", &ferr);
    if (!lib) lib = film::FilmLibrary::loadJsonDir(data / "profiles", &ferr);
    if (!lib) {
      std::printf("film library not found (%s)\n", ferr.c_str());
      return 1;
    }
    film::setLibrary(lib);
  }
  Env env;
  eg::ContextOptions o;
  o.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") && std::getenv("DAPPLE_GPU_SOFTWARE")[0] == '1';
  std::string err;
  o.selfTest = !(std::getenv("DAPPLE_GPU_TEST_NOSELF"));
  env.be = eg::Context::create(o, err);
  if (!env.be) {
    std::printf("SKIP/FAIL: no GPU backend: %s\n", err.c_str());
    // No Vulkan device at all is not a failure of the backend (CI machines): report as skipped (ctest SKIP_RETURN_CODE 77).
    return err.find("no Vulkan") != std::string::npos || err.find("no usable Vulkan device") != std::string::npos ? 77 : 1;
  }
  std::printf("%s\nself-test: %s\n\n", env.be->deviceText().c_str(), env.be->selfTestReport().c_str());
  if (argc > 1 && std::string(argv[1]) == "--exact") {
    testExact(env);
    testNoFallback(env);
    testSolveCache(env);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--film-interactive") {
    testFilmInteractive(env, argc > 2 ? std::atoi(argv[2]) : 1080);
    return g_fail ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--film-timing") {
    testFilmTiming(env, argc > 2 && std::string(argv[2]) == "24mp");
    return g_fail ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--solve-cache") {
    testSolveCache(env);
    return g_fail ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--coverage") {
    testNoFallback(env);
    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--film-tiling") {  // only the film seam test (with a 4000 px picture too on a real GPU)
    testFilmTiling(env);
    testExact(env);
    // the 4000 px picture needs a whole-frame film reference (288 bytes per pixel on the device): only where that fits
    if (!env.be->caps().software && std::uint64_t(4000) * 2667 * 288 * 20 / 9 <= env.be->caps().deviceLocalBytes) testFilmTiling(env, 4000, 2667, {512, 1000, 2048});
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
  }
  testStages(env);
  if (!quick) {
    testHalfBudget(env);
    testDownscale(env);
    testTiling(env, false);
    testTiling(env, true);
    testFilmTiling(env);
    if (!env.be->caps().software) testTiling(env, true, 4000, 2667, {512, 1000, 2048});  // radii ~2x: larger halos
    testSteadyState(env);
    testCApiHook(env);
    testBlocklist();
  }
  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
