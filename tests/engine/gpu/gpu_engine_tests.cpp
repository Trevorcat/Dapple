// GPU engine tests (ctest "engine.gpu_*"): HAL + scheduler + the O(1) box filter + submission splitting + engine selection.
//   dapple_gpu_engine_tests [hal] [box] [sched] [select] [bench-box]
// Exit code 77 = no Vulkan device. Env: DAPPLE_GPU_SOFTWARE=1 accepts SwiftShader.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/gpu.h"
#include "dapple/gpu_hook.h"
#include "dapple/renderer.h"
#include "engine/exec/gpu_executor.h"
#include "engine/nodes/gpu_common.h"
#include "engine/nodes/gpu_nodes.h"
#include "parity.h"

#if defined(_WIN32)
#include <stdlib.h>
static void setEnv(const char* k, const char* v) { _putenv_s(k, v); }
static void unsetEnv(const char* k) { _putenv((std::string(k) + "=").c_str()); }
#else
static void setEnv(const char* k, const char* v) { setenv(k, v, 1); }
static void unsetEnv(const char* k) { unsetenv(k); }
#endif

using namespace dapple;
namespace eg = dapple::engine::gpu;

namespace {
int g_fail = 0;
#define CHECK(cond, ...)                                           \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                    \
      std::printf("\n");                                           \
      ++g_fail;                                                    \
    }                                                              \
  } while (0)

float h16(float x) {  // round to the nearest fp16 value (RNE), as common.glsl h16()
  std::uint32_t u;
  std::memcpy(&u, &x, 4);
  u += 0xFFFu + ((u >> 13) & 1u);
  u &= ~0x1FFFu;
  std::memcpy(&x, &u, 4);
  return x;
}

std::unique_ptr<eg::Core> makeCore(const hal::DeviceOptions& o0, std::string& err) {
  hal::DeviceOptions o = o0;
  if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) o.allowSoftware = v[0] == '1';
  return eg::Core::create(o, err);
}

std::vector<float> randomRgb(int w, int h, unsigned seed) {
  std::vector<float> v(std::size_t(w) * h * 3);
  unsigned s = seed;
  for (std::size_t i = 0; i < v.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    const float n = float((s >> 8) & 0xFFFF) / 65535.f;
    const std::size_t px = i / 3;
    const float smooth = 0.5f + 0.4f * std::sin(float(px % std::size_t(w)) * 0.013f + float(i % 3)) * std::cos(float(px / std::size_t(w)) * 0.021f);
    v[i] = std::clamp(0.7f * smooth + 0.3f * n, 0.f, 1.f);
  }
  return v;
}

// H then V box of the luma of the fp16-rounded input, replicated edges, double precision.
std::vector<double> cpuBox2D(const std::vector<float>& rgb, int w, int h, int R) {
  std::vector<double> y(std::size_t(w) * h), t(y.size());
  for (std::size_t i = 0; i < y.size(); ++i)
    y[i] = double(h16(rgb[3 * i])) * 0.2126729 + double(h16(rgb[3 * i + 1])) * 0.7151522 + double(h16(rgb[3 * i + 2])) * 0.0721750;
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      double s = 0;
      for (int k = -R; k <= R; ++k) s += y[std::size_t(j) * w + std::clamp(i + k, 0, w - 1)];
      t[std::size_t(j) * w + i] = s / (2 * R + 1);
    }
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      double s = 0;
      for (int k = -R; k <= R; ++k) s += t[std::size_t(std::clamp(j + k, 0, h - 1)) * w + i];
      y[std::size_t(j) * w + i] = s / (2 * R + 1);
    }
  return y;
}

struct BoxRun {
  std::vector<float> out;
  double gpuMsH = 0, gpuMsV = 0;
  hal::SubmitStats st;
  bool ok = false;
};

