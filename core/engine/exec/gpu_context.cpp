// gpu::Context (process-wide device selection Vulkan -> GLES 3.1, start-up self-test, device rebuild), the runtime flag and the
// GpuProvider EngineSession selects executors through (docs/engine/pipeline-architecture.md 5.9, 6.6).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/renderer.h"
#include "engine/exec/big_core.h"
#include "engine/exec/engine_session.h"
#include "engine/exec/gpu_executor.h"
#include "engine/nodes/film/film_gpu.h"
#include "engine/nodes/film/film_async.h"
#include "engine/hal/select.h"
#include "engine/nodes/gpu_common.h"
#include "parity.h"
#if defined(DAPPLE_HAL_GLES)
#include "engine/hal/gles/gles_device.h"
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace dapple::engine::gpu {

namespace {
std::string jsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    if (c == '\n') o += "\\n";
    else if (c != '\r') o.push_back(c);
  }
  return o;
}

// Start-up check of the running-sum box (boxscan.comp) against the direct sum on this device. A driver that miscompiles it
// (Adreno 620 did so for shared arrays sized from specialisation constants) must not take the whole GPU path down: the scan
// is switched off and the direct sum used. Both horizontal and vertical variants, a small and a large radius.
bool boxSelfTest(Core& core, std::string& report) {
  const int w = 211, h = 157;
  std::vector<float> rgb(std::size_t(w) * h * 3);
  unsigned s = 12345u;
  for (std::size_t i = 0; i < rgb.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    rgb[i] = 0.6f * (0.5f + 0.5f * std::sin(float(i % 997) * 0.01f)) + 0.4f * float((s >> 8) & 0xFFFF) / 65535.f;
  }
  hal::Device& dev = core.dev();
  hal::ImageH in;
  std::string err;
  if (dev.createImage({hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h), 1}, &in) != hal::Status::Ok || !core.uploadRgbF32(rgb.data(), std::size_t(w) * 3, w, h, in, 0, 0, err)) {
    report = "box: upload failed";
    return false;
  }
  auto run = [&](int R, int scanMin, std::vector<float>& out) -> bool {
    auto cl = dev.begin();
    if (!cl || !core.ensureReadback(cl->slot(), std::size_t(w) * h * 4, err)) return false;
    GpuGraph g(dev, core.pool());
    GpuRecorder rec(dev, g, err);
    rec.weights = core.slot(cl->slot()).weights;
    rec.boxScanMin = scanMin;
    rec.W = w;
    rec.H_ = h;
    const H hin = g.external(in);
    const H t = rec.sep(0, hin, w, h, 0, 0, {R, 0, 0, 0}, kR32);
    const H o = t < 0 ? -1 : rec.sep(3, t, w, h, 1, 0, {R, 0, 0, 0}, kR32);
    if (o < 0) return false;
    g.copyImageToBuffer(o, core.slot(cl->slot()).readback, 0, 0, std::uint32_t(w), std::uint32_t(h));
    hal::FenceH f;
    if (runGraph(dev, g, *cl, &f, err) != hal::Status::Ok || dev.wait(f, 30ull * 1000 * 1000 * 1000) != hal::Status::Ok) return false;
    out.resize(std::size_t(w) * h);
    std::memcpy(out.data(), core.slot(cl->slot()).readback.mapped, out.size() * 4);
    return true;
  };
  bool ok = true;
  double worst = 0;
  for (int R : {9, 40}) {
    std::vector<float> a, b;
    if (!run(R, 1, a) || !run(R, 0, b)) {
      ok = false;
      break;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      const double d = std::fabs(double(a[i]) - double(b[i]));
      worst = std::max(worst, d);
      if (!(d <= 3e-5)) ok = false;  // also catches NaN
    }
  }
  dev.waitIdle();
  dev.destroy(in);
  char line[120];
  std::snprintf(line, sizeof line, "box scan vs direct: max diff %.2g %s", worst, ok ? "ok" : "FAIL (running-sum box disabled)");
  report = line;
  return ok;
}

}  // namespace

// ============================================================================================ Context ===
struct Context::Impl {
  ContextOptions opt;
  std::unique_ptr<Core> core;
  std::vector<std::unique_ptr<Core>> retired;  // lost devices: kept alive so stale handles never meet a destroyed device
  hal::Api api = hal::Api::Null;
  std::string selfTest, selection;
  std::mutex mtx;
  std::atomic<std::uint64_t> epoch{1};
  int rebuilds = 0;
  bool owning = true;
  std::vector<hal::Attempt> attempts;
  std::mutex srcMtx;
  std::map<std::uint64_t, std::weak_ptr<void>> sources;
};

Context::Context() : d_(new Impl) {}
Context::~Context() = default;
hal::Api Context::api() const { return d_->api; }
const hal::DeviceCaps& Context::caps() const { return d_->core->dev().caps(); }
const std::string& Context::selfTestReport() const { return d_->selfTest; }
std::string Context::selectionJson() const { return d_->selection; }
Core& Context::core() { return *d_->core; }
std::size_t Context::pooledBytes() const { return d_->core->pool().pooledBytes(); }
int Context::rebuilds() const { return d_->rebuilds; }
bool Context::deviceLost() const { return d_->core->dev().state() == hal::DeviceState::Lost; }
std::uint64_t Context::epoch() const { return d_->epoch.load(); }
std::mutex& Context::mutex() { return d_->mtx; }
std::shared_ptr<void> Context::residentFor(std::uint64_t identity, const std::function<std::shared_ptr<void>()>& make) {
  std::lock_guard<std::mutex> lk(d_->srcMtx);
  for (auto it = d_->sources.begin(); it != d_->sources.end();) it = it->second.expired() ? d_->sources.erase(it) : std::next(it);
  auto it = d_->sources.find(identity);
  if (it != d_->sources.end())
    if (auto p = it->second.lock()) return p;
  std::shared_ptr<void> p = make();
  d_->sources[identity] = p;
  return p;
}
const ContextOptions& Context::options() const { return d_->opt; }
std::size_t Context::savePipelineCache() {
  const std::string& path = d_->opt.device.pipelineCachePath;
  if (path.empty() || !d_->core) return 0;
  const std::vector<std::uint8_t> blob = d_->core->dev().pipelineCacheBlob();
  if (blob.empty()) return 0;
  {
    std::ofstream f(path + ".tmp", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    if (!f) return 0;
  }
  std::error_code ec;
  std::filesystem::rename(path + ".tmp", path, ec);
  return ec ? 0 : blob.size();
}
bool Context::nativeHandles(hal::NativeHandles* out) const { return d_->core->dev().nativeHandles(out); }

std::string Context::deviceText() const {
  int lx = 16, ly = 16, rwg = 256;
  workgroupShape(caps(), lx, ly, rwg);  // the 2D kernels' workgroup (docs/engine/gles-shader-patches.md)
  char b[96];
  std::snprintf(b, sizeof b, "\n  api %s  workgroup %dx%d (reduce %d)", hal::apiName(d_->api), lx, ly, rwg);
  return caps().text() + b;
}

namespace {

// The whole-pipeline self-test: a known pattern through the engine (Planner + GpuExecutor) against the CPU reference. The
// thresholds are loose enough for fp16 working images and different float rounding, and tight enough to catch a broken driver
// (wrong results, garbage, zeros).
bool pipelineSelfTest(const std::shared_ptr<Context>& ctx, std::string& report) {
  const SourceImage src = dapple::gpu::makeTestImage(192, 128);
  const auto cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  struct Case {
    const char* name;
    void (*set)(EditParams&);
  };
  static const Case cases[] = {
      {"point+geometry", [](EditParams& p) { p.exposure = 0.4f; p.contrast = 20; p.saturation = 15; p.straighten = 3.f; p.vignette = -30; }},
      {"spatial", [](EditParams& p) { p.exposure = 0.2f; p.nrLuma = 40; p.nrColor = 40; p.dehaze = 30; p.clarity = 30; p.texture = 25; p.sharpen = 60; p.grain = 20; }},
  };
  std::unique_ptr<IGpuExecutor> ex = ctx->createExecutor();
  EngineSession es;
  es.useGpuExecutor(ex.get(), true);
  MemorySourceStore store(src);
  bool ok = true;
  char line[200];
  report.clear();
  for (const Case& c : cases) {
    EditParams p;
    c.set(p);
    Renderer r(p, cam, nullptr);
    FastOptions fo;
    fo.spatial = SpatialQuality::Full;
    // the reference is the CpuExecutor on the same plan (a session without a source store never uses the GPU)
    EngineSession cpuSession;
    FrameJob cpuJob{p, r, src, nullptr, cam, nullptr, {}, {}, {}};
    cpuJob.request.bakeSize = 33;
    cpuJob.fastOptions = fo;
    FrameResult cpuRes;
    const bool cpuOk = cpuSession.renderFrame(cpuJob, cpuRes);
    std::uint8_t* cpu = cpuOk ? cpuRes.rgba : nullptr;
    const int w = cpuRes.w, h = cpuRes.h;
    FrameJob job{p, r, src, nullptr, cam, nullptr, {}, {}, {}};
    job.request.bakeSize = 33;
    job.fastOptions = fo;
    job.store = &store;
    FrameResult res;
    const bool rendered = es.renderFrame(job, res);
    if (!cpu || !rendered || !res.rgba || res.w != w || res.h != h) {
      std::free(cpu);
      std::free(res.rgba);
      report += std::string(report.empty() ? "" : "; ") + c.name + ": " + (res.error.empty() ? "size mismatch" : res.error);
      return false;
    }
    const dapple::gpu::DeStats s = dapple::gpu::compareRgba8(res.rgba, cpu, std::size_t(w) * h);
    std::free(cpu);
    std::free(res.rgba);
    const bool pass = s.mean <= 1.0 && s.p99 <= 5.0 && s.max <= 25.0;
    std::snprintf(line, sizeof line, "%s%s: mean dE %.3f p99 %.2f max %.2f %s", report.empty() ? "" : "; ", c.name, s.mean, s.p99, s.max, pass ? "ok" : "FAIL");
    report += line;
    ok &= pass;
  }
  return ok;
}


// ---- start-up log + warm-up (docs/engine/pipeline-architecture.md section 26) --------------------------------------------------------
void probeLog(const std::string& text) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "dapple_engine", "%s", text.c_str());
#else
  const char* e = std::getenv("DAPPLE_ENGINE_LOG");
  if (e && e[0] == '1') std::fprintf(stderr, "dapple_engine: %s\n", text.c_str());
#endif
}

