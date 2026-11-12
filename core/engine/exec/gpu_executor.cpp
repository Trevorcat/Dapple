// GpuExecutor: device core, Backend + Session (resident sources, lattice cache, whole-frame render, tiled export, self-test,
// runtime flag) on top of the HAL, the scheduler and the node record functions. Ported from phase-1 core/gpu/src/session.cpp.
#include "engine/exec/gpu_executor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/gpu.h"
#include "engine/exec/gpu_frame.h"
#include "parity.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
// While a frame is in flight the calling thread sleeps in the fence wait and wakes on whatever core the scheduler picks:
// usually a little core at a low clock, where reading the mapped readback buffer took 49 ms instead of 7-13 ms (Pixel 5,
// 1.7 MP). The thread is pinned to the fastest cores for the duration of render() and restored afterwards.
#include <sched.h>
namespace {
cpu_set_t bigCoreSet(bool& ok) {
  static cpu_set_t set;
  static bool have = false, valid = false;
  if (!have) {
    have = true;
    CPU_ZERO(&set);
    long maxf[64] = {0}, top = 0;
    for (int c = 0; c < 64; ++c) {
      char path[96];
      std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
      if (FILE* f = std::fopen(path, "r")) {
        if (std::fscanf(f, "%ld", &maxf[c]) != 1) maxf[c] = 0;
        std::fclose(f);
        top = std::max(top, maxf[c]);
      }
    }
    int n = 0;
    for (int c = 0; c < 64; ++c)
      if (maxf[c] > 0 && maxf[c] * 10 >= top * 8) {
        CPU_SET(c, &set);
        ++n;
      }
    valid = n > 0;
  }
  ok = valid;
  return set;
}
struct BigCoreScope {
  cpu_set_t prev;
  bool active = false;
  BigCoreScope() {
    if (std::getenv("DAPPLE_GPU_NO_PIN")) return;
    bool ok = false;
    const cpu_set_t big = bigCoreSet(ok);
    if (!ok || sched_getaffinity(0, sizeof prev, &prev) != 0) return;
    cpu_set_t both;
    CPU_AND(&both, &prev, &big);
    if (CPU_COUNT(&both) == 0) return;
    active = sched_setaffinity(0, sizeof both, &both) == 0;
  }
  ~BigCoreScope() {
    if (active) sched_setaffinity(0, sizeof prev, &prev);
  }
};
}  // namespace
#else
namespace {
struct BigCoreScope {};
}  // namespace
#endif

namespace eg = dapple::engine::gpu;