// luma -> box H -> box V on the GPU through GpuRecorder::sep (kinds 0 and 3), with the given scan threshold.
BoxRun gpuBox2D(eg::Core& core, const std::vector<float>& rgb, int w, int h, int R, int scanMin, bool readOut = true) {
  BoxRun r;
  hal::Device& dev = core.dev();
  hal::ImageH in;
  std::string err;
  if (dev.createImage({hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h), 1}, &in) != hal::Status::Ok) return r;
  if (!core.uploadRgbF32(rgb.data(), std::size_t(w) * 3, w, h, in, 0, 0, err)) {
    std::printf("  upload failed: %s\n", err.c_str());
    dev.destroy(in);
    return r;
  }
  auto cl = dev.begin();
  eg::GpuGraph g(dev, core.pool());
  eg::GpuRecorder rec(dev, g, err);
  rec.weights = core.slot(cl->slot()).weights;
  rec.boxScanMin = scanMin;
  rec.W = w;
  rec.H_ = h;
  const eg::H hin = g.external(in);
  const eg::H t = rec.sep(0, hin, w, h, 0, 0, {R, 0, 0, 0}, eg::kR32);
  const eg::H o = t < 0 ? -1 : rec.sep(3, t, w, h, 1, 0, {R, 0, 0, 0}, eg::kR32);
  if (o < 0) {
    std::printf("  record failed: %s\n", err.c_str());
    dev.destroy(in);
    return r;
  }
  if (readOut) {
    core.ensureReadback(cl->slot(), std::size_t(w) * h * 4, err);
    g.copyImageToBuffer(o, core.slot(cl->slot()).readback, 0, 0, std::uint32_t(w), std::uint32_t(h));
  }
  hal::FenceH f;
  if (eg::runGraph(dev, g, *cl, &f, err) != hal::Status::Ok || dev.wait(f, 60ull * 1000 * 1000 * 1000) != hal::Status::Ok) {
    std::printf("  submit failed: %s\n", err.c_str());
    dev.destroy(in);
    return r;
  }
  r.st = dev.stats(f);
  int seen = 0;
  for (const auto& p : dev.passTimes(f))
    if (p.name == "boxscan" || p.name == "box") (seen++ == 0 ? r.gpuMsH : r.gpuMsV) += p.ms;
  if (readOut) {
    r.out.resize(std::size_t(w) * h);
    std::memcpy(r.out.data(), core.slot(cl->slot()).readback.mapped, r.out.size() * 4);
  }
  dev.waitIdle();
  dev.destroy(in);
  r.ok = true;
  return r;
}