// Film pipelines and the default stock's tables, created before the first film frame asks for them (pipeline creation was 218 ms and the
// table / plan upload 110-120 ms of the first film frame on the Find N5). Waits (bounded) for the app to set the film pack path. Returns the
// milliseconds spent, -1 when there was nothing to warm.
void measureFilmV3(const std::shared_ptr<Context>& ctx, const char* stock, std::string& note);
double warmFilm(const std::shared_ptr<Context>& ctx, std::string& note, bool* wrong = nullptr) {
  using Clk = std::chrono::steady_clock;
  const auto t0 = Clk::now();
  std::shared_ptr<const film::FilmLibrary> lib;
  for (int i = 0; i < 100 && !(lib = film::defaultLibrary()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!lib) return note = "film check skipped: no film library", -1.0;
  const char* stock = lib->find("kodak_portra_400") ? "kodak_portra_400" : nullptr;
  if (!stock) return note = "default stock missing", -1.0;
  const SourceImage src = dapple::gpu::makeTestImage(640, 427);
  const auto cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  std::unique_ptr<IGpuExecutor> ex = ctx->createExecutor();
  EngineSession es;
  es.useGpuExecutor(ex.get(), true);
  MemorySourceStore store(src);
  struct Tier {
    SpatialQuality q;
    bool grain;
    float strength = 100.f;       // < 100: the Blend pipelines of a film strength drag
    const char* active = nullptr;  // a table-moving parameter: the coarse print table path
  };
  for (const Tier t : {Tier{SpatialQuality::Interactive, false}, Tier{SpatialQuality::Full, false}, Tier{SpatialQuality::Interactive, true},
                       Tier{SpatialQuality::Interactive, false, 60.f, "film_strength"}, Tier{SpatialQuality::Interactive, false, 100.f, "film_black"}}) {
    EditParams p;
    p.filmStock = stock;
    p.filmStrength = t.strength;
    if (t.active) p.filmBlack = 0.2f;
    Renderer r(p, cam, nullptr);
    FastOptions fo;
    fo.spatial = t.q;
    fo.keepGrain = t.grain;
    fo.fullLongEdge = t.grain ? 2000 : 0;
    FrameJob job{p, r, src, nullptr, cam, nullptr, {}, {}, {}};
    job.request.bakeSize = 33;
    job.request.interactive = t.q == SpatialQuality::Interactive;
    job.request.activeParam = t.grain ? "film_grain" : (t.active ? t.active : "");
    fo.coarseFilm = t.active && std::string(t.active) != "film_strength";
    job.fastOptions = fo;
    job.store = &store;
    FrameResult res;
    if (!es.renderFrame(job, res)) note += (note.empty() ? "" : "; ") + res.error;
    if (wrong && !t.grain && t.q == SpatialQuality::Interactive && !res.rgba)
      note += (note.empty() ? "" : "; ") + std::string("film check skipped: the GPU frame returned no host pixels");
    if (wrong && !t.grain && t.q == SpatialQuality::Interactive && res.rgba) {
      // wave 8: the film path is part of the verdict. Forced GLES gave wrong film colours (mean dE 3 vs the CPU) on the Adreno 620 / 640 while the
      // point and spatial self-test passed; the same frame on the CPU is the reference, a device that disagrees is demoted by the caller.
      EngineSession cpuSession;
      FrameJob cj{p, r, src, nullptr, cam, nullptr, {}, {}, {}};
      cj.request.bakeSize = 33;
      cj.request.interactive = true;
      cj.fastOptions = fo;
      FrameResult cr;
      if (cpuSession.renderFrame(cj, cr) && cr.rgba && cr.w == res.w && cr.h == res.h) {
        const dapple::gpu::DeStats s = dapple::gpu::compareRgba8(res.rgba, cr.rgba, std::size_t(res.w) * res.h);
        char b[120];
        std::snprintf(b, sizeof b, "film check: mean dE %.3f p99 %.2f max %.2f", s.mean, s.p99, s.max);
        note += (note.empty() ? "" : "; ") + std::string(b);
        if (!(s.mean <= 1.0 && s.p99 <= 5.0 && s.max <= 25.0)) *wrong = true;
      }
      std::free(cr.rgba);
    }
    std::free(res.rgba);
  }
  measureFilmV3(ctx, stock, note);
  return std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
}
struct SelfTestOut {
  std::string report;
  bool boxOk = true;
};

}  // namespace