// ============================================================================================ Core ===
namespace dapple::engine::gpu {

std::unique_ptr<Core> Core::create(const hal::DeviceOptions& opt, std::string& err) {
  std::unique_ptr<Core> c(new Core);
  c->dev_ = hal::createDevice(hal::Api::Vulkan, opt, err);
  if (!c->dev_) return nullptr;
  c->pool_ = std::make_unique<GpuPool>(*c->dev_);
  for (int i = 0; i < hal::Device::kSlots; ++i) {
    if (c->dev_->createBuffer({16384, hal::MemKind::Upload, true}, &c->slots_[i].weights) != hal::Status::Ok ||
        c->dev_->createBuffer({8u << 20, hal::MemKind::Upload, true}, &c->slots_[i].staging) != hal::Status::Ok)
      return err = c->dev_->lastError(), nullptr;
  }
  if (c->dev_->createBuffer({256, hal::MemKind::DeviceLocal, true}, &c->gBuf) != hal::Status::Ok ||
      c->dev_->createBuffer({256 * 1024, hal::MemKind::DeviceLocal, true}, &c->partBuf) != hal::Status::Ok)
    return err = c->dev_->lastError(), nullptr;
  return c;
}

Core::~Core() {
  if (!dev_) return;
  dev_->waitIdle();
  pool_.reset();
  for (SlotRes& s : slots_) {
    dev_->destroy(s.staging);
    dev_->destroy(s.readback);
    dev_->destroy(s.weights);
  }
  dev_->destroy(gBuf);
  dev_->destroy(partBuf);
}

bool Core::ensureStaging(int slot, std::uint64_t bytes, std::string& err) {
  SlotRes& s = slots_[slot];
  if (s.staging.bytes >= bytes) return true;
  dev_->destroy(s.staging);
  if (dev_->createBuffer({std::max<std::uint64_t>(bytes, 8u << 20), hal::MemKind::Upload, true}, &s.staging) != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}
bool Core::ensureReadback(int slot, std::uint64_t bytes, std::string& err) {
  SlotRes& s = slots_[slot];
  if (s.readback.bytes >= bytes) return true;
  dev_->destroy(s.readback);
  if (dev_->createBuffer({bytes, hal::MemKind::Readback, false}, &s.readback) != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}

Core::Stats Core::statsOf(hal::FenceH f) {
  Stats s;
  if (!f) return s;
  const auto st = dev_->stats(f);
  s.passes = st.passes;
  s.barriers = st.barriers;
  s.times = dev_->passTimes(f);
  for (const auto& t : s.times) s.gpuMs += t.ms;
  return s;
}

bool Core::uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, const hal::ImageH& dst, int dx, int dy, std::string& err) {
  const std::size_t rowBytes = std::size_t(w) * 12;
  const std::size_t chunkBytes = 24u << 20;
  const int rows = int(std::max<std::size_t>(1, std::min<std::size_t>(std::size_t(h), chunkBytes / rowBytes)));
  for (int y = 0; y < h; y += rows) {
    const int n = std::min(rows, h - y);
    auto cl = dev_->begin();
    if (!cl) return err = dev_->lastError().empty() ? "no frame slot" : dev_->lastError(), false;
    if (!ensureStaging(cl->slot(), rowBytes * std::size_t(n), err)) return false;
    SlotRes& s = slots_[cl->slot()];
    auto* dstp = static_cast<std::uint8_t*>(s.staging.mapped);
    if (pitchFloats == std::size_t(w) * 3) {
      std::memcpy(dstp, rgb + std::size_t(y) * pitchFloats, rowBytes * std::size_t(n));
    } else {
      for (int j = 0; j < n; ++j) std::memcpy(dstp + std::size_t(j) * rowBytes, rgb + std::size_t(y + j) * pitchFloats, rowBytes);
    }
    GpuGraph g(*dev_, *pool_);
    std::string e2;
    GpuRecorder rec(*dev_, g, e2);
    const H out = g.external(dst);
    // the convert pass reads the staging buffer: build the dispatch by hand (the uniform carries the chunk placement)
    if (!recordConvert(rec, s.staging, out, w, n, dx, dy + y)) return err = e2, false;
    if (runGraph(*dev_, g, *cl, nullptr, err) != hal::Status::Ok) return false;
  }
  if (dev_->waitAll() != hal::Status::Ok) return err = dev_->lastError(), false;
  return true;
}

}  // namespace dapple::engine::gpu

// ============================================================================================ gpu API ===
namespace dapple::gpu {
using namespace engine::gpu;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// must match core/src/renderer.cpp (kBakeMax, kToneOctLo/Hi); the self-test notices a drift
constexpr float kBakeMax = 1.5f;
constexpr int kToneBase = (127 - 24) << 8;
constexpr std::size_t kToneN = ((6 + 24) << 8) + 2;
constexpr int kLat2N = 33;
constexpr float kLat2Max = 8.f;

void resizedDims(int w, int h, int longEdge, int& rw, int& rh) {  // image.cpp resizeToLongEdge
  const int srcLong = std::max(w, h);
  if (longEdge <= 0 || srcLong <= longEdge) {
    rw = w;
    rh = h;
    return;
  }
  const double scale = double(longEdge) / srcLong;
  rw = std::max(1, int(std::lround(w * scale)));
  rh = std::max(1, int(std::lround(h * scale)));
}

// Geometry::mapF of the CPU renderer for the footprint of a rectangle.
void mapPlan(const FramePlan& P, float px, float py, float& sx, float& sy) {
  float u = px - P.fullW * 0.5f;
  const float v = py - P.fullH * 0.5f;
  if (P.flip) u = -u;
  const float fx = u * P.cs + v * P.sn + P.fw * 0.5f;
  const float fy = -u * P.sn + v * P.cs + P.fh * 0.5f;
  switch (P.rot) {
    case 0: sx = fx; sy = fy; break;
    case 1: sx = fy; sy = float(P.srcH) - fx; break;
    case 2: sx = float(P.srcW) - fx; sy = float(P.srcH) - fy; break;
    default: sx = float(P.srcW) - fy; sy = fx; break;
  }
}

// Rectangle of the (resized) source the point pass reads for the output pixels of `work`.
WRect footprint(const FramePlan& P, const WRect& work) {
  if (!P.useGeo) return work;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (int cx = 0; cx < 2; ++cx)
    for (int cy = 0; cy < 2; ++cy) {
      const float px = P.ox0 + float(cx ? work.x1 : work.x0) * P.stepX, py = P.oy0 + float(cy ? work.y1 : work.y0) * P.stepY;
      float sx, sy;
      mapPlan(P, px, py, sx, sy);
      x0 = std::min(x0, sx);
      x1 = std::max(x1, sx);
      y0 = std::min(y0, sy);
      y1 = std::max(y1, sy);
    }
  const int m = P.exact ? 2 : 4;
  WRect r;
  r.x0 = std::max(0, int(std::floor(x0)) - m);
  r.y0 = std::max(0, int(std::floor(y0)) - m);
  r.x1 = std::min(P.srcW, int(std::ceil(x1)) + m);
  r.y1 = std::min(P.srcH, int(std::ceil(y1)) + m);
  return r;
}

hal::DeviceOptions toDeviceOptions(const BackendOptions& o) {
  hal::DeviceOptions d;
  d.allowSoftware = o.allowSoftware;
  d.deviceIndex = o.deviceIndex;
  d.profile = o.profile;
  d.blocklist = o.blocklist;
  d.pipelineCachePath = o.pipelineCachePath;
  d.poolBudgetBytes = o.poolBudgetBytes;
  return d;
}

DeviceInfo toInfo(const hal::DeviceCaps& c) {
  DeviceInfo i;
  i.name = c.name;
  i.driver = c.driver;
  i.memoryTypes = c.memoryTypes;
  i.apiVersion = c.apiVersion;
  i.driverVersion = c.driverVersion;
  i.vendorId = c.vendorId;
  i.deviceId = c.deviceId;
  i.deviceType = c.deviceType;
  i.maxImageDim2D = c.maxImage2D;
  i.maxImageDim3D = c.maxImage3D;
  i.maxSharedMem = c.maxSharedBytes;
  i.maxInvocations = c.maxWorkgroupInvocations;
  i.subgroupSize = c.subgroupSize;
  i.subgroupOps = c.subgroupOps;
  i.fp16Arith = c.fp16Arithmetic;
  i.storage16 = c.storage16;
  i.linearFilterRgba32f = c.linearFilterRgba32f;
  i.timestamps = c.timestamps;
  i.unifiedMemory = c.unifiedMemory;
  i.deviceLocalBytes = c.deviceLocalBytes;
  i.maxAllocation = c.maxBufferBytes;
  return i;
}

Session::Failure classify(hal::Status s) {
  switch (s) {
    case hal::Status::DeviceLost: return Session::Failure::DeviceLost;
    case hal::Status::Unsupported:
    case hal::Status::InvalidArgument: return Session::Failure::Unsupported;
    default: return Session::Failure::Transient;
  }
}

}  // namespace

// ============================================================================================ Backend ===
struct Backend::Impl {
  std::unique_ptr<Core> core;
  std::vector<std::unique_ptr<Core>> retired;  // lost devices: kept alive so stale handles never meet a destroyed device
  BackendOptions opt;
  DeviceInfo info;
  std::string selfTest;
  std::mutex mtx;
  std::atomic<std::uint64_t> epoch{1};
  int rebuilds = 0;
};

namespace {
bool runSelfTest(Backend& b, std::string& report);
}

Backend::Backend() : d_(new Impl) {}
Backend::~Backend() = default;

namespace {
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

std::shared_ptr<Backend> Backend::create(const BackendOptions& opt, std::string& err) {
  std::shared_ptr<Backend> b(new Backend);
  b->d_->opt = opt;
  b->d_->core = Core::create(toDeviceOptions(opt), err);
  if (!b->d_->core) return nullptr;
  b->d_->info = toInfo(b->d_->core->dev().caps());
  if (opt.selfTest) {
    std::string report, boxReport;
    b->d_->core->boxScanOk = boxSelfTest(*b->d_->core, boxReport);
    const bool ok = runSelfTest(*b, report);
    b->d_->selfTest = report + "; " + boxReport;
    if (!ok) {
      err = "GPU self-test failed on " + b->d_->info.name + " (" + b->d_->info.key() + "): " + report;
      return nullptr;
    }
  }
  return b;
}

const DeviceInfo& Backend::info() const { return d_->info; }
const std::string& Backend::selfTestReport() const { return d_->selfTest; }
std::size_t Backend::pooledBytes() const { return d_->core->pool().pooledBytes(); }
std::unique_ptr<Session> Backend::createSession() { return std::unique_ptr<Session>(new Session(shared_from_this())); }
int Backend::rebuilds() const { return d_->rebuilds; }
bool Backend::deviceLost() const { return d_->core->dev().state() == hal::DeviceState::Lost; }

bool Backend::rebuild(std::string& err) {
  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->rebuilds >= 1) return err = "device already rebuilt once in this process", false;
    ++d_->rebuilds;
    auto fresh = Core::create(toDeviceOptions(d_->opt), err);
    if (!fresh) return false;
    d_->retired.push_back(std::move(d_->core));
    d_->core = std::move(fresh);
    d_->info = toInfo(d_->core->dev().caps());
    d_->epoch.fetch_add(1);
  }
  if (d_->opt.selfTest) {
    std::string report, boxReport;
    d_->core->boxScanOk = boxSelfTest(*d_->core, boxReport);
    if (!runSelfTest(*this, report)) return err = "self-test failed after the device rebuild: " + report, false;
    d_->selfTest = report + "; " + boxReport;
  }
  return true;
}