// ---------------------------------------------------------------------------------------------------- tests ---
int testHal() {
  std::printf("== HAL: device, buffers, images, convert + readback ==\n");
  std::string err;
  auto core = makeCore({}, err);
  if (!core) {
    std::printf("  no device: %s\n", err.c_str());
    return 77;
  }
  hal::Device& dev = core->dev();
  const auto& c = dev.caps();
  std::printf("  %s  api %d  key %s  unified %d  timestamps %d\n", c.name.c_str(), int(c.api), c.key().c_str(), int(c.unifiedMemory), int(c.timestamps));
  for (hal::Format f : {hal::Format::RGBA16F, hal::Format::RGBA32F, hal::Format::R32F, hal::Format::RGBA8})
    CHECK(c.format[int(f)].sampled && c.format[int(f)].storageWrite, "format %d caps", int(f));
  CHECK(c.maxImage2D >= 4096 && c.maxSharedBytes >= 16384, "limits");
  // fp16 roundtrip: float RGB -> RGBA16F (convert pass) -> readback; the stored value is h16(x)
  const int w = 37, h = 19;
  const std::vector<float> rgb = randomRgb(w, h, 7);
  hal::ImageH im16;
  CHECK(dev.createImage({hal::Format::RGBA16F, w, h, 1}, &im16) == hal::Status::Ok, "image16");
  CHECK(core->uploadRgbF32(rgb.data(), std::size_t(w) * 3, w, h, im16, 0, 0, err), "upload: %s", err.c_str());
  // unknown shader / bad signature are reported, not crashed
  hal::PipelineH p;
  CHECK(dev.createPipeline({"no_such_shader", {}, ""}, &p) == hal::Status::Unsupported, "unknown shader");
  CHECK(dev.createPipeline({"down", {}, "II"}, &p) == hal::Status::InvalidArgument, "bad signature");
  CHECK(dev.createPipeline({"down", {{0, 0}}, "SI"}, &p) == hal::Status::Ok && p.bindings && std::strcmp(p.bindings, "SI") == 0, "pipeline");
  {
    auto cl = dev.begin();
    CHECK(core->ensureReadback(cl->slot(), std::size_t(w) * h * 8, err), "readback");
    eg::GpuGraph g(dev, core->pool());
    g.copyImageToBuffer(g.external(im16), core->slot(cl->slot()).readback, 0, 0, w, h);
    hal::FenceH f;
    CHECK(eg::runGraph(dev, g, *cl, &f, err) == hal::Status::Ok, "%s", err.c_str());
    CHECK(dev.wait(f, 30ull * 1000 * 1000 * 1000) == hal::Status::Ok, "wait");
    const auto* hp = static_cast<const std::uint16_t*>(core->slot(cl->slot()).readback.mapped);
    auto half2f = [](std::uint16_t hv) {
      const std::uint32_t s = (hv >> 15) & 1, e = (hv >> 10) & 31, m = hv & 1023;
      float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), int(e) - 25);
      return s ? -v : v;
    };
    double worst = 0;
    for (int i = 0; i < w * h; ++i)
      for (int k = 0; k < 3; ++k) worst = std::max(worst, double(std::fabs(half2f(hp[4 * i + k]) - h16(rgb[3 * i + k]))));
    std::printf("  fp16 roundtrip max |gpu - h16(x)| = %.3g\n", worst);
    CHECK(worst == 0.0, "the GPU store must equal the explicit round-to-nearest-even");
  }
  dev.destroy(im16);
  // GpuImpl table: every main node has an implementation, the shaders it names exist
  using engine::NodeType;
  for (NodeType t : {NodeType::Geometry, NodeType::Gain, NodeType::ToneColour, NodeType::Vignette, NodeType::NRLuma, NodeType::NRChroma, NodeType::Dehaze,
                     NodeType::Detail, NodeType::Resize, NodeType::Grain, NodeType::Pack8}) {
    const auto* impl = eg::gpuImplFor(t);
    CHECK(impl != nullptr, "node %d has no GpuImpl", int(t));
  }
  int built = 0;
  for (const char* n : eg::allShaderNames()) {
    hal::PipelineH q;
    const hal::Status s = dev.createPipeline({n, {}, ""}, &q);
    CHECK(s == hal::Status::Ok, "pipeline %s: %s", n, dev.lastError().c_str());
    built += s == hal::Status::Ok;
  }
  std::printf("  %d shader pipelines built, %zu in the GpuImpl table\n", built, eg::allShaderNames().size());
  return 0;
}