std::shared_ptr<Context> Context::create(const ContextOptions& opt, std::string& err) {
  std::shared_ptr<Context> c(new Context);
  c->d_->opt = opt;
  SelfTestOut st;
  hal::SelfTestFn test;
  if (opt.selfTest) {
    test = [&st, &opt](hal::Device& dev, std::string& why) -> bool {
      const std::string devKey = dev.caps().key();
      if (opt.cacheLookup) {
        bool boxOk = true;
        std::string detail;
        const int c = opt.cacheLookup(devKey, &boxOk, &detail);
        if (c > 0) {  // this device + driver + OS build + app version passed before: skip the (expensive) self-test
          st.boxOk = boxOk;
          st.report = "cached pass (" + detail + ")";
          return true;
        }
        if (c < 0) return why = "cached failure (" + detail + ")", false;
      }
#if defined(DAPPLE_HAL_GLES)
      if (dev.caps().api == hal::Api::Gles31) {
        const hal::gles::SelfTestReport r = hal::gles::selfTest(dev);
        if (!r.ok) {
          if (opt.cacheStore) opt.cacheStore(devKey, false, true, r.text);
          return why = r.text, false;
        }
      }
#endif
      std::string e;
      std::shared_ptr<Context> view(new Context);
      view->d_->owning = false;
      view->d_->api = dev.caps().api;
      view->d_->core = Core::view(dev, e);
      if (!view->d_->core) return why = e, false;
      std::string boxReport;
      st.boxOk = boxSelfTest(*view->d_->core, boxReport);
      view->d_->core->boxScanOk = st.boxOk;
      std::string report;
      const bool ok = pipelineSelfTest(view, report);
      st.report = report + "; " + boxReport;
      if (!ok) why = report;
      view->d_->core.reset();
      if (opt.cacheStore) opt.cacheStore(devKey, ok, st.boxOk, ok ? st.report : report);
      return ok;
    };
  }
  hal::Selection sel = hal::selectDevice(opt.device, opt.api, test, test);
  c->d_->attempts = sel.attempts;
  c->d_->selection = sel.statusJson();
  if (!sel.device) {
    std::string why;
    for (const hal::Attempt& a : sel.attempts) why += (why.empty() ? "" : "; ") + std::string(hal::apiName(a.api)) + ": " + a.detail;
    err = sel.forced == "cpu" ? "GPU disabled (DAPPLE_GPU_API=cpu)" : why.empty() ? "no GPU backend" : why;
    return nullptr;
  }
  c->d_->api = sel.api;
  std::string e2;
  c->d_->core = Core::adopt(std::move(sel.device), e2);
  if (!c->d_->core) return err = e2, nullptr;
  c->d_->core->boxScanOk = st.boxOk;
  c->d_->selfTest = st.report;
  return c;
}

bool Context::rebuild(std::string& err) {
  std::unique_ptr<Core> fresh;
  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->rebuilds >= 1) return err = "device already rebuilt once in this process", false;
    ++d_->rebuilds;
    hal::Selection sel = hal::selectDevice(d_->opt.device, d_->api == hal::Api::Gles31 ? "gles" : d_->api == hal::Api::Metal ? "metal" : "vulkan");
    if (!sel.device) {
      for (const hal::Attempt& a : sel.attempts) err += (err.empty() ? "" : "; ") + a.detail;
      return false;
    }
    fresh = Core::adopt(std::move(sel.device), err);
    if (!fresh) return false;
    d_->retired.push_back(std::move(d_->core));
    d_->core = std::move(fresh);
    d_->epoch.fetch_add(1);
  }
  if (d_->opt.selfTest) {
    std::string boxReport, report;
    d_->core->boxScanOk = boxSelfTest(*d_->core, boxReport);
    std::shared_ptr<Context> self = shared_from_this();
    if (!pipelineSelfTest(self, report)) return err = "self-test failed after the device rebuild: " + report, false;
    d_->selfTest = report + "; " + boxReport;
  }
  return true;
}

// ============================================================================================ runtime flag + provider ===
namespace {
std::atomic<int> g_override{-1};
std::atomic<int> g_presentExt{-1};

// Off: no GPU. On: explicit (setRuntimeEnabled(1), DAPPLE_GPU=1, debug.dapple.gpu=1): the device is created and self-tested lazily on the first GPU
// frame, on that thread. Auto (Android default): the app starts a background probe at start-up (startWarmup); frames are CPU frames until it passed.
enum class Mode { Off, On, Auto };

Mode envMode() {
  if (const char* v = std::getenv("DAPPLE_GPU")) return v[0] == 'a' ? Mode::Auto : v[0] == '1' ? Mode::On : Mode::Off;
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.dapple.gpu", buf) > 0 && buf[0]) return buf[0] == '0' ? Mode::Off : buf[0] == '1' ? Mode::On : Mode::Auto;
  return Mode::Auto;  // the default: on after the self-test passed (docs/engine/pipeline-architecture.md 24)
#elif defined(__APPLE__)
  return Mode::Auto;  // Metal: probed in the background, CPU frames until it passed (the iOS app also sets DAPPLE_GPU=auto itself)
#else
  return Mode::Off;
#endif
}

Mode mode() {
  const int o = g_override.load();
  return o >= 0 ? (o != 0 ? Mode::On : Mode::Off) : envMode();
}

std::string envApi() {
  if (const char* v = std::getenv("DAPPLE_GPU_API")) return v;
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.dapple.gpu_api", buf) > 0) return buf;
#endif
  return {};
}

// ---- persisted self-test verdicts + the crash / hang marker ------------------------------------------------------------------------------------
// gpu_selftest.v1 (one record per line): header "v1<TAB>fingerprint<TAB>appVersion", then "api<TAB>deviceKey<TAB>pass|fail<TAB>boxOk<TAB>detail". The key of a verdict is
// Build.FINGERPRINT + app version (the header) + api + DeviceCaps::key() (vendor:device:driverVersion), so an OS update, a driver update or a new
// build re-runs the test. gpu_selftest.running is written before an API is created and tested and removed after: a process that died or hung
// inside leaves it, and the next start records that API as failed for this fingerprint + app version (deviceKey "*") without trying it again.
struct Config {
  std::mutex mu;
  std::string dir, app, fingerprint;
};
Config& config() {
  static Config* c = new Config;
  return *c;
}

// The dominant mixture pass both ways, once per device (wave 8): film_v3 with direct taps through the texture path or with the staged shared-memory
// column tile. The answer depends on the GPU (Adreno 710: direct 5.3 ms vs staged 18.6; Apple M1: staged 5.5 vs direct 10.4, docs/ios/metal-backend.md), so
// no rule on the platform, the vendor or the memory model: a final-tier film frame is rendered with each variant and the faster one is used for this
// device. The verdict is cached next to the probe verdicts, keyed by build fingerprint + app version + API + device key.
void measureFilmV3(const std::shared_ptr<Context>& ctx, const char* stock, std::string& note) {
  using Clk = std::chrono::steady_clock;
  if (const char* e = std::getenv("DAPPLE_FILM_V3_DIRECT")) {
    if (e[0] == '0' || e[0] == '1') return;  // forced
  }
  std::string dir, app, fp;
  {
    std::lock_guard<std::mutex> lk(config().mu);
    dir = config().dir;
    app = config().app;
    fp = config().fingerprint;
  }
  std::string path;
  if (!dir.empty()) {
    char nm[96];
    const std::string key = fp + "|" + app + "|" + hal::apiName(ctx->api()) + "|" + ctx->caps().key();
    std::snprintf(nm, sizeof nm, "film_v3_verdict_%016llx.txt", (unsigned long long)std::hash<std::string>{}(key));
    path = (std::filesystem::path(dir) / nm).string();
    if (FILE* f = std::fopen(path.c_str(), "r")) {
      int v = -1;
      float mpx = 0.f;
      const bool ok = std::fscanf(f, "%d %f", &v, &mpx) >= 1;
      std::fclose(f);
      if (ok && (v == 0 || v == 1) && mpx > 0.f) {
        engine::filmnode::setFilmV3Verdict(v);
        engine::filmnode::setFilmInteractiveMsPerMpx(mpx);
        char b[96];
        std::snprintf(b, sizeof b, " (cached verdict), interactive %.1f ms/Mpx (cached)", double(mpx));
        note += (note.empty() ? "" : "; ") + std::string("film_v3 ") + (v ? "direct" : "staged") + b;
        return;
      }
    }
  }
  const SourceImage src = dapple::gpu::makeTestImage(1024, 683);
  const auto cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  std::unique_ptr<IGpuExecutor> ex = ctx->createExecutor();
  EngineSession es;
  es.useGpuExecutor(ex.get(), true);
  MemorySourceStore store(src);
  auto timeVariant = [&](int direct) -> double {
    engine::filmnode::setFilmV3Override(direct);
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {  // rep 0 compiles the variant's pipelines
      EditParams p;
      p.filmStock = stock;
      Renderer r(p, cam, nullptr);
      FastOptions fo;
      fo.spatial = SpatialQuality::Full;
      FrameJob job{p, r, src, nullptr, cam, nullptr, {}, {}, {}};
      job.request.bakeSize = 33;
      job.fastOptions = fo;
      job.store = &store;
      FrameResult res;
      const auto t0 = Clk::now();
      const bool ok = es.renderFrame(job, res);
      const double ms = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
      std::free(res.rgba);
      if (!ok || res.executor == "cpu") return -1.0;
      if (rep > 0) best = std::min(best, ms);
    }
    return best;
  };
  const double tDirect = timeVariant(1), tStaged = timeVariant(0);
  engine::filmnode::setFilmV3Override(-1);
  if (tDirect < 0 || tStaged < 0) {
    note += (note.empty() ? "" : "; ") + std::string("film_v3 not measured (the GPU frame failed)");
    return;
  }
  const bool dflt = ctx->caps().unifiedMemory;  // the old rule decides a tie
  const int verdict = tStaged < tDirect * 0.95 ? 0 : (tDirect < tStaged * 0.95 ? 1 : (dflt ? 1 : 0));
  engine::filmnode::setFilmV3Verdict(verdict);
  // the interactive frame (what a drag renders) at two sizes with the chosen variant: milliseconds per megapixel, hot (the minimum of the later rounds). It seeds the starting
  // interactive size of the editor (AdaptiveQuality) instead of stepping down through slow frames.
  float msPerMpx = 0.f;
  {
    const SourceImage isrc = dapple::gpu::makeTestImage(480, 320);  // a drag-sized frame: the cost per megapixel is not constant (960x640 measured 58 ms/Mpx, 480x320 26-44 on the same phone)
    MemorySourceStore istore(isrc);
    double best = 1e30, prev1 = 0, prev2 = 0;
    for (int rep = 0; rep < 14; ++rep) {  // until the last three agree within 15 %: the GPU clock has ramped (a drag runs hot)
      EditParams p;
      p.filmStock = stock;
      Renderer r(p, cam, nullptr);
      FastOptions fo;
      fo.spatial = SpatialQuality::Interactive;
      FrameJob job{p, r, isrc, nullptr, cam, nullptr, {}, {}, {}};
      job.request.bakeSize = 33;
      job.request.interactive = true;
      job.fastOptions = fo;
      job.store = &istore;
      FrameResult res;
      const auto t0 = Clk::now();
      const bool ok = es.renderFrame(job, res);
      const double ms = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
      const double mpx = double(res.w) * double(res.h) / 1e6;
      std::free(res.rgba);
      if (!ok || res.executor == "cpu" || mpx <= 0) break;
      const double v = ms / mpx;
      if (rep >= 2) best = std::min(best, v);
      if (rep >= 5 && prev1 > 0 && prev2 > 0 && std::max({v, prev1, prev2}) < 1.15 * std::min({v, prev1, prev2})) break;
      prev2 = prev1;
      prev1 = v;
    }
    if (best < 1e29) msPerMpx = float(best);
  }
  if (msPerMpx > 0.f) engine::filmnode::setFilmInteractiveMsPerMpx(msPerMpx);
  char b[160];
  std::snprintf(b, sizeof b, "film_v3 direct %.1f ms vs staged %.1f ms -> %s, interactive %.1f ms/Mpx", tDirect, tStaged, verdict ? "direct" : "staged", double(msPerMpx));
  note += (note.empty() ? "" : "; ") + std::string(b);
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fprintf(f, "%d %.4f\n", verdict, double(msPerMpx));
      std::fclose(f);
    }
  }
}

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  if (s.size() > 300) s.resize(300);
  return s;
}