std::string DeviceInfo::text() const {
  hal::DeviceCaps c;
  c.name = name;
  c.driver = driver;
  c.apiVersion = apiVersion;
  c.vendorId = vendorId;
  c.deviceType = deviceType;
  c.maxImage2D = maxImageDim2D;
  c.maxImage3D = maxImageDim3D;
  c.maxSharedBytes = maxSharedMem;
  c.maxWorkgroupInvocations = maxInvocations;
  c.subgroupSize = subgroupSize;
  c.subgroupOps = subgroupOps;
  c.fp16Arithmetic = fp16Arith;
  c.storage16 = storage16;
  c.linearFilterRgba32f = linearFilterRgba32f;
  c.timestamps = timestamps;
  c.deviceLocalBytes = deviceLocalBytes;
  c.unifiedMemory = unifiedMemory;
  return c.text();
}
std::string DeviceInfo::key() const {
  char b[96];
  std::snprintf(b, sizeof b, "%04x:%04x:%08x", vendorId, deviceId, driverVersion);
  return b;
}

// ============================================================================================ Session ===
struct Session::Impl {
  std::shared_ptr<Backend> be;
  Backend::Impl& bi() { return *be->d_; }
  Core& core() { return *be->d_->core; }
  hal::Device& dev() { return be->d_->core->dev(); }

  const SourceImage* cpu = nullptr;
  std::uint64_t stamp = 0;
  int W = 0, H = 0;
  std::uint64_t epoch = 0;
  hal::ImageH resident;  // the source as RGBA16F, uploaded on first use
  bool haveResident = false;
  Failure failure = Failure::None;
  Failure injected = Failure::None;
  int injectCount = 0;
  bool takeInjected(std::string& err) {
    if (injectCount <= 0) return false;
    --injectCount;
    failure = injected;
    err = "injected failure (test)";
    return true;
  }

  struct Resized {
    int w = 0, h = 0;
    hal::ImageH img;
    std::uint64_t used = 0;
  };
  std::vector<std::unique_ptr<Resized>> resized;

  struct Lut {
    std::string key;
    hal::ImageH lat, lat2;
    hal::BufferH tone;
    int n = 0, n2 = 0;
    std::uint64_t used = 0;
  };
  std::unique_ptr<Lut> luts[2];
  std::uint64_t tick = 0;

  // A rebuilt device invalidates every handle: forget them (the lost device is kept alive by the Backend).
  void checkEpoch() {
    const std::uint64_t e = bi().epoch.load();
    if (e == epoch) return;
    epoch = e;
    resident = {};
    haveResident = false;
    resized.clear();
    for (auto& l : luts) l.reset();
  }

  void dropResident() {
    if (haveResident) dev().destroy(resident);
    haveResident = false;
    for (auto& r : resized) dev().destroy(r->img);
    resized.clear();
  }
  void dropLut(std::unique_ptr<Lut>& l) {
    if (!l) return;
    dev().destroy(l->lat);
    dev().destroy(l->lat2);
    dev().destroy(l->tone);
    l.reset();
  }
  ~Impl() {
    if (!be) return;
    if (epoch == bi().epoch.load()) {
      dev().waitIdle();
      dropResident();
      for (auto& l : luts) dropLut(l);
    }
  }