int testBox() {
  std::printf("== box filter: running-sum scan vs direct sum vs double precision CPU reference ==\n");
  std::string err;
  auto core = makeCore({}, err);
  if (!core) return 77;
  std::printf("  %-10s %-3s  %-26s %-26s %s\n", "size", "R", "scan max / mean abs err", "direct max / mean abs err", "scan-vs-direct max");
  for (auto [w, h] : {std::pair{1531, 517}, std::pair{700, 1300}, std::pair{97, 61}})
    for (int R : {1, 5, 12, 30, 64}) {
      const auto rgb = randomRgb(w, h, unsigned(w * 31 + R));
      const auto ref = cpuBox2D(rgb, w, h, R);
      const BoxRun scan = gpuBox2D(*core, rgb, w, h, R, 1), direct = gpuBox2D(*core, rgb, w, h, R, 0);
      CHECK(scan.ok && direct.ok, "run");
      if (!scan.ok || !direct.ok) continue;
      auto stat = [&](const std::vector<float>& o, double& mx, double& mean) {
        mx = mean = 0;
        for (std::size_t i = 0; i < ref.size(); ++i) {
          const double e = std::fabs(double(o[i]) - ref[i]);
          mx = std::max(mx, e);
          mean += e;
        }
        mean /= double(ref.size());
      };
      double sm, sa, dm, da, sd = 0;
      stat(scan.out, sm, sa);
      stat(direct.out, dm, da);
      for (std::size_t i = 0; i < ref.size(); ++i) sd = std::max(sd, double(std::fabs(scan.out[i] - direct.out[i])));
      std::printf("  %4dx%-4d %3d      %9.3g / %-9.3g       %9.3g / %-9.3g     %.3g\n", w, h, R, sm, sa, dm, da, sd);
      // two passes of quantisation (7.6e-6 each, averaged) on top of the fp32 error of the direct path
      CHECK(sm <= 3e-5, "scan max error %.3g", sm);
      CHECK(dm <= 2e-6, "direct max error %.3g", dm);
    }
  // tile invariance: a window of the picture processed alone (halo included) equals the same pixels of the whole picture,
  // bit for bit, because integer sums do not depend on where a segment starts.
  {
    const int w = 1531, h = 517, R = 40;
    const auto rgb = randomRgb(w, h, 99);
    const BoxRun whole = gpuBox2D(*core, rgb, w, h, R, 1);
    const int x0 = 301, y0 = 77, tw = 600, th = 300, halo = 2 * R + 8;
    const int ax = std::max(0, x0 - halo), ay = std::max(0, y0 - halo), bx = std::min(w, x0 + tw + halo), by = std::min(h, y0 + th + halo);
    std::vector<float> sub(std::size_t(bx - ax) * (by - ay) * 3);
    for (int y = ay; y < by; ++y)
      std::memcpy(&sub[std::size_t(y - ay) * (bx - ax) * 3], &rgb[(std::size_t(y) * w + ax) * 3], std::size_t(bx - ax) * 12);
    const BoxRun part = gpuBox2D(*core, sub, bx - ax, by - ay, R, 1);
    long diff = 0;
    for (int y = y0; y < y0 + th; ++y)
      for (int x = x0; x < x0 + tw; ++x)
        if (part.out[std::size_t(y - ay) * (bx - ax) + (x - ax)] != whole.out[std::size_t(y) * w + x]) ++diff;
    std::printf("  tile invariance (window with halo vs whole picture): %ld differing pixels of %d\n", diff, tw * th);
    CHECK(diff == 0, "scan box is not tile invariant");
  }
  return 0;
}

