// GPU backend tests (ctest "gpu_parity", "gpu_parity_swiftshader"): per-stage parity with the CPU reference, tiled
// export seams, streamed source, downscale, region, steady-state allocation, blocklist. Prints a table (mean / p99 / max
// Delta E 2000); the pass criterion of a stage is mean <= 0.5.
//
// Env: DAPPLE_GPU_SOFTWARE=1 accepts a CPU Vulkan device (SwiftShader); DAPPLE_GPU_DEVICE=<index>.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/encode.h"
#include "dapple/edit_params.h"
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
};

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
  Renderer r(p, cam, nullptr);
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
  std::uint8_t* gpu = rig.render(p, r, src, fo, 41, gw, gh, nullptr, err);
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
  testStages(env);
  if (!quick) {
    testHalfBudget(env);
    testDownscale(env);
    testTiling(env, false);
    testTiling(env, true);
    if (!env.be->caps().software) testTiling(env, true, 4000, 2667, {512, 1000, 2048});  // radii ~2x: larger halos
    testSteadyState(env);
    testCApiHook(env);
    testBlocklist();
  }
  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