  std::uint64_t residentLimit() { return std::min<std::uint64_t>(std::uint64_t(double(dev().caps().deviceLocalBytes) * 0.35), 1536ull << 20); }
  bool canResident(int w, int h) {
    const auto& c = dev().caps();
    return w > 0 && h > 0 && std::uint32_t(w) <= c.maxImage2D && std::uint32_t(h) <= c.maxImage2D && std::uint64_t(w) * h * 8 <= residentLimit();
  }

  bool fail(Failure f, std::string& err, const std::string& msg) {
    failure = f;
    err = msg;
    return false;
  }
  bool failStatus(hal::Status s, std::string& err, const std::string& msg) { return fail(classify(s), err, msg.empty() ? dev().lastError() : msg); }

  bool ensureResident(std::string& err) {
    if (haveResident) return true;
    if (!cpu) return fail(Failure::Unsupported, err, "no source");
    if (!canResident(W, H)) return fail(Failure::Unsupported, err, "source too large for a resident GPU copy");
    hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(W), std::uint32_t(H), 1}, &resident);
    if (s != hal::Status::Ok) return failStatus(s, err, "");
    if (!core().uploadRgbF32(cpu->linear.data.data(), std::size_t(W) * 3, W, H, resident, 0, 0, err)) {
      dev().destroy(resident);
      return fail(dev().state() == hal::DeviceState::Lost ? Failure::DeviceLost : Failure::Transient, err, err);
    }
    haveResident = true;
    return true;
  }

  // The resident source resized (Lanczos-3) to w x h; cached (two sizes).
  hal::ImageH* resizedSource(int w, int h, std::string& err) {
    if (w == W && h == H) return &resident;
    for (auto& r : resized)
      if (r->w == w && r->h == h) {
        r->used = ++tick;
        return &r->img;
      }
    if (!canResident(w, h)) return fail(Failure::Unsupported, err, "resized source too large"), nullptr;
    auto r = std::make_unique<Resized>();
    r->w = w;
    r->h = h;
    hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h), 1}, &r->img);
    if (s != hal::Status::Ok) return failStatus(s, err, ""), nullptr;
    auto cl = dev().begin();
    if (!cl) return failStatus(hal::Status::Timeout, err, ""), nullptr;
    GpuGraph g(dev(), core().pool());
    GpuRecorder rec(dev(), g, err);
    rec.weights = core().slot(cl->slot()).weights;
    rec.boxScanMin = core().boxScanOk ? -1 : 0;
    const eg::H src = g.external(resident);
    const SrcWin full{0, 0, W, H};
    const eg::H out = recordResize(rec, src, full, W, H, w, h, WRect{0, 0, w, h});
    if (out < 0) return fail(Failure::Transient, err, err), nullptr;
    g.copyImage(out, g.external(r->img), std::uint32_t(w), std::uint32_t(h));
    hal::FenceH f;
    s = runGraph(dev(), g, *cl, &f, err);
    if (s == hal::Status::Ok) s = dev().wait(f, 30ull * 1000 * 1000 * 1000);
    if (s != hal::Status::Ok) return failStatus(s, err, err), nullptr;
    r->used = ++tick;
    if (resized.size() >= 2) {
      auto victim = std::min_element(resized.begin(), resized.end(), [](auto& a, auto& b) { return a->used < b->used; });
      dev().destroy((*victim)->img);
      resized.erase(victim);
    }
    resized.push_back(std::move(r));
    return &resized.back()->img;
  }

  // Lattice + tone table on the device (cached per bake key).
  Lut* ensureLut(const Renderer& r, const BakedLut& lut, std::string& err) {
    for (auto& l : luts)
      if (l && l->key == lut.key) {
        l->used = ++tick;
        return l.get();
      }
    if (lut.n < 2 || lut.rgb.size() != std::size_t(lut.n) * lut.n * lut.n * 3) return fail(Failure::Unsupported, err, "lattice not usable (film only bake?)"), nullptr;
    if (lut.tone.size() != kToneN) return fail(Failure::Unsupported, err, "tone table size differs from the GPU backend's"), nullptr;
    std::vector<float> over = r.bakeOverflow(*cpu, lut, kLat2N, kLat2Max);
    if (over.size() != std::size_t(kLat2N) * kLat2N * kLat2N * 3) return fail(Failure::Unsupported, err, "overflow lattice unavailable"), nullptr;
    int slotIdx = 0;
    if (luts[0] && (!luts[1] || luts[0]->used > luts[1]->used)) slotIdx = 1;
    if (luts[slotIdx]) {
      dev().waitIdle();
      dropLut(luts[slotIdx]);
    }
    auto l = std::make_unique<Lut>();
    l->key = lut.key;
    l->n = lut.n;
    l->n2 = kLat2N;
    hal::Status s = dev().createImage({hal::Format::RGBA32F, std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n)}, &l->lat);
    if (s == hal::Status::Ok) s = dev().createImage({hal::Format::RGBA32F, kLat2N, kLat2N, kLat2N}, &l->lat2);
    if (s == hal::Status::Ok) s = dev().createBuffer({kToneN * 4, hal::MemKind::DeviceLocal, true}, &l->tone);
    if (s != hal::Status::Ok) {
      dev().destroy(l->lat);
      dev().destroy(l->lat2);
      return failStatus(s, err, ""), nullptr;
    }
    const std::size_t latBytes = std::size_t(lut.n) * lut.n * lut.n * 16, lat2Bytes = std::size_t(kLat2N) * kLat2N * kLat2N * 16;
    const std::size_t toneBytes = (kToneN * 4 + 255) / 256 * 256;
    auto cl = dev().begin();
    if (!cl) return failStatus(hal::Status::Timeout, err, ""), nullptr;
    if (!core().ensureStaging(cl->slot(), toneBytes + latBytes + lat2Bytes, err)) return fail(Failure::Transient, err, err), nullptr;
    auto& sr = core().slot(cl->slot());
    auto* base = static_cast<std::uint8_t*>(sr.staging.mapped);
    std::memcpy(base, lut.tone.data(), kToneN * 4);
    auto texels = [](const std::vector<float>& rgb, float* o) {
      const std::size_t n = rgb.size() / 3;
      for (std::size_t i = 0; i < n; ++i) {
        o[4 * i] = rgb[3 * i];
        o[4 * i + 1] = rgb[3 * i + 1];
        o[4 * i + 2] = rgb[3 * i + 2];
        o[4 * i + 3] = 1.f;
      }
    };
    texels(lut.rgb, reinterpret_cast<float*>(base + toneBytes));
    texels(over, reinterpret_cast<float*>(base + toneBytes + latBytes));
    GpuGraph g(dev(), core().pool());
    g.copyBufferToBuffer(sr.staging, g.externalBuf(l->tone), kToneN * 4, 0);
    g.copyBufferToImage(sr.staging, g.external(l->lat), std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n), toneBytes);
    g.copyBufferToImage(sr.staging, g.external(l->lat2), kLat2N, kLat2N, kLat2N, toneBytes + latBytes);
    hal::FenceH f;
    s = runGraph(dev(), g, *cl, &f, err);
    if (s == hal::Status::Ok) s = dev().wait(f, 30ull * 1000 * 1000 * 1000);
    if (s != hal::Status::Ok) {
      dropLut(l);
      return failStatus(s, err, err), nullptr;
    }
    l->used = ++tick;
    luts[slotIdx] = std::move(l);
    return luts[slotIdx].get();
  }

  LutBind lutBind(GpuGraph& g, Lut& l) {
    LutBind b;
    b.tone = g.externalBuf(l.tone);
    b.lat = g.external(l.lat);
    b.lat2 = g.external(l.lat2);
    b.n = l.n;
    b.n2 = l.n2;
    b.toneN = int(kToneN);
    b.toneBase = kToneBase;
    b.max1 = kBakeMax;
    b.max2 = kLat2Max;
    return b;
  }

  void finishFrame(FrameStats* st, const Core::Stats& s, double wall) {
    if (!st) return;
    st->wallMs = wall;
    st->gpuMs = s.gpuMs;
    st->passes = s.passes;
    st->barriers = s.barriers;
    st->passTimes.clear();
    for (const auto& t : s.times) st->passTimes.push_back({t.name, t.ms});
  }
};