int testSched() {
  std::printf("== scheduler: layering, barrier count, steady state, submission splitting ==\n");
  std::string err;
  auto record = [&](eg::Core& core, int R, std::vector<float>* out, hal::SubmitStats* st, std::size_t* pooled) {
    hal::Device& dev = core.dev();
    const int w = 640, h = 480;
    const auto rgb = randomRgb(w, h, 5);
    hal::ImageH in;
    dev.createImage({hal::Format::RGBA16F, w, h, 1}, &in);
    core.uploadRgbF32(rgb.data(), std::size_t(w) * 3, w, h, in, 0, 0, err);
    auto cl = dev.begin();
    eg::GpuGraph g(dev, core.pool());
    eg::GpuRecorder rec(dev, g, err);
    rec.weights = core.slot(cl->slot()).weights;
    rec.boxScanMin = 0;  // direct sums: two passes per chain, deterministic count
    rec.W = w;
    rec.H_ = h;
    const eg::H hin = g.external(in);
    // two independent chains (H box, V box) of the same input at two radii: 2 levels, 1 barrier between them
    const eg::H a1 = rec.sep(0, hin, w, h, 0, 0, {R, 0, 0, 0}, eg::kR32), a2 = rec.sep(3, a1, w, h, 1, 0, {R, 0, 0, 0}, eg::kR32);
    const eg::H b1 = rec.sep(0, hin, w, h, 0, 0, {R + 3, 0, 0, 0}, eg::kR32), b2 = rec.sep(3, b1, w, h, 1, 0, {R + 3, 0, 0, 0}, eg::kR32);
    (void)b2;
    core.ensureReadback(cl->slot(), std::size_t(w) * h * 8, err);
    g.copyImageToBuffer(a2, core.slot(cl->slot()).readback, 0, 0, w, h);
    hal::FenceH f;
    const hal::Status s = eg::runGraph(dev, g, *cl, &f, err);
    CHECK(s == hal::Status::Ok, "%s", err.c_str());
    CHECK(dev.wait(f, 30ull * 1000 * 1000 * 1000) == hal::Status::Ok, "wait");
    *st = dev.stats(f);
    out->resize(std::size_t(w) * h);
    std::memcpy(out->data(), core.slot(cl->slot()).readback.mapped, out->size() * 4);
    *pooled = core.pool().pooledBytes();
    dev.waitIdle();
    dev.destroy(in);
  };
  std::vector<float> ref, split, o3;
  hal::SubmitStats st1, st2, st3;
  std::size_t pooled1 = 0, pooled2 = 0, pooled3 = 0;
  {
    auto core = makeCore({}, err);
    if (!core) return 77;
    record(*core, 20, &ref, &st1, &pooled1);
    record(*core, 20, &o3, &st3, &pooled3);
    std::printf("  4 box passes + readback: %d passes, %d barriers, %d submissions\n", st1.passes, st1.barriers, st1.parts);
    CHECK(st1.passes == 5, "passes %d", st1.passes);
    // initial + one between the two levels + the host-visibility barrier of the readback
    CHECK(st1.barriers <= 3, "barriers %d (layering should need initial + 1 per level boundary + host)", st1.barriers);
    CHECK(pooled1 == pooled3 && pooled1 > 0, "pool grew in steady state: %zu -> %zu", pooled1, pooled3);
    CHECK(ref == o3, "second run differs");
  }
  {
    setEnv("DAPPLE_GPU_SPLIT_GROUPS", "1");  // every pass becomes its own submission
    auto core = makeCore({}, err);
    unsetEnv("DAPPLE_GPU_SPLIT_GROUPS");
    if (!core) return 77;
    record(*core, 20, &split, &st2, &pooled2);
    std::printf("  with splitting: %d submissions for %d passes, results identical: %s\n", st2.parts, st2.passes, split == ref ? "yes" : "NO");
    CHECK(st2.parts >= 4, "expected the frame to be cut into several submissions, got %d", st2.parts);
    CHECK(split == ref, "split submission changed the result");
  }
  return 0;
}

