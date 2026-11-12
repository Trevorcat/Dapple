// Backend + Session: resident sources, lattice cache, single-frame rendering, tiled export, self-test, runtime flag.
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
#include "frame.h"
#include "parity.h"
#include "vk_core.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif


#if defined(__ANDROID__)
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

namespace dapple::gpu {
using namespace vk;
using Hd = vk::H;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

constexpr VkFormat kF16 = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kF32 = VK_FORMAT_R32G32B32A32_SFLOAT;
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

struct ResizeU {
  std::int32_t src[4], dst[4], out_[4];
  float sc[4];
};

int cdiv(int a, int b) { return (a + b - 1) / b; }

// ---- Lanczos resize planning ----
struct Win {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};

// Source window (clamped, exclusive end) the Lanczos-3 filter reads to produce `dst` of a srcW x srcH -> dstW x dstH resize.
Win lanczosWindow(int srcW, int srcH, int dstW, int dstH, const Rect& dst) {
  Win w;
  auto axis = [](int srcLen, int dstLen, int d0, int d1, int& lo, int& hi) {
    const float s = float(srcLen) / float(dstLen), S = std::max(1.f, s);
    lo = int(std::floor((float(d0) + 0.5f) * s - 0.5f - 3.f * S)) - 1;
    hi = int(std::floor((float(d1) - 0.5f) * s - 0.5f + 3.f * S)) + 3;
    lo = std::max(0, lo);
    hi = std::min(srcLen, hi);
  };
  axis(srcW, dstW, dst.x0, dst.x1, w.x0, w.x1);
  axis(srcH, dstH, dst.y0, dst.y1, w.y0, w.y1);
  return w;
}

// Adds the two Lanczos passes producing rect `dst` of the resized picture from the source window `win` held in `src`.
Hd appendResize(Engine& e, Graph& g, H src, const Win& win, int srcW, int srcH, int dstW, int dstH, const Rect& dst, std::string& err) {
  Pipeline* ph = e.pipeline("resize", {{0, 0}}, err);
  Pipeline* pv = e.pipeline("resize", {{0, 1}}, err);
  if (!ph || !pv) return -1;
  const float sx = float(srcW) / float(dstW), sy = float(srcH) / float(dstH);
  const int bw = dst.w(), bh = dst.h();
  const Hd tmp = g.transient(kF16, std::uint32_t(bw), std::uint32_t(win.h()));
  const Hd out = g.transient(kF16, std::uint32_t(bw), std::uint32_t(bh));
  ResizeU u{};
  u.src[0] = srcW;
  u.src[1] = srcH;
  u.src[2] = win.x0;
  u.src[3] = win.y0;
  u.dst[0] = dstW;
  u.dst[1] = dstH;
  u.dst[2] = dst.x0;
  u.dst[3] = dst.y0;
  u.out_[0] = bw;
  u.out_[1] = win.h();
  u.out_[2] = win.y0;
  u.sc[0] = sx;
  u.sc[1] = std::max(1.f, sx);
  g.dispatch("resize_h", ph, {Graph::R(src), Graph::W(tmp)}, std::uint32_t(cdiv(bw, 16)), std::uint32_t(cdiv(win.h(), 16)), &u, sizeof u);
  ResizeU v = u;
  v.src[2] = 0;
  v.out_[0] = bw;
  v.out_[1] = bh;
  v.out_[2] = 0;
  v.sc[0] = sy;
  v.sc[1] = std::max(1.f, sy);
  g.dispatch("resize_v", pv, {Graph::R(tmp), Graph::W(out)}, std::uint32_t(cdiv(bw, 16)), std::uint32_t(cdiv(bh, 16)), &v, sizeof v);
  return out;
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
Rect footprint(const FramePlan& P, const Rect& work) {
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
  Rect r;
  r.x0 = std::max(0, int(std::floor(x0)) - m);
  r.y0 = std::max(0, int(std::floor(y0)) - m);
  r.x1 = std::min(P.srcW, int(std::ceil(x1)) + m);
  r.y1 = std::min(P.srcH, int(std::ceil(y1)) + m);
  return r;
}

}  // namespace

// ============================================================================================ Backend ===
struct Backend::Impl {
  std::unique_ptr<Engine> eng;
  BackendOptions opt;
  std::string selfTest;
  Buffer gBuf, partBuf;  // dehaze statistics + reduction partials
  std::mutex mtx;
  ~Impl() {
    if (eng) {
      eng->waitIdle();
      eng->destroy(gBuf);
      eng->destroy(partBuf);
    }
  }
};

namespace {
bool runSelfTest(Backend& b, std::string& report);
}

Backend::Backend() : d_(new Impl) {}
Backend::~Backend() = default;

std::shared_ptr<Backend> Backend::create(const BackendOptions& opt, std::string& err) {
  std::shared_ptr<Backend> b(new Backend);
  b->d_->opt = opt;
  b->d_->eng = Engine::create(opt, err);
  if (!b->d_->eng) return nullptr;
  Engine& e = *b->d_->eng;
  if (!e.createBuffer(b->d_->gBuf, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Mem::DeviceLocal, err) ||
      !e.createBuffer(b->d_->partBuf, 256 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Mem::DeviceLocal, err))
    return nullptr;
  if (opt.selfTest) {
    std::string report;
    const bool ok = runSelfTest(*b, report);
    b->d_->selfTest = report;
    if (!ok) {
      err = "GPU self-test failed on " + e.info().name + " (" + e.info().key() + "): " + report;
      return nullptr;
    }
  }
  return b;
}

const DeviceInfo& Backend::info() const { return d_->eng->info(); }
const std::string& Backend::selfTestReport() const { return d_->selfTest; }
std::size_t Backend::pooledBytes() const { return d_->eng->pooledBytes(); }
std::unique_ptr<Session> Backend::createSession() { return std::unique_ptr<Session>(new Session(shared_from_this())); }

std::string DeviceInfo::text() const {
  char b[1024];
  std::snprintf(b, sizeof b,
                "%s (vendor 0x%04x, type %u, driver %s)\n  Vulkan %u.%u.%u  maxImage2D %u  maxImage3D %u  shared %u KB  "
                "maxInvocations %u\n  subgroup size %u ops 0x%x  fp16 arith %d  storage16 %d  linear-filter RGBA32F %d  "
                "timestamps %d  device-local heap %.0f MB  unified memory %d",
                name.c_str(), vendorId, deviceType, driver.c_str(), VK_VERSION_MAJOR(apiVersion), VK_VERSION_MINOR(apiVersion),
                VK_VERSION_PATCH(apiVersion), maxImageDim2D, maxImageDim3D, maxSharedMem / 1024, maxInvocations, subgroupSize,
                subgroupOps, int(fp16Arith), int(storage16), int(linearFilterRgba32f), int(timestamps),
                double(deviceLocalBytes) / 1048576.0, int(unifiedMemory));
  return b;
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
  Engine& eng() { return *be->d_->eng; }

  const SourceImage* cpu = nullptr;
  std::uint64_t stamp = 0;
  int W = 0, H = 0;
  Image resident;  // the source as RGBA16F, uploaded on first use
  bool haveResident = false;

  struct Resized {
    int w = 0, h = 0;
    Image img;
    std::uint64_t used = 0;
  };
  std::vector<std::unique_ptr<Resized>> resized;

  struct Lut {
    std::string key;
    Image lat, lat2;
    Buffer tone;
    int n = 0, n2 = 0;
    std::uint64_t used = 0;
  };
  std::unique_ptr<Lut> luts[2];
  std::uint64_t tick = 0;

  void dropResident() {
    if (haveResident) eng().destroy(resident);
    haveResident = false;
    for (auto& r : resized) eng().destroy(r->img);
    resized.clear();
  }
  ~Impl() {
    if (!be) return;
    eng().waitIdle();
    dropResident();
    for (auto& l : luts)
      if (l) {
        eng().destroy(l->lat);
        eng().destroy(l->lat2);
        eng().destroy(l->tone);
      }
  }

  std::uint64_t residentLimit() const {
    const auto& i = be->d_->eng->info();
    return std::min<std::uint64_t>(std::uint64_t(double(i.deviceLocalBytes) * 0.35), 1536ull << 20);
  }
  bool canResident(int w, int h) const {
    const auto& i = be->d_->eng->info();
    return w > 0 && h > 0 && std::uint32_t(w) <= i.maxImageDim2D && std::uint32_t(h) <= i.maxImageDim2D && std::uint64_t(w) * h * 8 <= residentLimit();
  }

  bool ensureResident(std::string& err) {
    if (haveResident) return true;
    if (!cpu) return err = "no source", false;
    if (!canResident(W, H)) return err = "source too large for a resident GPU copy", false;
    if (!eng().createImage(resident, kF16, std::uint32_t(W), std::uint32_t(H), 1, err)) return false;
    if (!eng().uploadRgbF32(cpu->linear.data.data(), std::size_t(W) * 3, W, H, resident, 0, 0, err)) {
      eng().destroy(resident);
      return false;
    }
    haveResident = true;
    return true;
  }

  // The resident source resized (Lanczos-3) to w x h; cached (two sizes).
  Image* resizedSource(int w, int h, std::string& err) {
    if (w == W && h == H) return &resident;
    for (auto& r : resized)
      if (r->w == w && r->h == h) {
        r->used = ++tick;
        return &r->img;
      }
    if (!canResident(w, h)) return err = "resized source too large", nullptr;
    auto r = std::make_unique<Resized>();
    r->w = w;
    r->h = h;
    if (!eng().createImage(r->img, kF16, std::uint32_t(w), std::uint32_t(h), 1, err)) return nullptr;
    FrameSlot* s = eng().acquireSlot(err);
    if (!s) return nullptr;
    Graph g(eng());
    const Hd src = g.external(&resident);
    const Win full{0, 0, W, H};
    const Hd out = appendResize(eng(), g, src, full, W, H, w, h, Rect{0, 0, w, h}, err);
    if (out < 0) return nullptr;
    g.copyImage(out, g.external(&r->img), std::uint32_t(w), std::uint32_t(h));
    if (!eng().submit(g, *s, err) || !eng().wait(*s, err)) return nullptr;
    r->used = ++tick;
    if (resized.size() >= 2) {
      auto victim = std::min_element(resized.begin(), resized.end(), [](auto& a, auto& b) { return a->used < b->used; });
      eng().destroy((*victim)->img);
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
    if (lut.n < 2 || lut.rgb.size() != std::size_t(lut.n) * lut.n * lut.n * 3) return err = "lattice not usable (film only bake?)", nullptr;
    if (lut.tone.size() != kToneN) return err = "tone table size differs from the GPU backend's", nullptr;
    std::vector<float> over = r.bakeOverflow(*cpu, lut, kLat2N, kLat2Max);
    if (over.size() != std::size_t(kLat2N) * kLat2N * kLat2N * 3) return err = "overflow lattice unavailable", nullptr;
    int slotIdx = 0;
    if (luts[0] && (!luts[1] || luts[0]->used > luts[1]->used)) slotIdx = 1;
    if (luts[slotIdx]) {
      eng().waitIdle();
      eng().destroy(luts[slotIdx]->lat);
      eng().destroy(luts[slotIdx]->lat2);
      eng().destroy(luts[slotIdx]->tone);
    }
    auto l = std::make_unique<Lut>();
    l->key = lut.key;
    l->n = lut.n;
    l->n2 = kLat2N;
    if (!eng().createImage(l->lat, kF32, std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n), err) ||
        !eng().createImage(l->lat2, kF32, kLat2N, kLat2N, kLat2N, err) ||
        !eng().createBuffer(l->tone, kToneN * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Mem::DeviceLocal, err))
      return nullptr;
    const std::size_t latBytes = std::size_t(lut.n) * lut.n * lut.n * 16, lat2Bytes = std::size_t(kLat2N) * kLat2N * kLat2N * 16;
    const std::size_t toneBytes = (kToneN * 4 + 255) / 256 * 256;
    FrameSlot* s = eng().acquireSlot(err);
    if (!s || !eng().ensureStaging(*s, toneBytes + latBytes + lat2Bytes, err)) return nullptr;
    auto* base = static_cast<std::uint8_t*>(s->staging.mapped);
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
    Graph g(eng());
    g.copyBufferToBuffer(&s->staging, g.externalBuf(&l->tone), kToneN * 4, 0);
    g.copyBufferToImage(&s->staging, g.external(&l->lat), std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n), toneBytes);
    g.copyBufferToImage(&s->staging, g.external(&l->lat2), kLat2N, kLat2N, kLat2N, toneBytes + latBytes);
    if (!eng().submit(g, *s, err) || !eng().wait(*s, err)) return nullptr;
    l->used = ++tick;
    luts[slotIdx] = std::move(l);
    return luts[slotIdx].get();
  }