Session::Session(std::shared_ptr<Backend> b) : d_(new Impl) {
  d_->be = std::move(b);
  d_->epoch = d_->bi().epoch.load();
}
Session::~Session() {
  if (d_ && d_->be) {
    const std::shared_ptr<Backend> be = d_->be;  // keeps the backend (and its mutex) alive while the session is torn down
    std::lock_guard<std::mutex> lk(be->d_->mtx);
    d_.reset();
  }
}
Session::Failure Session::lastFailure() const { return d_->failure; }
void Session::injectFailure(Failure f, int count) {
  d_->injected = f;
  d_->injectCount = count;
}

bool Session::setSource(const SourceImage& src, std::uint64_t stamp, std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
  d_->checkEpoch();
  if (src.linear.empty()) return err = "empty source", false;
  if (d_->cpu == &src && d_->stamp == stamp && d_->W == src.linear.width && d_->H == src.linear.height) return true;
  d_->dropResident();
  d_->cpu = &src;
  d_->stamp = stamp;
  d_->W = src.linear.width;
  d_->H = src.linear.height;
  return true;
}
bool Session::hasSource() const { return d_->cpu != nullptr; }
bool Session::canRenderResident(int w, int h) const { return d_->canResident(w, h); }

bool Session::frameSize(const Renderer& r, const FastOptions& opt, const FrameOptions& fo, int& w, int& h) const {
  if (!d_->cpu) return false;
  int rw, rh;
  resizedDims(d_->W, d_->H, fo.srcLongEdge, rw, rh);
  const FramePlan p = r.framePlan(rw, rh, d_->cpu->sceneReferred, opt);
  if (!p.supported) return false;
  w = p.outW;
  h = p.outH;
  return true;
}