// Selection policy of the hook (docs 5.9) with injected failures, including a real device rebuild. Runs last: it ends in
// the sticky-CPU state of the process.
int testSelect() {
  std::printf("== engine selection: unsupported / transient / device lost (rebuild once) / second loss (sticky CPU) ==\n");
  gpu::setRuntimeEnabled(1);
  if (!gpu::sharedBackend()) {
    std::printf("  no GPU backend\n");
    return 77;
  }
  const SourceImage src = gpu::makeTestImage(320, 240);
  const auto cams = CameraRegistry::withBuiltins();
  EditParams p;
  p.exposure = 0.3f;
  p.nrLuma = 20;
  Renderer r(p, cams.resolve("", ""), nullptr);
  const auto lut = r.bake(src, 33);
  FastOptions fo;
  fo.spatial = SpatialQuality::Full;
  gpu::SessionSlot slot;
  int w = 0, h = 0;
  auto call = [&] { return gpu::hookRenderFrame(slot, src, r, *lut, fo, 0, w, h); };
  auto json = [] { return gpu::statusJson(); };
  std::uint8_t* ok0 = call();
  CHECK(ok0 != nullptr, "first frame on the GPU");
  std::free(ok0);
  auto expectFail = [&](gpu::Session::Failure f, const char* what) {
    slot->injectFailure(f);
    std::uint8_t* o = call();
    CHECK(o == nullptr, "%s: the frame must go to the CPU", what);
    std::free(o);
    const std::string j = json();
    const auto k = j.find("\"fallbacks\"");
    std::printf("  %-12s -> CPU for this frame; %s\n", what, k == std::string::npos ? j.c_str() : j.c_str() + k);
  };
  expectFail(gpu::Session::Failure::Unsupported, "unsupported");
  std::uint8_t* o1 = call();
  CHECK(o1 != nullptr, "GPU still used after an unsupported plan");
  std::free(o1);
  expectFail(gpu::Session::Failure::Transient, "transient");
  std::uint8_t* o2 = call();
  CHECK(o2 != nullptr, "GPU still used after a transient error");
  std::free(o2);
  expectFail(gpu::Session::Failure::DeviceLost, "device lost");
  CHECK(json().find("\"rebuilds\":1") != std::string::npos, "device rebuilt once: %s", json().c_str());
  std::uint8_t* o3 = call();
  CHECK(o3 != nullptr, "GPU in use again after the rebuild (resident data re-uploaded)");
  std::free(o3);
  expectFail(gpu::Session::Failure::DeviceLost, "second loss");
  std::uint8_t* o4 = call();
  CHECK(o4 == nullptr, "after the second loss the process stays on the CPU");
  std::free(o4);
  CHECK(json().find("\"available\":false") != std::string::npos, "status: %s", json().c_str());
  std::printf("  final status: %s\n", json().c_str());
  return 0;
}

int benchBox() {
  std::printf("== box microbench: 6000x4000 plane, GPU ms per pass (median of 5) ==\n");
  std::string err;
  hal::DeviceOptions o;
  o.profile = true;
  auto core = makeCore(o, err);
  if (!core) return 77;
  const int w = 6000, h = 4000;
  std::vector<float> rgb(std::size_t(w) * h * 3);
  for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = float(((i * 2654435761u) >> 8) & 0xFFFF) / 65535.f;
  std::printf("  %4s  %-22s %-22s\n", "R", "direct H / V (ms)", "scan H / V (ms)");
  for (int R : {2, 4, 8, 12, 16, 24, 32, 48, 64}) {
    double dH[5], dV[5], sH[5], sV[5];
    for (int it = 0; it < 5; ++it) {
      const BoxRun d = gpuBox2D(*core, rgb, w, h, R, 0, false), s = gpuBox2D(*core, rgb, w, h, R, 1, false);
      dH[it] = d.gpuMsH, dV[it] = d.gpuMsV, sH[it] = s.gpuMsH, sV[it] = s.gpuMsV;
    }
    auto med = [](double* a) {
      std::sort(a, a + 5);
      return a[2];
    };
    std::printf("  %4d  %8.3f / %-8.3f     %8.3f / %-8.3f\n", R, med(dH), med(dV), med(sH), med(sV));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> want(argv + 1, argv + argc);
  auto on = [&](const char* n) { return want.empty() ? std::strcmp(n, "bench-box") != 0 : std::find(want.begin(), want.end(), n) != want.end(); };
  int rc = 0;
  auto run = [&](const char* n, int (*f)()) {
    if (!on(n)) return;
    const int r = f();
    if (r == 77) rc = rc ? rc : 77;
  };
  run("hal", testHal);
  run("box", testBox);
  run("sched", testSched);
  run("bench-box", benchBox);
  run("select", testSelect);
  if (g_fail) {
    std::printf("\nFAILED (%d failures)\n", g_fail);
    return 1;
  }
  std::printf("\nPASSED (0 failures)\n");
  return rc;
}