class SelfTestStore {
 public:
  SelfTestStore(std::string dir, std::string app, std::string fp) : dir_(std::move(dir)), app_(clean(std::move(app))), fp_(clean(std::move(fp))) {
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(dir_, ec);
    }
  }
  bool persistent() const { return !dir_.empty(); }

  // A marker left by a run that did not finish: that API failed for this fingerprint + app version.
  void recoverMarker() {
    if (!persistent()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::ifstream in(markerPath());
    if (!in) return;
    std::string line;
    std::getline(in, line);
    in.close();
    std::error_code ec;
    std::filesystem::remove(markerPath(), ec);
    const std::string want = fp_ + "\t" + app_ + "\t";
    if (line.compare(0, want.size(), want) != 0) return;  // another build / OS: not a verdict for this one
    const std::string api = line.substr(want.size());
    std::vector<Rec> recs = load();
    recs.erase(std::remove_if(recs.begin(), recs.end(), [&](const Rec& r) { return r.api == api && r.devKey == "*"; }), recs.end());
    recs.push_back({api, "*", false, true, "the self-test crashed or hung in an earlier run"});
    save(recs);
  }
  void arm(const std::string& api) {
    if (!persistent()) return;
    std::ofstream out(markerPath(), std::ios::trunc);
    out << fp_ << "\t" << app_ << "\t" << api << "\n";
  }
  void disarm() {
    if (!persistent()) return;
    std::error_code ec;
    std::filesystem::remove(markerPath(), ec);
  }
  bool crashed(const std::string& api) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Rec& r : load())
      if (r.api == api && r.devKey == "*" && !r.pass) return true;
    return false;
  }
  int lookup(const std::string& api, const std::string& devKey, bool* boxOk, std::string* detail) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Rec& r : load())
      if (r.api == api && r.devKey == devKey) {
        if (boxOk) *boxOk = r.boxOk;
        if (detail) *detail = r.detail;
        return r.pass ? 1 : -1;
      }
    return 0;
  }
  void store(const std::string& api, const std::string& devKey, bool ok, bool boxOk, const std::string& detail) {
    if (!persistent()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Rec> recs = load();
    recs.erase(std::remove_if(recs.begin(), recs.end(), [&](const Rec& r) { return r.api == api && r.devKey == devKey; }), recs.end());
    recs.push_back({api, devKey, ok, boxOk, clean(detail)});
    if (recs.size() > 16) recs.erase(recs.begin(), recs.end() - 16);
    save(recs);
  }

 private:
  struct Rec {
    std::string api, devKey;
    bool pass = false, boxOk = true;
    std::string detail;
  };
  std::string path() const { return dir_ + "/gpu_selftest.v1"; }
  std::string markerPath() const { return dir_ + "/gpu_selftest.running"; }
  std::vector<Rec> load() const {
    std::vector<Rec> v;
    if (!persistent()) return v;
    std::ifstream in(path());
    std::string line;
    if (!std::getline(in, line) || line != "v1\t" + fp_ + "\t" + app_) return v;  // other OS build / app version: nothing is known
    while (std::getline(in, line)) {
      std::vector<std::string> f;
      std::size_t p = 0;
      for (int i = 0; i < 4; ++i) {
        const std::size_t q = line.find('\t', p);
        if (q == std::string::npos) break;
        f.push_back(line.substr(p, q - p));
        p = q + 1;
      }
      if (f.size() != 4) continue;
      v.push_back({f[0], f[1], f[2] == "pass", f[3] != "0", line.substr(p)});
    }
    return v;
  }
  void save(const std::vector<Rec>& recs) const {
    const std::string tmp = path() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << "v1\t" << fp_ << "\t" << app_ << "\n";
      for (const Rec& r : recs) out << r.api << "\t" << r.devKey << "\t" << (r.pass ? "pass" : "fail") << "\t" << (r.boxOk ? "1" : "0") << "\t" << r.detail << "\n";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path(), ec);
  }
  std::string dir_, app_, fp_;
  std::mutex mu_;
};

std::string systemFingerprint() {
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("ro.build.fingerprint", buf) > 0) return buf;  // what android.os.Build.FINGERPRINT reads
#endif
  return "unknown";
}

class ProviderImpl final : public GpuProvider {
 public:
  enum class State { Idle, Probing, Ready, Failed };