std::uint8_t* Session::render(const Renderer& r, const BakedLut& lut, const FastOptions& opt, const FrameOptions& fo, int& w, int& h, FrameStats* stats,
                              std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
  BigCoreScope pin;
  const auto t0 = Clock::now();
  Impl& d = *d_;
  d.checkEpoch();
  d.failure = Failure::None;
  hal::Device& dev = d.dev();
  if (d.takeInjected(err)) return nullptr;
  if (dev.state() == hal::DeviceState::Lost) return d.fail(Failure::DeviceLost, err, "device lost"), nullptr;
  if (!d.cpu) return d.fail(Failure::Unsupported, err, "no source"), nullptr;
  int rw, rh;
  resizedDims(d.W, d.H, fo.srcLongEdge, rw, rh);
  const FramePlan plan = r.framePlan(rw, rh, d.cpu->sceneReferred, opt);
  if (!plan.supported) return d.fail(Failure::Unsupported, err, "unsupported on the GPU: " + plan.why), nullptr;
  const auto& caps = dev.caps();
  if (plan.outW <= 0 || plan.outH <= 0 || std::uint32_t(plan.outW) > caps.maxImage2D || std::uint32_t(plan.outH) > caps.maxImage2D)
    return d.fail(Failure::Unsupported, err, "frame size outside the device limits"), nullptr;
  if (!d.ensureResident(err)) return nullptr;
  hal::ImageH* S = d.resizedSource(rw, rh, err);
  if (!S) return nullptr;
  Impl::Lut* L = d.ensureLut(r, lut, err);
  if (!L) return nullptr;
  const double uploadMs = msSince(t0);

  auto cl = dev.begin();
  if (!cl) return d.failStatus(hal::Status::Timeout, err, ""), nullptr;
  const int slot = cl->slot();
  const std::size_t bytes = std::size_t(plan.outW) * plan.outH * 4;
  if (!d.core().ensureReadback(slot, bytes, err)) return d.fail(Failure::Transient, err, err), nullptr;
  GpuGraph g(dev, d.core().pool());
  GpuRecorder rec(dev, g, err);
  rec.weights = d.core().slot(slot).weights;
  rec.boxScanMin = d.core().boxScanOk ? -1 : 0;
  rec.gBuf = d.core().gBuf;
  rec.partBuf = d.core().partBuf;
  FrameJob job;
  job.plan = &plan;
  job.resizeSharpen = (rw < d.W || rh < d.H) ? fo.resizeSharpen : 0.f;
  job.mode = Mode::Whole;
  job.interior = job.work = WRect{0, 0, plan.outW, plan.outH};
  job.src.img = g.external(*S);
  job.lut = d.lutBind(g, *L);
  H out8 = -1;
  if (!buildFrame(rec, job, &out8)) return d.fail(Failure::Transient, err, err), nullptr;

  g.copyImageToBuffer(out8, d.core().slot(slot).readback, 0, 0, std::uint32_t(plan.outW), std::uint32_t(plan.outH));
  const auto tSub = Clock::now();
  hal::FenceH fence;
  hal::Status s = runGraph(dev, g, *cl, &fence, err);
  if (s != hal::Status::Ok) return d.failStatus(s, err, err), nullptr;
  const auto tWait = Clock::now();
  s = dev.wait(fence, 30ull * 1000 * 1000 * 1000);
  if (s != hal::Status::Ok) return d.failStatus(s, err, ""), nullptr;
  const auto t1 = Clock::now();
  auto* out = static_cast<std::uint8_t*>(std::malloc(bytes));
  if (!out) return d.fail(Failure::Transient, err, "out of memory"), nullptr;
  std::memcpy(out, d.core().slot(slot).readback.mapped, bytes);
  const double copyMs = msSince(t1);
  w = plan.outW;
  h = plan.outH;
  const auto t2 = Clock::now();
  GpuPool& pool = d.core().pool();
  if (pool.freeBytes() > dev.poolBudget()) pool.trim(dev.poolBudget());
  const double trimMs = msSince(t2);
  d.finishFrame(stats, d.core().statsOf(fence), msSince(t0));
  if (stats) {
    stats->uploadMs = uploadMs;
    stats->readbackMs = copyMs;
    stats->trimMs = trimMs;
    stats->recordMs = std::chrono::duration<double, std::milli>(tSub - t0).count() - uploadMs;
    stats->submitMs = std::chrono::duration<double, std::milli>(tWait - tSub).count();
    stats->waitMs = std::chrono::duration<double, std::milli>(t1 - tWait).count();
  }
  return out;
}

