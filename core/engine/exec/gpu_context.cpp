// gpu::Context (process-wide device selection Vulkan -> GLES 3.1, start-up self-test, device rebuild), the runtime flag and the
// GpuProvider EngineSession selects executors through (docs/engine/pipeline-architecture.md 5.9, 6.6).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "engine/exec/engine_session.h"
#include "engine/exec/gpu_executor.h"
#include "engine/hal/select.h"
#include "engine/nodes/gpu_common.h"
#include "parity.h"
#if defined(DAPPLE_HAL_GLES)
#include "engine/hal/gles/gles_device.h"
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
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
    const auto lut = r.bake(src, 33);
    FastOptions fo;
    fo.spatial = SpatialQuality::Full;
    int w = 0, h = 0;
    std::uint8_t* cpu = r.renderBakedRgba(src, *lut, fo, w, h);
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
    test = [&st](hal::Device& dev, std::string& why) -> bool {
#if defined(DAPPLE_HAL_GLES)
      if (dev.caps().api == hal::Api::Gles31) {
        const hal::gles::SelfTestReport r = hal::gles::selfTest(dev);
        if (!r.ok) return why = r.text, false;
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
    hal::Selection sel = hal::selectDevice(d_->opt.device, hal::apiName(d_->api) == std::string("gles31") ? "gles" : "vulkan");
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

bool envEnabled() {
  if (const char* v = std::getenv("DAPPLE_GPU")) return v[0] == '1';
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.dapple.gpu", buf) > 0) return buf[0] == '1';
#endif
  return false;
}

std::string envApi() {
  if (const char* v = std::getenv("DAPPLE_GPU_API")) return v;
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.dapple.gpu_api", buf) > 0) return buf;
#endif
  return {};
}

class ProviderImpl final : public GpuProvider {
 public:
  bool enabled() const override { return runtimeEnabled() && !broken_.load(); }
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
    started_ = false;
    std::string w2;
    std::shared_ptr<Context> n = make(&w2);
    if (!n) {
      broken_.store(true);
      why += (why.empty() ? "" : "; ") + w2;
      return false;
    }
    ctx_ = n;
    started_ = true;
    return false;  // the failed API did not recover; the caller counts a demotion and goes on with a new executor
  }
  std::string statusJson() override {
    const bool en = runtimeEnabled();
    std::string why;
    std::shared_ptr<Context> c = en && !broken_.load() ? context(&why) : nullptr;
    const GpuCounters cnt = gpuCounters();
    std::string j = std::string("{\"enabled\":") + (en ? "true" : "false") + ",\"available\":" + (c ? "true" : "false");
    if (c)
      j += ",\"api\":\"" + std::string(c->api() == hal::Api::Gles31 ? "gles31" : "vulkan") + "\",\"device\":\"" + jsonEsc(c->caps().name) + "\",\"deviceKey\":\"" +
           c->caps().key() + "\",\"selfTest\":\"" + jsonEsc(c->selfTestReport()) + "\",\"selection\":" + c->selectionJson();
    else if (en) j += ",\"reason\":\"" + jsonEsc(broken_.load() ? std::string("device lost") : why) + "\"";
    j += ",\"frames\":" + std::to_string(cnt.frames) + ",\"fallbacks\":" + std::to_string(cnt.fallbacks) + ",\"transients\":" + std::to_string(cnt.transients) +
         ",\"deviceLost\":" + std::to_string(cnt.lost) + ",\"demotions\":" + std::to_string(cnt.demotions) + ",\"rebuilds\":" + std::to_string(c ? c->rebuilds() : 0);
    return j + "}";
  }
  std::size_t pooledBytes() override {
    std::shared_ptr<Context> c = context(nullptr);
    return c ? c->pooledBytes() : 0;
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

  std::shared_ptr<Context> context(std::string* why) {
    if (!runtimeEnabled()) {
      if (why) *why = "GPU backend disabled";
      return nullptr;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (broken_.load()) {
      if (why) *why = lastWhy_.empty() ? "GPU unavailable for the rest of the process" : lastWhy_;
      return nullptr;
    }
    if (!started_) {
      started_ = true;
      ctx_ = make(&lastWhy_);
      ++gen_;
    }
    if (!ctx_ && why) *why = lastWhy_;
    return ctx_;
  }

 private:
  // mtx_ held
  std::shared_ptr<Context> make(std::string* why) {
    ContextOptions o;
    if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) o.device.allowSoftware = v[0] == '1';
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
    if (excluded_.empty()) {  // first selection: Vulkan -> GLES 3.1 (or the one DAPPLE_GPU_API names), reasons of every attempt kept
      o.api = api;
      std::string err;
      std::shared_ptr<Context> c = Context::create(o, err);
      if (!c && why) *why = err;
      return c;
    }
    // after a demotion: the candidates that were not given up yet, in order
    std::string reasons;
    for (const char* cand : {"vulkan", "gles"}) {
      const hal::Api a = std::string(cand) == "vulkan" ? hal::Api::Vulkan : hal::Api::Gles31;
      if (excluded_.count(a) || (!api.empty() && api != cand)) continue;
      o.api = cand;
      std::string err;
      if (std::shared_ptr<Context> c = Context::create(o, err)) return c;
      reasons += (reasons.empty() ? "" : "; ") + err;
    }
    if (why) *why = reasons.empty() ? "no backend left" : reasons;
    return nullptr;
  }

  std::mutex mtx_;
  std::shared_ptr<Context> ctx_;
  std::set<hal::Api> excluded_;
  bool started_ = false;
  std::atomic<bool> broken_{false};
  std::uint64_t gen_ = 1;
  std::string lastWhy_;
};

ProviderImpl& provider() {
  static auto* p = new ProviderImpl;  // leaked on purpose: tearing the device down from a static destructor runs after the
                                      // Vulkan loader / driver have been unloaded (crashes on Android)
  return *p;
}
}  // namespace

bool runtimeEnabled() {
  const int o = g_override.load();
  return o >= 0 ? o != 0 : envEnabled();
}
void setRuntimeEnabled(int enabled) { g_override.store(enabled); }
void setPresentExtensions(int on) { g_presentExt.store(on); }
void installProvider() { installGpuProvider(&provider()); }
std::shared_ptr<Context> sharedContext(std::string* why) { return provider().context(why); }
std::string statusJson() { return provider().statusJson(); }

}  // namespace dapple::engine::gpu