  bool enabled() const override {
    const Mode m = mode();
    if (m == Mode::Off || broken_.load()) return false;
    return m == Mode::On || state_.load() == State::Ready;
  }
  std::uint64_t generation() override {
    std::lock_guard<std::mutex> lk(mtx_);
    return gen_;
  }
  std::unique_ptr<IGpuExecutor> createExecutor(std::string& why) override {
    std::shared_ptr<Context> c = context(&why);
    return c ? c->createExecutor() : nullptr;
  }
  bool recover(std::string& why) override {
    std::shared_ptr<Context> c;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      c = ctx_;
    }
    if (!c) return false;
    if (c->rebuild(why)) {
      std::lock_guard<std::mutex> lk(mtx_);
      ++gen_;
      return true;
    }
    // this API is given up: the next one (Vulkan -> GLES -> CPU) takes over
    std::lock_guard<std::mutex> lk(mtx_);
    excluded_.insert(c->api());
    ctx_.reset();
    ++gen_;
    state_.store(State::Idle);
    std::string w2;
    std::shared_ptr<Context> n = make(&w2, excluded_, nullptr);
    if (!n) {
      broken_.store(true);
      state_.store(State::Failed);
      why += (why.empty() ? "" : "; ") + w2;
      return false;
    }
    ctx_ = n;
    state_.store(State::Ready);
    return false;  // the failed API did not recover; the caller counts a demotion and goes on with a new executor
  }
  std::string inactiveReason() override {
    const Mode m = mode();
    if (m == Mode::Off) return "GPU disabled (developer switch)";
    std::lock_guard<std::mutex> lk(mtx_);
    if (broken_.load()) return "GPU given up after device loss" + (lastWhy_.empty() ? std::string() : ": " + lastWhy_);
    switch (state_.load()) {
      case State::Idle: return "GPU self-test not started";
      case State::Probing: return "GPU self-test running";
      case State::Failed: return "GPU unavailable: " + (lastWhy_.empty() ? std::string("no usable backend") : lastWhy_);
      case State::Ready: break;
    }
    return "GPU ready";
  }
  std::string statusJson() override {
    const Mode m = mode();
    const bool en = enabled();
    std::string why;
    std::shared_ptr<Context> c = m != Mode::Off && !broken_.load() ? context(&why) : nullptr;
    const GpuCounters cnt = gpuCounters();
    const char* st[] = {"idle", "probing", "ready", "failed"};
    std::string tried;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      tried = tried_;
    }
    std::string j = std::string("{\"mode\":\"") + (m == Mode::Off ? "off" : m == Mode::On ? "on" : "auto") + "\",\"state\":\"" + st[int(state_.load())] +
                    "\",\"enabled\":" + (en ? "true" : "false") + ",\"available\":" + (c ? "true" : "false");
    if (c)
      j += ",\"api\":\"" + std::string(hal::apiName(c->api())) + "\",\"device\":\"" + jsonEsc(c->caps().name) + "\",\"deviceKey\":\"" +
           c->caps().key() + "\",\"selfTest\":\"" + jsonEsc(c->selfTestReport()) + "\",\"selection\":" + c->selectionJson() +
           ",\"filmMsPerMpx\":" + std::to_string(double(engine::filmnode::filmInteractiveMsPerMpx()));
    else if (m != Mode::Off) j += ",\"reason\":\"" + jsonEsc(inactiveReason()) + "\"";
    if (!tried.empty()) j += ",\"skipped\":\"" + jsonEsc(tried) + "\"";
    j += ",\"frames\":" + std::to_string(cnt.frames) + ",\"fallbacks\":" + std::to_string(cnt.fallbacks) + ",\"transients\":" + std::to_string(cnt.transients) +
         ",\"deviceLost\":" + std::to_string(cnt.lost) + ",\"demotions\":" + std::to_string(cnt.demotions) + ",\"rebuilds\":" + std::to_string(c ? c->rebuilds() : 0) +
         ",\"executors\":" + executorStatsJson() + ",\"keepWarm\":" + keepWarmJson();
    return j + "}";
  }
  std::size_t pooledBytes() override {
    std::shared_ptr<Context> c = context(nullptr);
    return c ? c->pooledBytes() : 0;
  }
  std::size_t trimPool(std::size_t keepFree) override {
    std::shared_ptr<Context> c = peek();
    if (!c || c->deviceLost()) return 0;
    std::lock_guard<std::mutex> lk(c->mutex());  // the executors hold it for a whole frame: the pool is not in use while we hold it
    const std::size_t before = c->pooledBytes();
    c->core().pool().trim(keepFree);
    c->core().dev().trim();
    return before - std::min(before, c->pooledBytes());
  }
  bool memoryInfo(MemoryInfo* out) override {
    std::shared_ptr<Context> c = peek();
    if (!c || !out) return false;
    const hal::Device& d = c->core().dev();
    out->unified = d.caps().unifiedMemory;
    out->deviceLocal = d.caps().deviceLocalBytes;
    out->poolBudget = d.poolBudget();
    out->pooled = c->pooledBytes();
    out->budget = out->usage = 0;
    std::uint64_t b = 0, u = 0;
    if (d.memoryBudget(&b, &u)) out->budget = b, out->usage = u;
    return true;
  }
  // the active context without creating one (a memory query must not start a device)
  std::shared_ptr<Context> peek() {
    std::lock_guard<std::mutex> lk(mtx_);
    return broken_.load() ? nullptr : ctx_;
  }
  bool nativeHandles(hal::NativeHandles* out) override {
    std::shared_ptr<Context> c = context(nullptr);
    return c && c->nativeHandles(out);
  }
  bool lease(DeviceLease* out) override {
    std::shared_ptr<Context> c = context(nullptr);
    hal::NativeHandles h;
    if (!out || !c || !c->nativeHandles(&h) || !h.swapchainEnabled) return false;
    out->handles = h;
    out->keepAlive = c;
    out->queueMutex = &c->mutex();
    out->generation = c->epoch();
    return true;
  }

  // Auto mode: start the device creation + self-test on a background thread (once). Frames render on the CPU until it is done; nothing waits for it.
  void startWarmup() {
    if (mode() != Mode::Auto) return;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (state_.load() != State::Idle || broken_.load()) return;
      state_.store(State::Probing);
    }
    std::thread([this] { probe(); }).detach();
  }

  std::shared_ptr<Context> context(std::string* why) {
    const Mode m = mode();
    if (m == Mode::Off) {
      if (why) *why = "GPU backend disabled";
      return nullptr;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (broken_.load()) {
      if (why) *why = lastWhy_.empty() ? "GPU unavailable for the rest of the process" : lastWhy_;
      return nullptr;
    }
    if (m == Mode::On && state_.load() == State::Idle) {  // explicit switch: create + self-test here, on the calling thread
      state_.store(State::Probing);
      ctx_ = make(&lastWhy_, excluded_, &tried_);
      ++gen_;
      state_.store(ctx_ ? State::Ready : State::Failed);
    }
    if (!ctx_ && why) *why = state_.load() == State::Probing ? "GPU self-test running" : lastWhy_.empty() ? "GPU self-test not started" : lastWhy_;
    return ctx_;
  }

 private:
  void probe() {
    const auto tStart = std::chrono::steady_clock::now();
    std::string why, tried;
    std::shared_ptr<Context> c;
    try {
#if defined(__ANDROID__)
      if (!std::getenv("DAPPLE_GPU_PROBE_NICE_OFF")) setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);  // never compete with the first paint
#endif
      std::set<hal::Api> ex;
      {
        std::lock_guard<std::mutex> lk(mtx_);
        ex = excluded_;
      }
      c = make(&why, ex, &tried);
    } catch (...) {
      why = "exception during the GPU self-test";
    }
    std::unique_lock<std::mutex> lock(mtx_);
    ctx_ = c;
    lastWhy_ = why;
    tried_ = tried;
    ++gen_;
    state_.store(c ? State::Ready : State::Failed);
    lock.unlock();
    const double probeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
    if (!c) {
      probeLog("gpu probe: no backend (" + why + ") after " + std::to_string(int(probeMs)) + " ms, frames stay on the CPU");
      return;
    }
    const std::string st = c->selfTestReport();
    const bool cached = st.rfind("cached pass", 0) == 0;
    std::string line = std::string("gpu probe: api=") + hal::apiName(c->api()) + " device=\"" + c->caps().name + "\" driver=" + std::to_string(c->caps().driverVersion) +
                       " self-test " + (cached ? "cached" : "fresh") + ", probe total " + std::to_string(int(probeMs)) + " ms" + (tried.empty() ? "" : " (skipped: " + tried + ")");
    probeLog(line);
    if (!std::getenv("DAPPLE_GPU_WARM_OFF")) {
      std::string note;
      bool filmWrong = false;
      const double wm = warmFilm(c, note, &filmWrong);
      const std::size_t pc = c->savePipelineCache();
      if (filmWrong) {  // wave 8: the film frame disagrees with the CPU: give this API up, the next one (Vulkan -> GLES -> CPU) takes over
        probeLog(std::string("gpu probe: ") + hal::apiName(c->api()) + " renders film frames wrong (" + note + "): demoted");
        std::lock_guard<std::mutex> lk(mtx_);
        excluded_.insert(c->api());
        ctx_.reset();
        ++gen_;
        std::string w2;
        std::shared_ptr<Context> n = make(&w2, excluded_, nullptr);
        ctx_ = n;
        lastWhy_ = n ? std::string() : "film check failed: " + w2;
        state_.store(n ? State::Ready : State::Failed);
        if (!n) return;
        ++gen_;
      }
      probeLog("gpu warm-up: film pipelines + default stock " + (wm < 0 ? "skipped (" + note + ")" : std::to_string(int(wm)) + " ms" + (note.empty() ? "" : " (" + note + ")")) + ", pipeline cache " + (pc ? std::to_string(pc / 1024) + " KB saved" : "not saved") + (c->options().device.pipelineCachePath.empty() ? "" : " [" + c->options().device.pipelineCachePath + "]"));
    }
  }

  // Tries the candidates in order (Vulkan -> GLES 3.1, or the one API forced by DAPPLE_GPU_API / debug.dapple.gpu_api), each wrapped in the
  // in-progress marker and consulting the persisted verdicts. `tried` = the candidates that failed before the one that was taken.
  std::shared_ptr<Context> make(std::string* why, const std::set<hal::Api>& excluded, std::string* tried) {
    ContextOptions o;
    if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) o.device.allowSoftware = v[0] == '1';
    if (std::getenv("DAPPLE_GPU_PROFILE")) o.device.profile = true;  // per-pass timestamps (the engine log prints them with DAPPLE_GPU_PASSLOG=1)
    if (const char* v = std::getenv("DAPPLE_GPU_PIPELINE_CACHE")) o.device.pipelineCachePath = v;  // persisted VkPipelineCache
    const int pe = g_presentExt.load();
    o.device.presentExtensions = pe >= 0 ? pe != 0 : (std::getenv("DAPPLE_GPU_PRESENT") && std::getenv("DAPPLE_GPU_PRESENT")[0] == '1');