// ============================================================================================ export ===
bool Session::exportTiled(const Renderer& r, const SourceImage& src, const BakedLut& lut, const FastOptions& opt, const ExportOptions& eo, FrameStats* stats,
                          std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
  const auto t0 = Clock::now();
  Impl& d = *d_;
  d.checkEpoch();
  d.failure = Failure::None;
  hal::Device& dev = d.dev();
  GpuPool& pool = d.core().pool();
  const double gpu0 = dev.gpuAccumMs();
  if (d.takeInjected(err)) return false;
  if (dev.state() == hal::DeviceState::Lost) return d.fail(Failure::DeviceLost, err, "device lost");
  if (!eo.sink) return d.fail(Failure::Unsupported, err, "no tile sink");
  if (!d.cpu || d.cpu != &src) {
    d.dropResident();
    d.cpu = &src;
    d.W = src.linear.width;
    d.H = src.linear.height;
  }
  int rw, rh;
  resizedDims(d.W, d.H, eo.frame.srcLongEdge, rw, rh);
  if (opt.region.active()) return d.fail(Failure::Unsupported, err, "tiled export renders the whole picture");
  const FramePlan plan = r.framePlan(rw, rh, src.sceneReferred, opt);
  if (!plan.supported) return d.fail(Failure::Unsupported, err, "unsupported on the GPU: " + plan.why);
  const bool down = rw < d.W || rh < d.H;
  const float rsharp = down ? eo.frame.resizeSharpen : 0.f;
  const SpatialRadii R = spatialRadii(plan, rsharp);
  const int maxDim = int(dev.caps().maxImage2D);
  int tile = std::max(64, eo.tile / 4 * 4);
  while (tile + 2 * R.halo > maxDim && tile > 64) tile = std::max(64, (tile / 2) / 4 * 4);
  if (tile + 2 * R.halo > maxDim) return d.fail(Failure::Unsupported, err, "device image limit too small for the halo");

  Impl::Lut* L = d.ensureLut(r, lut, err);
  if (!L) return false;

  // the source: resident (small enough) or streamed per tile
  const bool resident = !eo.forceStream && d.canResident(d.W, d.H) && d.canResident(rw, rh) && std::uint64_t(d.W) * d.H <= 40000000ull;
  hal::ImageH* S = nullptr;
  if (resident) {
    if (!d.ensureResident(err)) return false;
    S = d.resizedSource(rw, rh, err);
    if (!S) return false;
  }

  const bool wantDz = plan.dehaze > 0, wantClar = plan.clarity != 0;
  const int glw = cdiv(plan.outW, 4), glh = cdiv(plan.outH, 4);
  hal::ImageH gColor, gDc, gYy, gAb;
  struct Free {
    hal::Device& dev;
    hal::ImageH* im[4];
    ~Free() {
      dev.waitIdle();
      for (hal::ImageH* i : im) dev.destroy(*i);
    }
  } freer{dev, {&gColor, &gDc, &gYy, &gAb}};
  auto mk = [&](hal::ImageH& im, hal::Format f) {
    const hal::Status s = dev.createImage({f, std::uint32_t(glw), std::uint32_t(glh), 1}, &im);
    return s == hal::Status::Ok || (d.failStatus(s, err, ""), false);
  };
  if (wantDz && (!mk(gColor, hal::Format::RGBA32F) || !mk(gDc, hal::Format::R32F))) return false;
  if (wantClar && (!mk(gYy, hal::Format::RGBA32F) || !mk(gAb, hal::Format::RGBA32F))) return false;

  std::vector<WRect> tiles;
  for (int y = 0; y < plan.outH; y += tile)
    for (int x = 0; x < plan.outW; x += tile) tiles.push_back({x, y, std::min(plan.outW, x + tile), std::min(plan.outH, y + tile)});

  struct Pending {
    int slot = -1;
    hal::FenceH fence;
    WRect interior;
  };
  Pending pend;
  int passes = 0, barriers = 0;
  auto flush = [&]() -> bool {
    if (pend.slot < 0) return true;
    const hal::Status ws = dev.wait(pend.fence, 30ull * 1000 * 1000 * 1000);
    if (ws != hal::Status::Ok) return d.failStatus(ws, err, "");
    const bool ok = eo.sink(pend.interior.x0, pend.interior.y0, pend.interior.w(), pend.interior.h(),
                            static_cast<const std::uint8_t*>(d.core().slot(pend.slot).readback.mapped), std::size_t(pend.interior.w()) * 4);
    pend.slot = -1;
    dev.pinSlot(-1);
    if (!ok) err = "export aborted by the sink", d.failure = Failure::Unsupported;
    return ok;
  };

  auto runTile = [&](const WRect& in, Mode mode, bool readback) -> bool {
    WRect work{std::max(0, in.x0 - R.halo), std::max(0, in.y0 - R.halo), std::min(plan.outW, in.x1 + R.halo), std::min(plan.outH, in.y1 + R.halo)};
    // 1. the source window of a streamed export is uploaded first (the upload takes frame slots of its own)
    hal::ImageH acquired;
    bool haveAcq = false;
    const WRect fp = footprint(plan, work);
    SrcWin win{fp.x0, fp.y0, fp.x1, fp.y1};
    if (!resident) {
      if (down) win = lanczosWindow(d.W, d.H, rw, rh, fp);
      const hal::Status as = pool.acquire(hal::Format::RGBA16F, std::uint32_t(win.w()), std::uint32_t(win.h()), &acquired);
      if (as != hal::Status::Ok) return d.failStatus(as, err, "");
      haveAcq = true;
      const float* base = d.cpu->linear.data.data() + (std::size_t(win.y0) * d.W + win.x0) * 3;
      if (!d.core().uploadRgbF32(base, std::size_t(d.W) * 3, win.w(), win.h(), acquired, 0, 0, err)) {
        pool.release(acquired);
        return d.fail(dev.state() == hal::DeviceState::Lost ? Failure::DeviceLost : Failure::Transient, err, err);
      }
    }
    auto cl = dev.begin();
    if (!cl) {
      if (haveAcq) pool.release(acquired);
      return d.failStatus(hal::Status::Timeout, err, "");
    }
    const int slot = cl->slot();
    GpuGraph g(dev, pool);
    GpuRecorder rec(dev, g, err);
    rec.weights = d.core().slot(slot).weights;
    rec.boxScanMin = d.core().boxScanOk ? -1 : 0;
    rec.gBuf = d.core().gBuf;
    rec.partBuf = d.core().partBuf;
    rec.allocW = int(pool.bucket(std::uint32_t(std::min(plan.outW, tile + 2 * R.halo))));
    rec.allocH = int(pool.bucket(std::uint32_t(std::min(plan.outH, tile + 2 * R.halo))));
    FrameJob job;
    job.plan = &plan;
    job.resizeSharpen = rsharp;
    job.mode = mode;
    job.interior = in;
    job.work = work;
    job.lut = d.lutBind(g, *L);
    GlobalRes gl;
    gl.lw = glw;
    gl.lh = glh;
    if (wantDz) {
      gl.color = g.external(gColor);
      gl.dc = g.external(gDc);
    }
    if (wantClar) {
      gl.yy = g.external(gYy);
      gl.ab = g.external(gAb);
    }
    job.glob = &gl;
    bool ok = true;
    if (resident) {
      job.src.img = g.external(*S);
    } else {
      const H a = g.external(acquired);
      if (down) {
        const H s2 = recordResize(rec, a, win, d.W, d.H, rw, rh, fp);
        if (s2 < 0) ok = false;
        job.src.img = s2;
        job.src.ox = fp.x0;
        job.src.oy = fp.y0;
      } else {
        job.src.img = a;
        job.src.ox = win.x0;
        job.src.oy = win.y0;
      }
    }
    H out8 = -1;
    if (ok) ok = buildFrame(rec, job, &out8);
    if (ok && readback) {
      const std::size_t bytes = std::size_t(in.w()) * in.h() * 4;
      if (!d.core().ensureReadback(slot, bytes, err)) ok = false;
      else g.copyImageToBuffer(out8, d.core().slot(slot).readback, std::uint32_t(in.x0 - work.x0), std::uint32_t(in.y0 - work.y0), std::uint32_t(in.w()), std::uint32_t(in.h()));
    }
    if (!ok) {
      if (haveAcq) pool.release(acquired);
      return d.fail(Failure::Transient, err, err);
    }
    hal::FenceH fence;
    const hal::Status s = runGraph(dev, g, *cl, &fence, err);
    if (haveAcq) pool.release(acquired);
    if (s != hal::Status::Ok) return d.failStatus(s, err, err);
    {
      const auto st = dev.stats(fence);
      passes += st.passes;
      barriers += st.barriers;
    }
    if (readback) {
      if (!flush()) return false;  // previous tile: its GPU work ran while this one was recorded
      pend.slot = slot;
      pend.fence = fence;
      pend.interior = in;
      dev.pinSlot(slot);
    }
    return true;
  };

  auto globalPass = [&](bool dehaze) -> bool {
    auto cl = dev.begin();
    if (!cl) return d.failStatus(hal::Status::Timeout, err, "");
    const int slot = cl->slot();
    GpuGraph g(dev, pool);
    GpuRecorder rec(dev, g, err);
    rec.weights = d.core().slot(slot).weights;
    rec.boxScanMin = d.core().boxScanOk ? -1 : 0;
    rec.gBuf = d.core().gBuf;
    rec.partBuf = d.core().partBuf;
    FrameJob job;
    job.plan = &plan;
    job.resizeSharpen = rsharp;
    GlobalRes gl;
    gl.lw = glw;
    gl.lh = glh;
    gl.color = wantDz ? g.external(gColor) : -1;
    gl.dc = wantDz ? g.external(gDc) : -1;
    gl.yy = wantClar ? g.external(gYy) : -1;
    gl.ab = wantClar ? g.external(gAb) : -1;
    job.glob = &gl;
    job.work = WRect{0, 0, glw, glh};
    if (!buildGlobalAnalysis(rec, job, dehaze)) return d.fail(Failure::Transient, err, err);
    const hal::Status s = runGraph(dev, g, *cl, nullptr, err);
    return s == hal::Status::Ok || d.failStatus(s, err, err);
  };

  if (wantDz) {
    for (const WRect& t : tiles)
      if (!runTile(t, Mode::AnalyzeDehaze, false)) return false;
    if (!globalPass(true)) return false;
  }
  if (wantClar) {
    for (const WRect& t : tiles)
      if (!runTile(t, Mode::AnalyzeClarity, false)) return false;
    if (!globalPass(false)) return false;
  }
  for (const WRect& t : tiles)
    if (!runTile(t, Mode::Final, true)) return false;
  if (!flush()) return false;
  if (dev.waitAll() != hal::Status::Ok) return d.failStatus(hal::Status::Timeout, err, "");
  if (pool.freeBytes() > dev.poolBudget()) pool.trim(dev.poolBudget());
  if (stats) {
    stats->wallMs = msSince(t0);
    stats->gpuMs = dev.gpuAccumMs() - gpu0;
    stats->passes = passes;
    stats->barriers = barriers;
  }
  return true;
}