  static LutBind lutBind(Graph& g, Lut& l) {
    LutBind b;
    b.tone = g.externalBuf(&l.tone);
    b.lat = g.external(&l.lat);
    b.lat2 = g.external(&l.lat2);
    b.n = l.n;
    b.n2 = l.n2;
    b.toneN = int(kToneN);
    b.toneBase = kToneBase;
    b.max1 = kBakeMax;
    b.max2 = kLat2Max;
    return b;
  }

  void finishFrame(FrameStats* st, const FrameSlot& s, double wall) {
    if (!st) return;
    st->wallMs = wall;
    st->gpuMs = s.gpuMs;
    st->passes = s.passes;
    st->barriers = s.barriers;
    st->passTimes = s.times;
  }
};

Session::Session(std::shared_ptr<Backend> b) : d_(new Impl) { d_->be = std::move(b); }
Session::~Session() {
  if (d_ && d_->be) {
    const std::shared_ptr<Backend> be = d_->be;  // keeps the backend (and its mutex) alive while the session is torn down
    std::lock_guard<std::mutex> lk(be->d_->mtx);
    d_.reset();
  }
}

bool Session::setSource(const SourceImage& src, std::uint64_t stamp, std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
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

std::uint8_t* Session::render(const Renderer& r, const BakedLut& lut, const FastOptions& opt, const FrameOptions& fo, int& w, int& h,
                              FrameStats* stats, std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
  BigCoreScope pin;
  const auto t0 = Clock::now();
  Impl& d = *d_;
  Engine& e = d.eng();
  if (!d.cpu) return err = "no source", nullptr;
  int rw, rh;
  resizedDims(d.W, d.H, fo.srcLongEdge, rw, rh);
  const FramePlan plan = r.framePlan(rw, rh, d.cpu->sceneReferred, opt);
  if (!plan.supported) return err = "unsupported on the GPU: " + plan.why, nullptr;
  if (plan.outW <= 0 || plan.outH <= 0 || std::uint32_t(plan.outW) > e.info().maxImageDim2D || std::uint32_t(plan.outH) > e.info().maxImageDim2D)
    return err = "frame size outside the device limits", nullptr;
  if (!d.ensureResident(err)) return nullptr;
  Image* S = d.resizedSource(rw, rh, err);
  if (!S) return nullptr;
  Impl::Lut* L = d.ensureLut(r, lut, err);
  if (!L) return nullptr;
  const double uploadMs = msSince(t0);

  FrameSlot* slot = e.acquireSlot(err);
  if (!slot) return nullptr;
  const std::size_t bytes = std::size_t(plan.outW) * plan.outH * 4;
  if (!e.ensureReadback(*slot, bytes, err)) return nullptr;
  Graph g(e);
  FrameJob job;
  job.plan = &plan;
  job.resizeSharpen = (rw < d.W || rh < d.H) ? fo.resizeSharpen : 0.f;
  job.mode = Mode::Whole;
  job.interior = job.work = Rect{0, 0, plan.outW, plan.outH};
  job.src.img = g.external(S);
  job.lut = Impl::lutBind(g, *L);
  job.gBuf = &d.bi().gBuf;
  job.partBuf = &d.bi().partBuf;
  job.weights = &slot->weights;
  Hd out8 = -1;
  if (!buildFrame(e, g, job, &out8, err)) return nullptr;

  g.copyImageToBuffer(out8, &slot->readback, 0, 0, std::uint32_t(plan.outW), std::uint32_t(plan.outH));
  const auto tSub = Clock::now();
  if (!e.submit(g, *slot, err)) return nullptr;
  const auto tWait = Clock::now();
  if (!e.wait(*slot, err)) return nullptr;
  const auto t1 = Clock::now();
  auto* out = static_cast<std::uint8_t*>(std::malloc(bytes));
  if (!out) return err = "out of memory", nullptr;
  std::memcpy(out, slot->readback.mapped, bytes);
  const double copyMs = msSince(t1);
  w = plan.outW;
  h = plan.outH;
  const auto t2 = Clock::now();
  if (e.freeBytes() > e.poolBudget()) e.trimPool(e.poolBudget());
  const double trimMs = msSince(t2);
  d.finishFrame(stats, *slot, msSince(t0));
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
bool Session::exportTiled(const Renderer& r, const SourceImage& src, const BakedLut& lut, const FastOptions& opt, const ExportOptions& eo,
                          FrameStats* stats, std::string& err) {
  std::lock_guard<std::mutex> lk(d_->bi().mtx);
  const auto t0 = Clock::now();
  Impl& d = *d_;
  Engine& e = d.eng();
  const double gpu0 = e.gpuAccum();
  if (!eo.sink) return err = "no tile sink", false;
  if (!d.cpu || d.cpu != &src) {
    d.dropResident();
    d.cpu = &src;
    d.W = src.linear.width;
    d.H = src.linear.height;
  }
  int rw, rh;
  resizedDims(d.W, d.H, eo.frame.srcLongEdge, rw, rh);
  if (opt.region.active()) return err = "tiled export renders the whole picture", false;
  const FramePlan plan = r.framePlan(rw, rh, src.sceneReferred, opt);
  if (!plan.supported) return err = "unsupported on the GPU: " + plan.why, false;
  const bool down = rw < d.W || rh < d.H;
  const float rsharp = down ? eo.frame.resizeSharpen : 0.f;
  const SpatialRadii R = spatialRadii(plan, rsharp);
  const int maxDim = int(e.info().maxImageDim2D);
  int tile = std::max(64, eo.tile / 4 * 4);
  while (tile + 2 * R.halo > maxDim && tile > 64) tile = std::max(64, (tile / 2) / 4 * 4);
  if (tile + 2 * R.halo > maxDim) return err = "device image limit too small for the halo", false;

  Impl::Lut* L = d.ensureLut(r, lut, err);
  if (!L) return false;

  // the source: resident (small enough) or streamed per tile
  const bool resident = !eo.forceStream && d.canResident(d.W, d.H) && d.canResident(rw, rh) && std::uint64_t(d.W) * d.H <= 40000000ull;
  Image* S = nullptr;
  if (resident) {
    if (!d.ensureResident(err)) return false;
    S = d.resizedSource(rw, rh, err);
    if (!S) return false;
  }

  const bool wantDz = plan.dehaze > 0, wantClar = plan.clarity != 0;
  GlobalRes glob;
  glob.lw = cdiv(plan.outW, 4);
  glob.lh = cdiv(plan.outH, 4);
  Image gColor, gDc, gYy, gAb;
  struct Free {
    Engine& e;
    Image* im[4];
    ~Free() {
      e.waitIdle();
      for (Image* i : im) e.destroy(*i);
    }
  } freer{e, {&gColor, &gDc, &gYy, &gAb}};
  auto mk = [&](Image& im, VkFormat f) { return e.createImage(im, f, std::uint32_t(glob.lw), std::uint32_t(glob.lh), 1, err); };
  if (wantDz && (!mk(gColor, kF32) || !mk(gDc, VK_FORMAT_R32_SFLOAT))) return false;
  if (wantClar && (!mk(gYy, kF32) || !mk(gAb, kF32))) return false;

  std::vector<Rect> tiles;
  for (int y = 0; y < plan.outH; y += tile)
    for (int x = 0; x < plan.outW; x += tile) tiles.push_back({x, y, std::min(plan.outW, x + tile), std::min(plan.outH, y + tile)});

  // one job on a slot; `release` = pooled source windows to give back after the submit
  struct Pending {
    FrameSlot* slot = nullptr;
    Rect interior;
  };
  Pending pend;
  int passes = 0, barriers = 0;
  double gpuMs = 0;
  auto flush = [&]() -> bool {
    if (!pend.slot) return true;
    if (!e.wait(*pend.slot, err)) return false;
    gpuMs += pend.slot->gpuMs;
    const bool ok = eo.sink(pend.interior.x0, pend.interior.y0, pend.interior.w(), pend.interior.h(), static_cast<const std::uint8_t*>(pend.slot->readback.mapped),
                            std::size_t(pend.interior.w()) * 4);
    pend.slot = nullptr;
    e.pinSlot(nullptr);
    if (!ok) err = "export aborted by the sink";
    return ok;
  };

  auto runTile = [&](const Rect& in, Mode mode, bool readback) -> bool {
    Rect work{std::max(0, in.x0 - R.halo), std::max(0, in.y0 - R.halo), std::min(plan.outW, in.x1 + R.halo), std::min(plan.outH, in.y1 + R.halo)};
    // 1. the source window of a streamed export is uploaded first (the upload takes frame slots of its own)
    Image* acquired = nullptr;
    const Rect fp = footprint(plan, work);
    Win win{fp.x0, fp.y0, fp.x1, fp.y1};
    if (!resident) {
      if (down) win = lanczosWindow(d.W, d.H, rw, rh, fp);
      acquired = e.acquire(kF16, std::uint32_t(win.w()), std::uint32_t(win.h()), err);
      if (!acquired) return false;
      const float* base = d.cpu->linear.data.data() + (std::size_t(win.y0) * d.W + win.x0) * 3;
      if (!e.uploadRgbF32(base, std::size_t(d.W) * 3, win.w(), win.h(), *acquired, 0, 0, err)) return false;
    }
    FrameSlot* slot = e.acquireSlot(err);
    if (!slot) return false;
    Graph g(e);
    FrameJob job;
    job.plan = &plan;
    job.resizeSharpen = rsharp;
    job.mode = mode;
    job.interior = in;
    job.work = work;
    job.allocW = e.bucket(std::uint32_t(std::min(plan.outW, tile + 2 * R.halo)));
    job.allocH = e.bucket(std::uint32_t(std::min(plan.outH, tile + 2 * R.halo)));
    job.lut = Impl::lutBind(g, *L);
    job.gBuf = &d.bi().gBuf;
    job.partBuf = &d.bi().partBuf;
    job.weights = &slot->weights;
    GlobalRes gl = glob;
    if (wantDz) {
      gl.color = g.external(&gColor);
      gl.dc = g.external(&gDc);
    }
    if (wantClar) {
      gl.yy = g.external(&gYy);
      gl.ab = g.external(&gAb);
    }
    job.glob = &gl;
    if (resident) {
      job.src.img = g.external(S);
    } else {
      const Hd a = g.external(acquired);
      if (down) {
        const Hd s2 = appendResize(e, g, a, win, d.W, d.H, rw, rh, fp, err);
        if (s2 < 0) return false;
        job.src.img = s2;
        job.src.ox = fp.x0;
        job.src.oy = fp.y0;
      } else {
        job.src.img = a;
        job.src.ox = win.x0;
        job.src.oy = win.y0;
      }
    }
    Hd out8 = -1;
    if (!buildFrame(e, g, job, &out8, err)) return false;
    if (readback) {
      const std::size_t bytes = std::size_t(in.w()) * in.h() * 4;
      if (!e.ensureReadback(*slot, bytes, err)) return false;
      g.copyImageToBuffer(out8, &slot->readback, std::uint32_t(in.x0 - work.x0), std::uint32_t(in.y0 - work.y0), std::uint32_t(in.w()), std::uint32_t(in.h()));
    }
    const bool ok = e.submit(g, *slot, err);
    if (acquired) e.release(acquired);
    if (!ok) return false;
    passes += slot->passes;
    barriers += slot->barriers;
    if (readback) {
      if (!flush()) return false;  // previous tile: its GPU work ran while this one was recorded
      pend.slot = slot;
      pend.interior = in;
      e.pinSlot(slot);
    }
    return true;
  };

  auto globalPass = [&](bool dehaze) -> bool {
    FrameSlot* slot = e.acquireSlot(err);
    if (!slot) return false;
    Graph g(e);
    FrameJob job;
    job.plan = &plan;
    job.weights = &slot->weights;
    job.gBuf = &d.bi().gBuf;
    job.partBuf = &d.bi().partBuf;
    GlobalRes gl = glob;
    gl.color = wantDz ? g.external(&gColor) : -1;
    gl.dc = wantDz ? g.external(&gDc) : -1;
    gl.yy = wantClar ? g.external(&gYy) : -1;
    gl.ab = wantClar ? g.external(&gAb) : -1;
    job.glob = &gl;
    job.work = Rect{0, 0, glob.lw, glob.lh};
    if (!buildGlobalAnalysis(e, g, job, dehaze, err)) return false;
    return e.submit(g, *slot, err);
  };

  if (wantDz) {
    for (const Rect& t : tiles)
      if (!runTile(t, Mode::AnalyzeDehaze, false)) return false;
    if (!globalPass(true)) return false;
  }
  if (wantClar) {
    for (const Rect& t : tiles)
      if (!runTile(t, Mode::AnalyzeClarity, false)) return false;
    if (!globalPass(false)) return false;
  }
  for (const Rect& t : tiles)
    if (!runTile(t, Mode::Final, true)) return false;
  if (!flush() || !e.waitAll(err)) return false;
  if (e.freeBytes() > e.poolBudget()) e.trimPool(e.poolBudget());
  (void)gpuMs;
  if (stats) {
    stats->wallMs = msSince(t0);
    stats->gpuMs = e.gpuAccum() - gpu0;
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