#if defined(__ANDROID__)
    // The zero-readback display adopts this device, so the surface / swapchain extensions are on by default on Android (offered,
    // never required). DAPPLE_GPU_PRESENT=0 turns them off.
    if (pe < 0 && !std::getenv("DAPPLE_GPU_PRESENT")) o.device.presentExtensions = true;
#endif
    std::string api = envApi();
    for (auto& ch : api) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (api == "vk") api = "vulkan";
    if (api == "auto") api.clear();
    const bool forced = !api.empty();

    std::string dir, app, fp;
    {
      std::lock_guard<std::mutex> lk(config().mu);
      dir = config().dir;
      app = config().app;
      fp = config().fingerprint.empty() ? systemFingerprint() : config().fingerprint;
    }
    auto store = std::make_shared<SelfTestStore>(dir, app, fp);
    store->recoverMarker();

    const bool userCache = !o.device.pipelineCachePath.empty();
    std::vector<std::string> cands;
    if (forced) cands = {api};
#if defined(__APPLE__)
    else cands = {"metal", "gles"};  // Apple: Api::Metal (the Vulkan factory symbol there is only an alias of the Metal device)
#else
    else cands = {"vulkan", "gles"};
#endif
    std::string reasons;
    for (const std::string& cand : cands) {
      const hal::Api a = cand == "vulkan" ? hal::Api::Vulkan : cand == "gles" ? hal::Api::Gles31 : cand == "metal" ? hal::Api::Metal : hal::Api::Null;
      if (excluded.count(a)) continue;
      if (!forced && store->crashed(cand)) {  // earlier run died or hung in this API's self-test: not again for this OS build + app version
        reasons += (reasons.empty() ? "" : "; ") + cand + ": the self-test crashed or hung in an earlier run (cached)";
        continue;
      }
      o.api = cand;
      if (!userCache && !dir.empty()) {  // persisted pipeline cache, keyed by build fingerprint + app version + API (the driver also checks its own UUID header)
        char nm[64];
        std::snprintf(nm, sizeof nm, "pipeline_cache_%016llx.bin", (unsigned long long)std::hash<std::string>{}(fp + "|" + app + "|" + cand));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        o.device.pipelineCachePath = (std::filesystem::path(dir) / nm).string();
      }
      o.cacheLookup = [store, cand](const std::string& k, bool* boxOk, std::string* d) { return store->lookup(cand, k, boxOk, d); };
      o.cacheStore = [store, cand](const std::string& k, bool ok, bool boxOk, const std::string& d) { store->store(cand, k, ok, boxOk, d); };
      store->arm(cand);
      std::string err;
      std::shared_ptr<Context> c = Context::create(o, err);
      store->disarm();
      if (c) {
        if (tried) *tried = reasons;
        return c;
      }
      reasons += (reasons.empty() ? "" : "; ") + err;
    }
    if (tried) *tried = reasons;
    if (why) *why = reasons.empty() ? "no backend left" : reasons;
    return nullptr;
  }

  std::mutex mtx_;
  std::shared_ptr<Context> ctx_;
  std::set<hal::Api> excluded_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> broken_{false};
  std::uint64_t gen_ = 1;
  std::string lastWhy_, tried_;
};

ProviderImpl& provider() {
  static auto* p = new ProviderImpl;  // leaked on purpose: tearing the device down from a static destructor runs after the
                                      // Vulkan loader / driver have been unloaded (crashes on Android)
  return *p;
}
}  // namespace

bool runtimeEnabled() { return mode() != Mode::Off; }
void setRuntimeEnabled(int enabled) { g_override.store(enabled); }
void setPresentExtensions(int on) { g_presentExt.store(on); }
void cacheConfig(std::string* dir, std::string* appVersion, std::string* fingerprint) {
  std::lock_guard<std::mutex> lk(config().mu);
  if (dir) *dir = config().dir;
  if (appVersion) *appVersion = config().app;
  if (fingerprint) *fingerprint = config().fingerprint.empty() ? systemFingerprint() : config().fingerprint;
}

void configure(const std::string& cacheDir, const std::string& appVersion, const std::string& fingerprint) {
  std::lock_guard<std::mutex> lk(config().mu);
  config().dir = cacheDir;
  config().app = appVersion;
  config().fingerprint = fingerprint;
  if (!cacheDir.empty()) engine::filmnode::setFilmTableDisk((std::filesystem::path(cacheDir) / "film_tables").string(), appVersion + "|" + fingerprint);
}
void installProvider() { installGpuProvider(&provider()); }
void startWarmup() {
  installProvider();
  provider().startWarmup();
}
std::shared_ptr<Context> sharedContext(std::string* why) { return provider().context(why); }
std::string statusJson() { return provider().statusJson(); }