// ============================================================================================ self-test ===
namespace {

bool runSelfTest(Backend& b, std::string& report) {
  // A known pattern through the whole pipeline against the CPU reference: point stages (geometry, lattice incl. the
  // overflow lattice, vignette) and every spatial stage. The thresholds are loose enough for fp16 working images and
  // different float rounding, and tight enough to catch a broken driver (wrong results, garbage, zeros).
  const SourceImage src = makeTestImage(192, 128);
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
  std::string err;
  auto sess = b.createSession();
  if (!sess->setSource(src, 1, err)) return report = err, false;
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
    int w = 0, h = 0, gw = 0, gh = 0;
    std::uint8_t* cpu = r.renderBakedRgba(src, *lut, fo, w, h);
    std::uint8_t* gpu = sess->render(r, *lut, fo, FrameOptions{}, gw, gh, nullptr, err);
    if (!cpu || !gpu || w != gw || h != gh) {
      std::free(cpu);
      std::free(gpu);
      report += std::string(report.empty() ? "" : "; ") + c.name + ": " + (err.empty() ? "size mismatch" : err);
      return false;
    }
    const DeStats s = compareRgba8(gpu, cpu, std::size_t(w) * h);
    std::free(cpu);
    std::free(gpu);
    const bool pass = s.mean <= 1.0 && s.p99 <= 5.0 && s.max <= 25.0;
    std::snprintf(line, sizeof line, "%s%s: mean dE %.3f p99 %.2f max %.2f %s", report.empty() ? "" : "; ", c.name, s.mean, s.p99, s.max, pass ? "ok" : "FAIL");
    report += line;
    ok &= pass;
  }
  return ok;
}

}  // namespace

// ============================================================================================ runtime flag ===
namespace {
std::atomic<int> g_override{-1};
std::once_flag g_once;
// Leaked on purpose: tearing the device down from a static destructor runs after the Vulkan loader / driver have been
// unloaded (crashes on Android); the OS reclaims everything at process exit.
std::shared_ptr<Backend>& backendRef() {
  static auto* b = new std::shared_ptr<Backend>;
  return *b;
}
std::string g_why;
std::mutex g_mtx;

bool envEnabled() {
  if (const char* v = std::getenv("DAPPLE_GPU")) return v[0] == '1';
#if defined(__ANDROID__)
  char buf[PROP_VALUE_MAX] = {0};
  if (__system_property_get("debug.dapple.gpu", buf) > 0) return buf[0] == '1';
#endif
  return false;
}
}  // namespace

bool runtimeEnabled() {
  const int o = g_override.load();
  return o >= 0 ? o != 0 : envEnabled();
}
void setRuntimeEnabled(int enabled) { g_override.store(enabled); }

std::shared_ptr<Backend> sharedBackend(std::string* why) {
  if (!runtimeEnabled()) {
    if (why) *why = "GPU backend disabled";
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(g_mtx);
  std::call_once(g_once, [] {
    BackendOptions o;
    if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) o.allowSoftware = v[0] == '1';
    if (const char* v = std::getenv("DAPPLE_GPU_PIPELINE_CACHE")) o.pipelineCachePath = v;  // persisted VkPipelineCache
    backendRef() = Backend::create(o, g_why);
  });
  if (!backendRef() && why) *why = g_why;
  return backendRef();
}

}  // namespace dapple::gpu