// ---- keep-warm pump (gpu_runtime.h keepWarm) ----------------------------------------------------------------------------------------------------------
// Measured on a mid-range phone (docs/engine/device-results/redmi-garnet.md, round 10): after a pause the first GPU submission pays a wake-up of ~8 ms
// whatever its size; the GPU and memory clocks then need ~45-60 ms of saturating load to step up (sooner when the big CPU cores are busy as well: the
// memory clock follows the CPU clock too), and they fall back within ~100-200 ms of idle. A drag on its own (60-70 % GPU busy) holds a raised clock but
// never raises a low one. What a real frame waits for is the work queued ahead of it.
// So: the load is GPU-only units of bounded cost (a buffer copy over a ring larger than the caches, sized by measurement to ~unitMs), at most `depth`
// on the queue and none submitted while a real frame is between request and completion (it waits for <= the units already on the GPU); while a frame
// runs, up to three units sized for the low clock are queued right BEHIND it (piggyback: costs that frame nothing, keeps the GPU busy while the host
// reads back and prepares the next; the next frame waits for at most their remainder); one spinning thread on the big cores for the whole window. Window: from touch-down, at least `burst_ms`; until the first real frame or `holdMs`
// (a finger that has not moved yet); after the first real frame `dragMs` more (the drag holds the clock from there); never more than `maxMs`.
// Every device call holds the context mutex (the HAL device is not thread-safe; a frame holds it for its whole run), and only for the call itself.
namespace {
struct KeepWarmPump {
  using Clk = std::chrono::steady_clock;
  std::mutex m;
  std::condition_variable cv;
  bool started = false;
  Clk::time_point deadline{};  // touch-down + burst_ms of the newest request
  Clk::time_point touchAt{};
  bool prepare = false;
  std::atomic<int> realActive{0};
  std::atomic<std::uint64_t> realSeq{0};
  std::atomic<bool> live{false};   // a window is open: piggyback units are wanted
  std::atomic<int> piggyUnits{0};  // units queued behind real frames in this window
  // device state: touched only with the context mutex held
  std::weak_ptr<Context> ctxOf;  // the context the buffers belong to
  Context* ctxRaw = nullptr;
  hal::BufferH ring, sink;
  std::uint64_t ringBytes = 0, sinkBytes = 0, chunk = 256ull << 10, off = 0;
  // unit throughput in the low-clock state (median of the units of a calibration at idle, refreshed by the first ~40 ms of every window): sizes the
  // first units of a window and every piggyback unit, so a unit costs ~unitMs at the slowest clock and less once the clock is up. 0 = not measured yet
  double lowMBps = 0;
  std::uint64_t lowBytes(double unitMs, std::uint64_t cap) const {
    if (lowMBps <= 0) return std::min<std::uint64_t>(chunk, cap);  // not measured yet (first window of the process): the adapted size
    return std::clamp<std::uint64_t>(std::uint64_t(lowMBps * unitMs / 1000.0 * double(1ull << 20)) & ~std::uint64_t(0xFFFF), 64ull << 10, cap);
  }
  double pigUnitMs = 1.5;
  int piggyDepth = 1;
  // last window (diagnostics)
  std::mutex jm;
  std::string json = "{}";

  static double envD(const char* k, double d) {
    const char* e = std::getenv(k);
    return e && *e ? std::atof(e) : d;
  }
  void request(int ms) {
    std::lock_guard<std::mutex> lk(m);
    const Clk::time_point now = Clk::now();
    if (ms < 0) {
      prepare = true;
    } else {
      if (now >= deadline) touchAt = now;  // a new touch (not an extension of a running window)
      deadline = std::max(deadline, now + std::chrono::milliseconds(ms));
    }
    if (!started) {
      started = true;
      std::thread([this] { loop(); }).detach();
    }
    cv.notify_all();
  }
  void loop() {
    detail::pinCurrentThreadToBigCores();  // the pump polls fences: a prompt wake-up keeps the gaps between units short
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      // The working set (12 MB) is kept for the life of the context: allocating it costs ~25-50 ms on a phone, with the context mutex held, which a
      // touch must never make a real frame wait for. It is allocated by the prepare call (at idle, once per photo), else by the first touch.
      cv.wait(lk, [&] { return prepare || Clk::now() < deadline; });
      const bool prep = prepare;
      prepare = false;
      lk.unlock();
      run(prep);
      lk.lock();
    }
  }
  void release() {
    std::shared_ptr<Context> c = ctxOf.lock();
    if (!c) return;
    std::lock_guard<std::mutex> dl(c->mutex());
    hal::Device& dev = c->core().dev();
    dev.waitAll();
    if (ring) dev.destroy(ring);
    if (sink) dev.destroy(sink);
    ring = sink = hal::BufferH{};
    ringBytes = sinkBytes = 0;
    ctxOf.reset();
    ctxRaw = nullptr;
  }
  bool ensure(Context& c, std::uint64_t wantRing, std::uint64_t wantSink) {  // context mutex held
    hal::Device& dev = c.core().dev();
    if (ring && ringBytes >= wantRing && sink && sinkBytes >= wantSink) return true;
    if (ring) dev.destroy(ring);
    if (sink) dev.destroy(sink);
    ring = sink = hal::BufferH{};
    hal::BufferDesc d;
    d.kind = hal::MemKind::DeviceLocal;
    d.storage = true;
    d.bytes = wantRing;
    if (dev.createBuffer(d, &ring) != hal::Status::Ok) return ring = hal::BufferH{}, false;
    d.bytes = wantSink;
    if (dev.createBuffer(d, &sink) != hal::Status::Ok) {
      dev.destroy(ring);
      ring = sink = hal::BufferH{};
      return false;
    }
    ringBytes = wantRing, sinkBytes = wantSink, off = 0;
    return true;
  }
  // one unit: copy `chunk` bytes of the ring (rolling offset) into the sink; context mutex held
  bool submitUnit(hal::Device& dev, hal::FenceH* f, std::uint64_t* bytes) {
    std::unique_ptr<hal::CommandList> cl = dev.begin();
    if (!cl) return false;
    const std::uint64_t n = std::min(chunk, sinkBytes);
    if (off + n > ringBytes) off = 0;
    cl->copyBufferToBuffer(ring, sink, n, off);
    if (dev.submit(*cl, f) != hal::Status::Ok) return false;
    off += n;
    if (bytes) *bytes = n;
    return true;
  }
  // Executor::run, right after a frame's submission (context mutex held by the caller)
  void piggyback(Context& c) {
    if (!live.load(std::memory_order_acquire) || ctxRaw != &c || !ring) return;
    hal::Device& dev = c.core().dev();
    const std::uint64_t keep = chunk;
    chunk = lowBytes(pigUnitMs, sinkBytes);  // sized for the low clock: <= ~unitMs whatever state the clock is in
    for (int i = 0; i < piggyDepth; ++i) {
      if (!submitUnit(dev, nullptr, nullptr)) return void(chunk = keep);
      piggyUnits.fetch_add(1, std::memory_order_relaxed);
    }
    chunk = keep;
  }
  void run(bool prep) {
    if (!provider().enabled()) return;
    std::shared_ptr<Context> c = provider().context(nullptr);
    if (!c) return;
    if (ctxOf.lock() != c) {
      release();
      std::lock_guard<std::mutex> dl(c->mutex());
      ctxOf = c;
      ctxRaw = c.get();
    }
    const std::uint64_t mb = 1ull << 20;
    const std::uint64_t wantRing = std::uint64_t(std::max(4.0, envD("DAPPLE_HINT_RING_MB", 8))) * mb;
    const std::uint64_t wantSink = std::min<std::uint64_t>(wantRing, std::uint64_t(std::max(1.0, envD("DAPPLE_HINT_SINK_MB", 4))) * mb);
    const double unitMs = std::max(0.3, envD("DAPPLE_HINT_UNIT_MS", 1.5));
    const int depthIdle = std::max(1, int(envD("DAPPLE_HINT_DEPTH", 2)));  // units on the queue while no real frame is pending
    piggyDepth = std::max(0, int(envD("DAPPLE_HINT_PIGGY", 3)));          // units queued behind each real frame
    const double holdMs = envD("DAPPLE_HINT_HOLD_MS", 250), dragMs = envD("DAPPLE_HINT_DRAG_MS", 300), maxMs = envD("DAPPLE_HINT_MAX_MS", 400);
    const double holdDuty = std::clamp(envD("DAPPLE_HINT_HOLD_DUTY", 1.0), 0.1, 1.0);  // GPU duty after burst_ms while the finger has not moved yet
    const int logLevel = int(envD("DAPPLE_HINT_LOG", 0));
    const bool cpu = envD("DAPPLE_HINT_CPU", 1) != 0;
    const bool cpuYield = envD("DAPPLE_HINT_CPU_YIELD", 0) != 0;
    Clk::time_point tDown, base;
    {
      std::lock_guard<std::mutex> lk(m);
      tDown = touchAt;
      base = deadline;
    }
    const Clk::time_point t0 = Clk::now();
    if (prep) tDown = base = t0;
    const std::uint64_t seq0 = realSeq.load();
    Clk::time_point firstReal{};
    bool haveReal = false;
    auto windowEnd = [&]() {
      Clk::time_point b;
      {
        std::lock_guard<std::mutex> lk(m);
        b = deadline;
      }
      auto us = [](double v) { return std::chrono::microseconds(std::int64_t(v * 1000.0)); };
      const Clk::time_point e = std::max(b, haveReal ? firstReal + us(dragMs) : tDown + us(holdMs));
      return std::min(e, std::max(b, tDown + us(maxMs)));
    };
    std::atomic<bool> spinOn{cpu && !prep};
    std::atomic<bool> spinPause{false};
    std::thread spinner;
    if (spinOn.load())
      spinner = std::thread([&] {  // big-core load (the memory clock follows the CPU clock; DAPPLE_HINT_CPU_YIELD=1: not while a real frame runs)
        detail::pinCurrentThreadToBigCores();
        volatile double x = 1.0;
        while (spinOn.load(std::memory_order_relaxed)) {
          if ((cpuYield && realActive.load(std::memory_order_relaxed) > 0) || spinPause.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
          }
          for (int i = 0; i < 2000; ++i) x = x * 1.0000001 + 1e-9;
        }
      });
    struct Fly {
      hal::FenceH f;
      Clk::time_point t;
      std::uint64_t bytes;
      std::uint64_t seq;
      int piggy;
    };
    std::deque<Fly> q;
    std::vector<double> early;  // unit throughputs of the first ~40 ms (low clock)
    Clk::time_point lastDone = t0, restUntil = t0;
    int units = 0, measured = 0;
    double busyMs = 0, firstMBps = 0, lastMBps = 0, allocMs = 0, maxUnitMs = 0, lastUnitMs = unitMs;
    std::uint64_t bytesTotal = 0;
    piggyUnits.store(0);
    bool failed = false;
    {
      std::lock_guard<std::mutex> dl(c->mutex());
      // every window starts with units sized for the slowest state seen so far (the clock is low after a pause), and adapts from there
      pigUnitMs = unitMs;
      chunk = lowMBps > 0 ? lowBytes(unitMs, wantSink) : (256ull << 10);
      const Clk::time_point a0 = Clk::now();
      failed = c->deviceLost() || !ensure(*c, wantRing, wantSink);
      allocMs = std::chrono::duration<double, std::milli>(Clk::now() - a0).count();
    }
    if (!failed && !prep) live.store(true, std::memory_order_release);
    auto retire = [&](hal::Device& dev) {
      while (!q.empty()) {
        if (!dev.poll(q.front().f)) break;
        const Clk::time_point now = Clk::now();
        const Clk::time_point from = std::max(lastDone, q.front().t);
        const double ms = std::chrono::duration<double, std::milli>(now - from).count();
        lastDone = now;
        // the interval between two completions on a queue that only held units is the unit's GPU time (+ the poll granularity): adapt the chunk
        // to `unitMs`. Intervals with a real frame or a piggyback unit in between are not measured.
        if (ms > 0.05 && q.front().seq == realSeq.load() && q.front().piggy == piggyUnits.load()) {
          const double mbps = double(q.front().bytes) / double(mb) / (ms / 1000.0);
          if (logLevel >= 2) std::fprintf(stderr, "keepwarm unit t %.1f ms: %.2f ms %.0f MB/s\n", std::chrono::duration<double, std::milli>(now - t0).count(), ms, mbps);
          if (measured == 0) firstMBps = mbps;
          lastMBps = mbps;
          if (measured > 0 && !prep && std::chrono::duration<double, std::milli>(now - t0).count() < 40.0) early.push_back(mbps);  // the first unit carries the wake-up
          maxUnitMs = std::max(maxUnitMs, ms);
          lastUnitMs = ms;
          ++measured;
          busyMs += ms;
          const double scale = std::clamp(unitMs / ms, 0.5, 2.0);
          chunk = std::clamp<std::uint64_t>(std::uint64_t(double(chunk) * scale) & ~std::uint64_t(0xFFFF), 256ull << 10, sinkBytes);
        }
        q.pop_front();
        if (holdDuty < 1.0) restUntil = now + std::chrono::microseconds(std::int64_t(lastUnitMs * (1.0 / holdDuty - 1.0) * 1000.0));
      }
    };
    while (!failed) {
      const Clk::time_point now = Clk::now();
      if (!haveReal && realSeq.load(std::memory_order_relaxed) != seq0) haveReal = true, firstReal = now;
      const bool open = prep ? units == 0 : now < windowEnd();  // prepare: allocation + one unit (no calibration: the clock is high right after a frame)
      if (!open) break;  // units still on the GPU finish on their own (their slots are reused safely; release() waits before freeing the buffers)
      const bool holding = !haveReal && now >= base && holdDuty < 1.0;  // the finger is down but has not moved: lighter load
      spinPause.store(holding, std::memory_order_relaxed);
      const int depth = realActive.load(std::memory_order_acquire) > 0 ? 0 : haveReal || holding ? 1 : depthIdle;
      if (int(q.size()) >= depth && q.empty()) {  // nothing to retire, nothing to submit
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      bool submitted = false;
      {
        std::lock_guard<std::mutex> dl(c->mutex());
        if (c->deviceLost()) break;
        hal::Device& dev = c->core().dev();
        retire(dev);
        // a real frame may have been requested while this thread waited for the mutex: check again under it
        if (int(q.size()) < depth && realActive.load(std::memory_order_acquire) == 0 && !(holding && Clk::now() < restUntil)) {
          Fly fl{};
          if (!submitUnit(dev, &fl.f, &fl.bytes)) break;
          fl.t = Clk::now();
          fl.seq = realSeq.load();
          fl.piggy = piggyUnits.load();
          q.push_back(fl);
          bytesTotal += fl.bytes;
          ++units;
          submitted = true;
        }
      }
      if (!submitted) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    live.store(false, std::memory_order_release);
    if (prep) {  // the calibration waits for its last units
      std::lock_guard<std::mutex> dl(c->mutex());
      hal::Device& dev = c->core().dev();
      for (int i = 0; i < 2000 && !q.empty(); ++i) {
        retire(dev);
        if (!q.empty()) std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    if (early.size() >= 3) {
      std::sort(early.begin(), early.end());
      lowMBps = early[early.size() / 2];
    }
    spinOn.store(false);
    if (spinner.joinable()) spinner.join();
    const double wallMs = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
    char b[512];
    std::snprintf(b, sizeof b,
                  "{\"units\":%d,\"piggyback\":%d,\"wallMs\":%.1f,\"firstRealMs\":%.1f,\"busyMs\":%.1f,\"MB\":%.1f,\"firstMBps\":%.0f,\"lastMBps\":%.0f,"
                  "\"maxUnitMs\":%.2f,\"chunkKB\":%llu,\"allocMs\":%.2f,\"realFrames\":%llu,\"lowMBps\":%.0f}",
                  units, piggyUnits.load(), wallMs, haveReal ? std::chrono::duration<double, std::milli>(firstReal - tDown).count() : -1.0, busyMs,
                  double(bytesTotal) / double(mb), firstMBps, lastMBps, maxUnitMs, static_cast<unsigned long long>(chunk >> 10), allocMs,
                  static_cast<unsigned long long>(realSeq.load() - seq0), lowMBps);
    {
      std::lock_guard<std::mutex> lk(jm);
      json = b;
    }
    if (logLevel >= 1) std::fprintf(stderr, "keepwarm %s\n", b);
  }
};
KeepWarmPump& pump() {
  static auto* p = new KeepWarmPump;  // leaked like the provider (never torn down from a static destructor)
  return *p;
}
}  // namespace

void keepWarm(int ms) {
  if (ms == 0 || !provider().enabled()) return;
  pump().request(ms);
}
void keepWarmFrame(int delta) {
  KeepWarmPump& p = pump();
  if (delta > 0) p.realSeq.fetch_add(1, std::memory_order_relaxed);
  p.realActive.fetch_add(delta, std::memory_order_acq_rel);
}
void keepWarmAfterSubmit(Context& c) { pump().piggyback(c); }
std::string keepWarmJson() {
  KeepWarmPump& p = pump();
  std::lock_guard<std::mutex> lk(p.jm);
  return p.json;
}

}  // namespace dapple::engine::gpu
