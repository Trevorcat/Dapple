// GpuExecutor: Core (device, pool, slot buffers) and Executor (IGpuExecutor): walks the stages of a Plan and appends the passes
// of its nodes through gpuImplFor(type)->recordPorts, keeps the resident source (fed by a SourceStore) and the device tables
// (on a ResultCache). Whole-frame runs record every stage into one command list; a tiled export records one job per tile and
// sweep (docs/engine/pipeline-architecture.md 5.1 - 5.5).
#include "engine/exec/gpu_executor.h"
#include "determinism.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "engine/nodes/film/film_async.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "engine/exec/big_core.h"
#include "engine/exec/result_cache.h"
#include "dapple/film.h"
#include "dapple/lut.h"
#include "engine/nodes/film/film_gpu.h"
#include "engine/nodes/film/film_node_internal.h"
#include "engine/nodes/gpu_common.h"
#include "engine/nodes/gpu_nodes.h"

namespace dapple::engine::gpu {

// ============================================================================================ Core ===
std::unique_ptr<Core> Core::create(const hal::DeviceOptions& opt, std::string& err) {
  std::unique_ptr<Core> c(new Core);
#if defined(DAPPLE_HAL_METAL)
  c->own_ = hal::createDevice(hal::Api::Metal, opt, err);
#else
  c->own_ = hal::createDevice(hal::Api::Vulkan, opt, err);
#endif
  if (!c->own_) return nullptr;
  c->dev_ = c->own_.get();
  return c->init(err) ? std::move(c) : nullptr;
}
std::unique_ptr<Core> Core::adopt(std::unique_ptr<hal::Device> dev, std::string& err) {
  std::unique_ptr<Core> c(new Core);
  c->own_ = std::move(dev);
  c->dev_ = c->own_.get();
  return c->init(err) ? std::move(c) : nullptr;
}
std::unique_ptr<Core> Core::view(hal::Device& dev, std::string& err) {
  std::unique_ptr<Core> c(new Core);
  c->dev_ = &dev;
  return c->init(err) ? std::move(c) : nullptr;
}

bool Core::init(std::string& err) {
  pool_ = std::make_unique<GpuPool>(*dev_);
  for (int i = 0; i < hal::Device::kSlots; ++i) {
    if (dev_->createBuffer({16384, hal::MemKind::Upload, true}, &slots_[i].weights) != hal::Status::Ok ||
        dev_->createBuffer({256u << 10, hal::MemKind::Upload, true}, &slots_[i].taps) != hal::Status::Ok ||
        dev_->createBuffer({8u << 20, hal::MemKind::Upload, true}, &slots_[i].staging) != hal::Status::Ok)
      return err = dev_->lastError(), false;
  }
  if (dev_->createBuffer({256, hal::MemKind::DeviceLocal, true}, &gBuf) != hal::Status::Ok ||
      dev_->createBuffer({256 * 1024, hal::MemKind::DeviceLocal, true}, &partBuf) != hal::Status::Ok)
    return err = dev_->lastError(), false;
  return true;
}

Core::~Core() {
  if (!dev_) return;
  dev_->waitIdle();
  pool_.reset();
  for (SlotRes& s : slots_) {
    dev_->destroy(s.staging);
    dev_->destroy(s.readback);
    dev_->destroy(s.weights);
    dev_->destroy(s.taps);
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
  s.parts = st.parts;
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

// ============================================================================================ Executor ===
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
constexpr std::uint64_t kWaitNs = 30ull * 1000 * 1000 * 1000;

// must match core/src/renderer.cpp (kBakeMax, kToneOctLo/Hi); the start-up self-test notices a drift
constexpr float kBakeMax = 1.5f;
constexpr int kToneBase = (127 - 24) << 8;
constexpr std::size_t kToneN = ((6 + 24) << 8) + 2;
constexpr int kLat2N = 33;
constexpr float kLat2Max = 8.f;

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

GpuFailure classify(hal::Status s) {
  switch (s) {
    case hal::Status::DeviceLost: return GpuFailure::DeviceLost;
    case hal::Status::Unsupported:
    case hal::Status::InvalidArgument: return GpuFailure::Unsupported;
    default: return GpuFailure::Transient;
  }
}

// What a device copy of the HostTable payloads is (ResultCache payload, destroyed on eviction).
struct DeviceTables {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  hal::ImageH lat, lat2;
  hal::BufferH tone;
  int n = 0, n2 = 0;
};

// The device copy of one SourceStore (RGBA16F), plus its Lanczos-resized variants. Shared by every executor of a context that
// reads the same store (a frame session and its export session do not upload the 24 MP twice); lives while one of them uses it.
struct ResidentSource {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  hal::ImageH img;  // the source at its natural size
  bool have = false;
  struct Resized {
    int w = 0, h = 0;
    hal::ImageH img;
    std::uint64_t used = 0;
  };
  std::vector<std::unique_ptr<Resized>> resized;
  std::uint64_t tick = 0;
  ~ResidentSource() {
    if (!ctx || epoch != ctx->epoch()) return;  // a rebuilt device: the old handles died with it
    hal::Device& d = ctx->core().dev();
    d.waitIdle();
    if (have) d.destroy(img);
    for (auto& r : resized) d.destroy(r->img);
  }
};

// The device copy of the film tables of one frame (raw LUT, print + scan LUT, curves, sRGB decode) and of its grain sampler tables.
// Kept by the executor while the table payloads of the CPU bake (process-wide film table cache) stay the same objects.
struct DeviceFilm {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  filmnode::FilmTablesHandle tb;
  filmnode::GpuFilmTables gt;
  bool haveTables = false;
  std::shared_ptr<const filmnode::FilmPlanData> planKeep;
  filmnode::GpuFilmPlanRes pr;
  bool havePlan = false;
  ~DeviceFilm() {
    if (!ctx || epoch != ctx->epoch()) return;  // a rebuilt device: the old handles died with it
    if (havePlan) filmnode::releaseGpuPlan(ctx->core(), pr);
    if (haveTables) filmnode::releaseGpuTables(ctx->core(), gt);
  }
};

// The device copy of the DisplayStages after a film: the four 1024-sample curves ([4][n], a buffer) and the look LUT (RGBA32F n^3, red fastest).
// The identity (plain frames) is a 16 byte buffer and a 1^3 image, bound for the pipeline's sake only.
struct DeviceDisplay {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  hal::BufferH curves;
  hal::ImageH lut;
  int lutN = 0;          // 0: no look LUT
  unsigned curveMask = 0;
  int curveSamples = 0;
  float lutMix = 0.f;
  float domMin[3] = {0, 0, 0}, domMax[3] = {1, 1, 1};
  Hash128 key;
  ~DeviceDisplay() {
    if (!ctx || epoch != ctx->epoch()) return;
    hal::Device& d = ctx->core().dev();
    d.waitIdle();
    d.destroy(curves);
    d.destroy(lut);
  }
};

NodeType typeOfNode(const GpuRun& in, NodeId id) { return id < in.graph.nodes().size() ? in.graph.nodes()[id].type : NodeType::Source; }

// The plan blends a film with the normal branch (film strength < 1).
bool planHasBlend(const GpuRun& in) {
  for (const Stage& st : in.plan.stages)
    for (const Group& g : st.groups)
      for (NodeId id : g.nodes)
        if (id < in.graph.nodes().size() && in.graph.nodes()[id].type == NodeType::Blend) return true;
  return false;
}

// A frame-global solve kept on the device (the dehaze dark channel / airlight grid, the guided filter coefficients of the clarity base), keyed by the
// node key of the planner (the frame it was made for); a frame whose solve is a hit skips the accumulation sweep and the solve.
struct DeviceSolve {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  hal::ImageH img;
  int w = 0, h = 0;
  ~DeviceSolve() {
    if (!ctx || epoch != ctx->epoch()) return;
    hal::Device& d = ctx->core().dev();
    d.waitIdle();
    d.destroy(img);
  }
};

// Node types a GPU plan may contain (everything else makes the plan Unsupported, 5.9).
bool headNodeOk(NodeType t) {
  return t == NodeType::Resize || t == NodeType::Geometry || t == NodeType::Gain || t == NodeType::ToneColour || t == NodeType::ToneColourExact ||
         t == NodeType::Vignette;
}
bool windowNodeOk(NodeType t) {
  return t == NodeType::NRLuma || t == NodeType::NRChroma || t == NodeType::Dehaze || t == NodeType::Detail || t == NodeType::Grain;
}

// The graph handles and resolved blocks of one job (a frame, or one tile of one sweep).
union ResolvedStorage {
  PointNode point;
  NrLumaNode nrl;
  NrChromaNode nrc;
  DehazeNode dz;
  DetailNode dt;
  ResolvedStorage() {}
};

}  // namespace

class Executor final : public IGpuExecutor {
 public:
  explicit Executor(std::shared_ptr<Context> c) : ctx_(std::move(c)) {
    epoch_ = ctx_->epoch();
    tables_.setLimit(NodeType::ColourLattice, 2);  // the two lattices of an interactive and a final frame
    solves_.setLimit(NodeType::Dehaze, 2);  // the 1/4 grids of the last frames (a slider that does not touch the solve keeps hitting)
    solves_.setLimit(NodeType::Detail, 2);
  }
  ~Executor() override {
    std::lock_guard<std::mutex> lk(ctx_->mutex());
    releaseLocked();
  }
  hal::Api api() const override { return ctx_->api(); }
  const hal::DeviceCaps& caps() const override { return ctx_->caps(); }
  Status run(const GpuRun& in, GpuOut& out) override;
  const ResultCache* solveCache() const override { return &solves_; }
  void injectFailure(GpuFailure f, int count) override {
    injected_ = f;
    injectCount_ = count;
  }
  void release() override {
    std::lock_guard<std::mutex> lk(ctx_->mutex());
    releaseLocked();
  }

 private:
  struct Walk;
  Core& core() { return ctx_->core(); }
  hal::Device& dev() { return ctx_->core().dev(); }

  // Drops this executor's device copies (the shared source goes when the last executor using it lets go).
  void releaseLocked() {
    if (epoch_ == ctx_->epoch()) dev().waitIdle();
    rs_.reset();
    store_ = nullptr;
    film_.reset();
    display_.clear();
    tables_.clear();  // payload deleters destroy the tables, or only forget them after a device rebuild
    solves_.clear();
  }
  // A rebuilt device invalidates every handle: forget them (the lost device is kept alive by the Context).
  void checkEpoch() {
    const std::uint64_t e = ctx_->epoch();
    if (e == epoch_) return;
    tables_.clear();
    film_.reset();
    display_.clear();
    solves_.clear();
    rs_.reset();
    store_ = nullptr;
    epoch_ = e;
  }

  bool fail(GpuFailure f, const std::string& msg) {
    failure_ = f;
    err_ = msg;
    return false;
  }
  bool failStatus(hal::Status s, const std::string& msg) { return fail(classify(s), msg.empty() ? dev().lastError() : msg); }

  std::uint64_t residentLimit() { return std::min<std::uint64_t>(std::uint64_t(double(dev().caps().deviceLocalBytes) * 0.35), 1536ull << 20); }
  bool canResident(int w, int h) {
    const auto& c = dev().caps();
    return w > 0 && h > 0 && std::uint32_t(w) <= c.maxImage2D && std::uint32_t(h) <= c.maxImage2D && std::uint64_t(w) * h * 8 <= residentLimit();
  }

  void bindSource(SourceStore& s) {
    if (store_ == &s && rs_ && srcId_ == s.identity() && W_ == s.width() && H_ == s.height()) return;
    store_ = &s;
    srcId_ = s.identity();
    W_ = s.width();
    H_ = s.height();
    std::shared_ptr<Context> c = ctx_;
    const std::uint64_t ep = epoch_;
    rs_ = std::static_pointer_cast<ResidentSource>(ctx_->residentFor(srcId_, [c, ep]() -> std::shared_ptr<void> {
      auto r = std::make_shared<ResidentSource>();
      r->ctx = c;
      r->epoch = ep;
      return r;
    }));
  }
  bool ensureResident();
  hal::ImageH* resizedSource(int w, int h);
  std::shared_ptr<DeviceTables> ensureTables(const GpuRun& in);
  LutBind lutBind(GpuGraph& g, const DeviceTables& t);
  // Film frames (section 20): arguments of the film nodes from the FramePlan, device tables of their bake
  bool filmResolve(const GpuRun& in, filmnode::FilmResolved& r);
  std::shared_ptr<DeviceFilm> ensureFilm(const filmnode::FilmResolved& r);
  // DisplayStages after a film: curves + look LUT on the device (the identity for plain frames); a small LRU of the last displays
  std::shared_ptr<DeviceDisplay> ensureDisplay(const FramePlan& P);

  Status runWhole(const GpuRun& in, GpuOut& out);
  Status runTiled(const GpuRun& in, GpuOut& out);
  Status finish(const GpuRun& in, GpuOut& out, Status s);
  bool validate(const GpuRun& in);

  std::shared_ptr<Context> ctx_;
  std::uint64_t epoch_ = 0;

  // source residency (SourceStore -> shared device copy)
  SourceStore* store_ = nullptr;
  std::uint64_t srcId_ = 0;
  int W_ = 0, H_ = 0;
  std::shared_ptr<ResidentSource> rs_;

  // device tables on a ResultCache (ColourLattice slot: lattice + overflow lattice + tone table of one bake key)
  ResultCache tables_;
  ResultCache solves_;  // DeviceSolve payloads by Dehaze / Detail node key (whole-frame runs)
  std::shared_ptr<DeviceFilm> film_;  // device copy of the film tables of the last film frame
  filmnode::FilmResolved lastFilmR_{};  // the last film frame that rendered: the stand-in while another stock's tables are baked in the background
  bool haveLastFilmR_ = false;
  std::vector<std::shared_ptr<DeviceDisplay>> display_;  // most recently used first (3)

  GpuFailure failure_ = GpuFailure::None;
  std::string err_;
  GpuFailure injected_ = GpuFailure::None;
  int injectCount_ = 0;
};

// ---- source -------------------------------------------------------------------------------------------------------------------
bool Executor::ensureResident() {
  if (!rs_ || !store_) return fail(GpuFailure::Unsupported, "no source");
  if (rs_->have) return true;
  if (!canResident(W_, H_)) return fail(GpuFailure::Unsupported, "source too large for a resident GPU copy");
  hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(W_), std::uint32_t(H_), 1}, &rs_->img);
  if (s != hal::Status::Ok) return failStatus(s, "");
  std::size_t pitch = 0;
  const float* px = store_->window(0, 0, W_, H_, pitch);
  std::string err;
  if (!px || !core().uploadRgbF32(px, pitch, W_, H_, rs_->img, 0, 0, err)) {
    dev().destroy(rs_->img);
    return fail(!px ? GpuFailure::Transient : dev().state() == hal::DeviceState::Lost ? GpuFailure::DeviceLost : GpuFailure::Transient, px ? err : "source store cannot be read");
  }
  rs_->have = true;
  return true;
}

// The resident source resized (Lanczos-3) to w x h; cached (two sizes).
hal::ImageH* Executor::resizedSource(int w, int h) {
  if (w == W_ && h == H_) return &rs_->img;
  for (auto& r : rs_->resized)
    if (r->w == w && r->h == h) {
      r->used = ++rs_->tick;
      return &r->img;
    }
  if (!canResident(w, h)) return fail(GpuFailure::Unsupported, "resized source too large"), nullptr;
  auto r = std::make_unique<ResidentSource::Resized>();
  r->w = w;
  r->h = h;
  hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h), 1}, &r->img);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;
  auto cl = dev().begin();
  if (!cl) {
    dev().destroy(r->img);
    return failStatus(hal::Status::Timeout, ""), nullptr;
  }
  GpuGraph g(dev(), core().pool());
  std::string err;
  GpuRecorder rec(dev(), g, err);
  rec.weights = core().slot(cl->slot()).weights;
  rec.taps = core().slot(cl->slot()).taps;
  rec.boxScanMin = core().boxScanOk ? -1 : 0;
  const H src = g.external(rs_->img);
  const SrcWin full{0, 0, W_, H_};
  const H out = recordResize(rec, src, full, W_, H_, w, h, WRect{0, 0, w, h});
  if (out < 0) {
    dev().destroy(r->img);
    return fail(GpuFailure::Transient, err), nullptr;
  }
  g.copyImage(out, g.external(r->img), std::uint32_t(w), std::uint32_t(h));
  hal::FenceH f;
  s = runGraph(dev(), g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) {
    dev().destroy(r->img);
    return failStatus(s, err), nullptr;
  }
  r->used = ++rs_->tick;
  if (rs_->resized.size() >= 2) {
    auto victim = std::min_element(rs_->resized.begin(), rs_->resized.end(), [](auto& a, auto& b) { return a->used < b->used; });
    dev().waitIdle();  // a frame of the other executor sharing this source may still read it
    dev().destroy((*victim)->img);
    rs_->resized.erase(victim);
  }
  rs_->resized.push_back(std::move(r));
  return &rs_->resized.back()->img;
}

// ---- tables (HostTable payloads on the device) ------------------------------------------------------------------------------------
std::shared_ptr<DeviceTables> Executor::ensureTables(const GpuRun& in) {
  const BakedLut& lut = in.bake;
  Hasher hk;
  hk.str(lut.key);
  const Hash128 key = hk.finish();
  if (ResultCache::Entry* e = tables_.findLru(NodeType::ColourLattice, key)) return std::static_pointer_cast<DeviceTables>(e->payload);  // LRU: the two latest lattices stay (frame + final)

  if (lut.n < 2 || lut.rgb.size() != std::size_t(lut.n) * lut.n * lut.n * 3) return fail(GpuFailure::Unsupported, "lattice not usable (film only bake?)"), nullptr;
  if (lut.tone.size() != kToneN) return fail(GpuFailure::Unsupported, "tone table size differs from the GPU backend's"), nullptr;
  std::vector<float> overOwned;
  const std::vector<float>* over = in.overflow;
  if (!over) {
    const SourceImage* img = in.store.image();
    if (!img || !in.renderer) return fail(GpuFailure::Unsupported, "overflow lattice unavailable (no CPU source)"), nullptr;
    overOwned = in.renderer->bakeOverflow(*img, lut, kLat2N, kLat2Max);
    over = &overOwned;
  }
  if (over->size() != std::size_t(kLat2N) * kLat2N * kLat2N * 3) return fail(GpuFailure::Unsupported, "overflow lattice unavailable"), nullptr;

  std::shared_ptr<Context> ctx = ctx_;
  auto t = std::shared_ptr<DeviceTables>(new DeviceTables, [](DeviceTables* p) {
    if (p->ctx && p->epoch == p->ctx->epoch()) {
      hal::Device& d = p->ctx->core().dev();
      d.waitIdle();  // a frame in flight may still read the tables
      d.destroy(p->lat);
      d.destroy(p->lat2);
      d.destroy(p->tone);
    }
    delete p;
  });
  t->ctx = ctx;
  t->epoch = epoch_;
  t->n = lut.n;
  t->n2 = kLat2N;
  hal::Status s = dev().createImage({hal::Format::RGBA32F, std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n)}, &t->lat);
  if (s == hal::Status::Ok) s = dev().createImage({hal::Format::RGBA32F, kLat2N, kLat2N, kLat2N}, &t->lat2);
  if (s == hal::Status::Ok) s = dev().createBuffer({kToneN * 4, hal::MemKind::DeviceLocal, true}, &t->tone);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;

  const std::size_t latBytes = std::size_t(lut.n) * lut.n * lut.n * 16, lat2Bytes = std::size_t(kLat2N) * kLat2N * kLat2N * 16;
  const std::size_t toneBytes = (kToneN * 4 + 255) / 256 * 256;
  auto cl = dev().begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), nullptr;
  std::string err;
  if (!core().ensureStaging(cl->slot(), toneBytes + latBytes + lat2Bytes, err)) return fail(GpuFailure::Transient, err), nullptr;
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
  texels(*over, reinterpret_cast<float*>(base + toneBytes + latBytes));
  GpuGraph g(dev(), core().pool());
  g.copyBufferToBuffer(sr.staging, g.externalBuf(t->tone), kToneN * 4, 0);
  g.copyBufferToImage(sr.staging, g.external(t->lat), std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n), toneBytes);
  g.copyBufferToImage(sr.staging, g.external(t->lat2), kLat2N, kLat2N, kLat2N, toneBytes + latBytes);
  hal::FenceH f;
  s = runGraph(dev(), g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) return failStatus(s, err), nullptr;

  ResultCache::Entry e;
  e.key = e.family = key;
  e.order = lut.n;
  e.payload = t;
  e.bytes = latBytes + lat2Bytes + kToneN * 4;
  tables_.put(NodeType::ColourLattice, std::move(e));
  return t;
}

LutBind Executor::lutBind(GpuGraph& g, const DeviceTables& t) {
  LutBind b;
  b.tone = g.externalBuf(t.tone);
  b.lat = g.external(t.lat);
  b.lat2 = g.external(t.lat2);
  b.n = t.n;
  b.n2 = t.n2;
  b.toneN = int(kToneN);
  b.toneBase = kToneBase;
  b.max1 = kBakeMax;
  b.max2 = kLat2Max;
  return b;
}

bool Executor::filmResolve(const GpuRun& in, filmnode::FilmResolved& r) {
  const FramePlan::Film& f = in.frame.film;
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  if (!lib) return fail(GpuFailure::Unsupported, "film library not available");
  filmnode::FilmFrameArgs a;
  a.lib = lib.get();
  a.stock = f.stock.c_str();
  a.paper = f.paper.c_str();
  for (int c = 0; c < 3; ++c) a.gain[c] = in.frame.gain[c];
  a.filmExposure = f.filmEv;
  a.printEv = f.printEv;
  a.black = f.black;
  a.sceneX = f.sceneX;
  a.sceneY = f.sceneY;
  a.adapt = f.adapt;
  a.warmth = f.warmth;
  a.tint = f.tint;
  a.grainPct = f.grainPct;
  a.halationPct = f.halationPct;
  a.formatScale = f.formatScale;
  a.w = in.frame.outW;
  a.h = in.frame.outH;
  a.quality = f.quality;
  a.keepGrain = f.keepGrain;
  a.coarse = f.coarse;
  a.fullLongEdge = f.fullLongEdge;
  a.frameIndex = f.frameIndex;
  // r.lib / r.stock point into the process-wide default library, which outlives the frame
  if (!filmnode::filmFrameResolved(a, r)) return fail(GpuFailure::Unsupported, "film stock / paper not available");
  r.clampOut = f.strength >= 1.f && f.displayPlain && in.frame.vignette == 0.f;
  // stock selection (wave 8): an interactive frame of a stock whose tables are not cached shows the previous stock until the background bake landed
  // (the repaint hook fires then); frame fields come from this frame, everything the tables depend on from the previous one, so nothing is baked here
  if (r.quality == 1 && haveLastFilmR_ && (r.stock != lastFilmR_.stock || r.paper != lastFilmR_.paper) && !filmnode::tablesCachedFor(r) && !deterministicMode()) {
    const char* off = std::getenv("DAPPLE_FILM_STOCK_FALLBACK");
    if (!(off && off[0] == '0')) {
      filmnode::requestStockPrep(r);
      filmnode::FilmResolved fb = lastFilmR_;
      for (int c = 0; c < 3; ++c) fb.gain[c] = r.gain[c];
      fb.w = r.w, fb.h = r.h, fb.fullLongEdge = r.fullLongEdge, fb.seed = r.seed, fb.frameIndex = r.frameIndex;
      fb.quality = r.quality, fb.keepGrain = r.keepGrain, fb.coarse = r.coarse, fb.clampOut = r.clampOut;
      fb.formatScale = r.formatScale, fb.pitch = r.pitch, fb.grainPct = r.grainPct, fb.halationPct = r.halationPct, fb.fx = r.fx;
      r = fb;
    }
  }
  std::string why;
  if (!filmnode::filmGpuSupported(r, why)) return fail(GpuFailure::Unsupported, why);
  return true;
}

std::shared_ptr<DeviceFilm> Executor::ensureFilm(const filmnode::FilmResolved& r) {
  filmnode::FilmTablesHandle tb;
  std::string err;
  // The process-wide table cache (raw + curves per stock, print + scan per stock / paper / warmth / tint / print EV): a strength, exposure,
  // grain or halation drag hits it, so nothing is baked and, because the cached payloads are the same objects, nothing is re-uploaded below.
  // (The uncached bakeFilmTables ran here on every frame: about 12 ms on the desktop, and a fresh upload each time.)
  if (!filmnode::tablesFor(r, tb)) return fail(GpuFailure::Unsupported, "film tables: bake failed"), nullptr;
  const auto pd = filmnode::planOf(r);
  // only the print table changed (a drag landed a new coarse table): replace the LUT texels in place instead of re-creating and re-uploading everything
  // a landed asynchronous copy: swap the binding (the frame that finds the fence done uses the new table)
  if (film_ && film_->epoch == epoch_ && film_->gt.lutNextActive && filmnode::finishGpuPrintLutUpdate(core(), film_->gt)) film_->tb.print = film_->gt.printNext;
  if (film_ && film_->epoch == epoch_ && film_->tb.raw == tb.raw && film_->tb.curves == tb.curves && film_->tb.print != tb.print && film_->haveTables &&
      !std::getenv("DAPPLE_FILM_LUT_INPLACE_OFF")) {
    std::string uerr;
    if (r.quality == 1 && film_->gt.lutN == tb.print->t.lutN && !std::getenv("DAPPLE_FILM_LUT_SYNC")) {
      // interactive frame: fence and swap, no wait on the frame thread; the frame shows the table that is bound now (the next landing is one frame behind)
      if (!film_->gt.lutNextActive) filmnode::startGpuPrintLutUpdate(core(), tb, film_->gt, uerr);
      tb.print = film_->tb.print;  // the bound table is the current one for this frame (raw / curves are the same); the plan below is checked as usual
    } else {
      if (film_->gt.lutNextActive) {  // a settled frame wants the exact table: wait for the pending copy first
        core().dev().waitAll();
        if (filmnode::finishGpuPrintLutUpdate(core(), film_->gt)) film_->tb.print = film_->gt.printNext;
      }
      if (film_->tb.print != tb.print && filmnode::updateGpuPrintLut(core(), tb, film_->gt, uerr)) film_->tb = tb;
    }
  }
  if (!film_ || film_->epoch != epoch_ || film_->tb.raw != tb.raw || film_->tb.curves != tb.curves || film_->tb.print != tb.print) {
    film_.reset();
    auto f = std::make_shared<DeviceFilm>();
    f->ctx = ctx_;
    f->epoch = epoch_;
    f->tb = tb;
    if (!filmnode::uploadGpuTables(core(), tb, f->gt, err)) return failStatus(hal::Status::OutOfMemory, "film tables upload: " + err), nullptr;
    f->haveTables = true;
    film_ = std::move(f);
  }
  if (!film_->havePlan || film_->planKeep != pd) {
    if (film_->havePlan) filmnode::releaseGpuPlan(core(), film_->pr);
    film_->havePlan = false;
    film_->planKeep = pd;
    if (!filmnode::uploadGpuPlan(core(), r, film_->pr, err)) return failStatus(hal::Status::OutOfMemory, "film grain tables upload: " + err), nullptr;
    film_->havePlan = true;
  }
  lastFilmR_ = r;
  haveLastFilmR_ = true;
  return film_;
}

std::shared_ptr<DeviceDisplay> Executor::ensureDisplay(const FramePlan& P) {
  const FramePlan::Display& d = P.display;
  const bool hasLut = d.lut && d.lutMix > 0.f && d.lut->size() >= 2 && d.lut->table().size() == std::size_t(d.lut->size()) * d.lut->size() * d.lut->size() * 3;
  const bool curves = d.curveMask != 0 && d.curveSamples >= 2 && d.curves.size() == std::size_t(4) * d.curveSamples;
  const bool plain = P.film.displayPlain || (!curves && !hasLut);
  Hasher hk;
  hk.i32(plain ? 0 : 1);
  if (!plain) {
    hk.i32(curves ? int(d.curveMask) : 0);
    hk.i32(curves ? d.curveSamples : 0);
    if (curves)
      for (float v : d.curves) hk.f32(v);
    hk.u64(hasLut ? d.lutId : 0);
    hk.i32(hasLut ? d.lut->size() : 0);
    hk.f32(hasLut ? d.lutMix : 0.f);
  }
  const Hash128 key = hk.finish();
  for (std::size_t i = 0; i < display_.size(); ++i)
    if (display_[i]->key == key) {
      std::rotate(display_.begin(), display_.begin() + std::ptrdiff_t(i), display_.begin() + std::ptrdiff_t(i) + 1);
      return display_.front();
    }
  const int n = hasLut ? d.lut->size() : 1;
  if (std::uint32_t(n) > dev().caps().maxImage3D) return fail(GpuFailure::Unsupported, "look LUT larger than the device's 3D image limit"), nullptr;
  auto t = std::make_shared<DeviceDisplay>();
  t->ctx = ctx_;
  t->epoch = epoch_;
  t->key = key;
  t->lutN = hasLut ? n : 0;
  t->curveMask = curves && !plain ? d.curveMask : 0;
  t->curveSamples = curves && !plain ? d.curveSamples : 0;
  t->lutMix = hasLut ? d.lutMix : 0.f;
  if (hasLut)
    for (int c = 0; c < 3; ++c) {
      t->domMin[c] = d.lut->domainMin()[c];
      t->domMax[c] = d.lut->domainMax()[c];
    }
  const std::size_t curveBytes = curves ? (d.curves.size() * 4 + 255) / 256 * 256 : 256;
  const std::size_t lutBytes = std::size_t(n) * n * n * 16;
  hal::Status s = dev().createBuffer({curveBytes, hal::MemKind::DeviceLocal, true}, &t->curves);
  if (s == hal::Status::Ok) s = dev().createImage({hal::Format::RGBA32F, std::uint32_t(n), std::uint32_t(n), std::uint32_t(n)}, &t->lut);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;
  auto cl = dev().begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), nullptr;
  std::string err;
  if (!core().ensureStaging(cl->slot(), curveBytes + lutBytes, err)) return fail(GpuFailure::Transient, err), nullptr;
  auto& sr = core().slot(cl->slot());
  auto* base = static_cast<std::uint8_t*>(sr.staging.mapped);
  std::memset(base, 0, curveBytes);
  if (curves) std::memcpy(base, d.curves.data(), d.curves.size() * 4);
  auto* texels = reinterpret_cast<float*>(base + curveBytes);
  if (hasLut) {
    const std::vector<float>& tb = d.lut->table();
    const std::size_t cnt = tb.size() / 3;
    for (std::size_t i = 0; i < cnt; ++i) {
      texels[4 * i] = tb[3 * i];
      texels[4 * i + 1] = tb[3 * i + 1];
      texels[4 * i + 2] = tb[3 * i + 2];
      texels[4 * i + 3] = 1.f;
    }
  } else {
    texels[0] = texels[1] = texels[2] = 0.f;
    texels[3] = 1.f;
  }
  GpuGraph g(dev(), core().pool());
  g.copyBufferToBuffer(sr.staging, g.externalBuf(t->curves), curveBytes, 0);
  g.copyBufferToImage(sr.staging, g.external(t->lut), std::uint32_t(n), std::uint32_t(n), std::uint32_t(n), curveBytes);
  hal::FenceH f;
  s = runGraph(dev(), g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) return failStatus(s, err), nullptr;
  display_.insert(display_.begin(), t);
  if (display_.size() > 3) display_.pop_back();
  return t;
}

bool Executor::validate(const GpuRun& in) {
  const Plan& pl = in.plan;
  if (pl.unsupported) return fail(GpuFailure::Unsupported, std::string("plan unsupported: ") + pl.why);
  if (!in.frame.supported) return fail(GpuFailure::Unsupported, "unsupported on the GPU: " + in.frame.why);
  const bool film = pl.hasFilm;
  if (film) {  // a whole film frame (section 20): film chain -> DisplayStages (identity) + Vignette -> window nodes
    if (!in.frame.film.active) return fail(GpuFailure::Unsupported, "film frame without film arguments");
    filmnode::FilmResolved fr;
    if (!filmResolve(in, fr)) return false;
    if (pl.mode == ExecMode::TiledExport && !filmnode::filmWindowable(fr))
      return fail(GpuFailure::Unsupported, "the enlarger diffusion filter blurs the whole frame (mixture halo = 4 sigma of the widest component, pyramid levels need aligned windows): no tiled export, CPU executor");
  }
  bool sweep = false;
  for (const Stage& st : pl.stages) {
    if (st.kind == StageKind::FilmFrame) {
      if (!film) return fail(GpuFailure::Unsupported, "FilmFrame stage");
      continue;
    }
    if (st.kind != StageKind::TileSweep) continue;
    sweep = true;
    for (const Group& g : st.groups) {
      for (NodeId id : g.nodes) {
        if (id >= in.graph.nodes().size()) return fail(GpuFailure::Unsupported, "plan refers to an unknown node");
        const NodeType t = in.graph.nodes()[id].type;
        if (film && g.kind == GroupKind::PointChain) {
          // DisplayStages / Blend / Vignette finish the film; Gain + ToneColour(Exact) are the normal branch of a blend (film strength < 1)
          if (t == NodeType::DisplayStages || t == NodeType::Vignette || t == NodeType::Blend || headNodeOk(t)) continue;
          return fail(GpuFailure::Unsupported, std::string("node ") + NodeRegistry::instance().desc(t).name + " after a film frame");
        }
        const bool ok = g.kind == GroupKind::PointChain ? headNodeOk(t) : g.kind == GroupKind::Window ? windowNodeOk(t) : false;
        if (!ok || !gpuImplFor(t)) return fail(GpuFailure::Unsupported, std::string("node ") + NodeRegistry::instance().desc(t).name + " has no GPU implementation");
      }
    }
  }
  if (!sweep) return fail(GpuFailure::Unsupported, "plan without a pixel sweep");
  return true;
}

// ---- the stage walk -------------------------------------------------------------------------------------------------------------
// One job = a frame, or one tile of one sweep. `ran` counts the groups of the stage list already recorded for the image `D`
// (a single-tile run keeps D across sweeps and records only what the next sweep adds).
struct Executor::Walk {
  GpuRecorder& r;
  const GpuRun& in;
  const FramePlan& P;
  SpatialRadii R;
  bool whole;
  WRect interior, work;
  SourceBind src;
  LutBind lut;
  GlobalRes* glob = nullptr;

  H D = -1, o8 = -1;
  int ran = 0;
  H col = -1, yy = -1, dc = -1, ab = -1;  // whole mode: the 1/4 grids and the solves
  Epilogue ep;
  // film frame (section 20)
  H filmH = -1;
  const filmnode::FilmResolved* fres = nullptr;
  const filmnode::GpuFilmTables* ftab = nullptr;
  const filmnode::GpuFilmPlanRes* fplan = nullptr;
  // DisplayStages after the film, the blend with the normal branch (strength < 1), the window of the film plane (tiled export)
  const DeviceDisplay* disp = nullptr;
  float strength = 1.f;
  FramePlan mainPlan;  // P without the vignette: the normal branch of a blend (the vignette follows the blend)
  int filmOx = 0, filmOy = 0;  // frame coordinates of pixel (0, 0) of filmH
  bool filmClamped = false;    // the film plane is already clamped to 0..1 (a pure-clamp film_fin is skipped)
  filmnode::FilmPre pre;

  Walk(GpuRecorder& rec, const GpuRun& run, float resizeSharpen, bool wholeFrame)
      : r(rec), in(run), P(run.frame), R(spatialRadii(run.frame, resizeSharpen)), whole(wholeFrame) {
    grainArgs(P, ep.amp, ep.period);
    ep.grain = P.grain > 0;
  }

  int gridW() const { return glob && !whole ? glob->lw : cdiv(work.w(), 4); }
  int gridH() const { return glob && !whole ? glob->lh : cdiv(work.h(), 4); }
  WRect interiorWin() const { return WRect{interior.x0 - work.x0, interior.y0 - work.y0, interior.x1 - work.x0, interior.y1 - work.y0}; }
  NodeType typeOf(NodeId id) const { return in.graph.nodes()[id].type; }

  // index (in st.groups) of the last group that ends with a real pass (the grain group is only an epilogue)
  int lastPassGroup(const Stage& st) const {
    int last = 0;
    for (int i = 0; i < int(st.groups.size()); ++i) {
      bool pass = filmH >= 0 && st.groups[std::size_t(i)].kind == GroupKind::PointChain;
      for (NodeId id : st.groups[std::size_t(i)].nodes) {
        const GpuImpl* gi = gpuImplFor(typeOf(id));
        if (gi && !gi->epilogue) pass = true;
      }
      if (pass) last = i;
    }
    return last;
  }

  // FilmFrame stage: the film input (geometry gather of the resident source when the frame is a resampling of it), then the film
  // chain (recordFilmFrame) -> the display image (RGBA32F, sRGB encoded, unclamped) that the following sweep finishes
  // the film input: the resident source, or its geometry gather (rotate / straighten / crop) at the frame size; -1 on error
  H filmInput() {
    H in = src.img;
    if (P.useGeo) {
      struct U {
        std::int32_t a[4], b[4];
        float geoA[4], geoB[4], geoC[4];
        std::int32_t c[4];
      } u{};
      u.a[0] = P.srcW;
      u.a[1] = P.srcH;
      u.a[2] = P.outW;
      u.a[3] = P.outH;
      u.geoA[0] = P.cs;
      u.geoA[1] = P.sn;
      u.geoA[2] = P.fw;
      u.geoA[3] = P.fh;
      u.geoB[0] = P.ox0;
      u.geoB[1] = P.oy0;
      u.geoB[2] = P.stepX;
      u.geoB[3] = P.stepY;
      u.geoC[0] = P.fullW;
      u.geoC[1] = P.fullH;
      u.c[0] = P.rot;
      u.c[1] = P.flip ? 1 : 0;
      const hal::PipelineH p = r.pl("film_src", {{0, P.exact ? 1 : 2}});
      if (!p) return -1;
      const H g = r.g.transient(hal::Format::RGBA32F, std::uint32_t(P.outW), std::uint32_t(P.outH));
      r.g.dispatch("film_src", p, {GpuGraph::R(src.img), GpuGraph::W(g)}, r.gx(P.outW), r.gy(P.outH), &u, sizeof u);
      in = g;
    }
    return in;
  }
  bool filmStage() {
    const H in = filmInput();
    if (in < 0) return false;
    filmH = filmnode::recordFilmFrame(r, *fres, *ftab, *fplan, in, P.outW, P.outH);
    filmClamped = filmH >= 0 && filmnode::filmEndsClamped(*fres);
    filmOx = filmOy = 0;
    return filmH >= 0;
  }
  // tiled export: the whole-frame part of the film (exposure, mixtures, DIR field) once, into `pre`
  bool filmPrePass(filmnode::FilmPre& out) {
    const H in = filmInput();
    if (in < 0) return false;
    return filmnode::recordFilmPre(r, *fres, *ftab, in, P.outW, P.outH, out);
  }
  // tiled export: the develop / grain / print / scan chain over the work rectangle grown by the film halo (clipped to the frame)
  bool filmWindowStage() {
    const int halo = filmnode::filmWindowHalo(*fres);
    filmnode::FilmWin win;
    win.x0 = std::max(0, work.x0 - halo);
    win.y0 = std::max(0, work.y0 - halo);
    win.w = std::min(P.outW, work.x1 + halo) - win.x0;
    win.h = std::min(P.outH, work.y1 + halo) - win.y0;
    const filmnode::FilmImg fi = filmnode::recordFilmWindow(r, *fres, *ftab, *fplan, pre, P.outW, P.outH, win);
    if (fi.img < 0) return false;
    filmH = fi.img;
    filmOx = fi.ox;
    filmOy = fi.oy;
    return true;
  }
  // the normal branch of a film frame at strength < 1 (Gain + ToneColour(Exact) through the lattice, no vignette): RGBA16F working image
  H mainBranch(int W, int Hh, int ox, int oy) {
    if (lut.n < 2) return r.err = "film blend without a colour lattice", -1;
    mainPlan = P;
    mainPlan.vignette = 0.f;
    PointNode pn{&mainPlan, src, lut, W, Hh, ox, oy, Epilogue{}};
    GpuPorts ports;
    const H M = r.tr(kF16, W, Hh);
    ports.out[0] = M;
    const GpuImpl* impl = gpuImplFor(NodeType::ToneColour);
    if (!impl || !impl->recordPorts(r, &pn, ports)) return -1;
    return M;
  }
  // the film plane -> display stages (curves, look LUT) -> blend with `main` (>= 0) -> vignette -> clamp -> D (or the grain epilogue + RGBA8
  // when it is the last pass)
  bool filmFinish(const Epilogue& e, H Dst, int W, int Hh, int ox, int oy, H main) {
    struct U {
      std::int32_t a[4];
      float geoB[4], geoC[4], e[4];
      std::int32_t f[4];
      float g[4], dmin[4], dmax[4];
    } u{};
    u.a[0] = W;
    u.a[1] = Hh;
    u.a[2] = ox;
    u.a[3] = oy;
    u.geoB[0] = P.ox0;
    u.geoB[1] = P.oy0;
    u.geoB[2] = P.stepX;
    u.geoB[3] = P.stepY;
    u.geoC[0] = P.fullW;
    u.geoC[1] = P.fullH;
    u.geoC[2] = P.vignette;
    u.e[0] = e.amp;
    u.e[1] = e.period;
    const bool blend = main >= 0;
    const bool dispOn = disp && (disp->curveMask != 0 || disp->lutN > 0);
    u.f[0] = filmOx;
    u.f[1] = filmOy;
    u.f[2] = dispOn ? int(disp->curveMask) : 0;
    u.f[3] = dispOn ? disp->lutN : 0;
    u.g[0] = strength;
    u.g[1] = dispOn ? disp->lutMix : 0.f;
    u.g[2] = dispOn ? float(disp->curveSamples) : 0.f;
    for (int c = 0; c < 3; ++c) {
      u.dmin[c] = disp ? disp->domMin[c] : 0.f;
      u.dmax[c] = disp ? disp->domMax[c] : 1.f;
    }
    if (!disp) return r.err = "film display resources missing", false;
    const hal::PipelineH p = r.pl("film_fin", {{10, e.last ? 1 : 0}, {11, e.last && e.grain ? 1 : 0}, {12, blend ? 1 : 0}, {13, dispOn ? 1 : 0}});
    if (!p) return false;
    r.g.dispatch("film_fin", p,
                 {GpuGraph::R(filmH), GpuGraph::W(Dst), e.last ? GpuGraph::W(e.out8) : GpuGraph::R(r.dummy(kU8)),
                  GpuGraph::R(blend ? main : r.dummy(kF16)), GpuGraph::R(r.g.externalBuf(disp->curves)), GpuGraph::R(r.g.external(disp->lut))},
                 r.gx(W), r.gy(Hh), &u, sizeof u);
    return true;
  }

  bool sweep(const Stage& st) {
    const bool finalMode = st.output;
    const int lastG = lastPassGroup(st);
    r.W = work.w();
    r.H_ = work.h();
    if (D < 0) {
      D = r.tr(kF16, r.W, r.H_);
      if (finalMode) o8 = r.tr(kU8, r.W, r.H_);
    } else if (finalMode && o8 < 0) {
      o8 = r.tr(kU8, r.W, r.H_);
    }
    const int ox = work.x0, oy = work.y0, W = r.W, Hh = r.H_;
    for (int gi = ran; gi < int(st.groups.size()); ++gi) {
      const Group& g = st.groups[std::size_t(gi)];
      Epilogue e = ep;
      e.last = finalMode && gi == lastG;
      e.out8 = e.last ? o8 : -1;
      e.grain = ep.grain && finalMode;
      // a group's nodes are one recipe (point chain) or one window node
      const NodeType t0 = typeOf(g.nodes.front());
      if (t0 == NodeType::Grain) continue;  // epilogue slot of the last pass
      ResolvedStorage rs;
      const GpuImpl* impl = gpuImplFor(t0);
      GpuPorts ports;
      const void* res = nullptr;
      if (filmH >= 0 && g.kind == GroupKind::PointChain) {  // [Gain + ToneColour] + DisplayStages [+ Blend] + Vignette of a film frame
        bool blend = false;
        for (NodeId id : g.nodes) blend |= typeOf(id) == NodeType::Blend;
        H M = -1;
        if (blend) {
          M = mainBranch(W, Hh, ox, oy);
          if (M < 0) return false;
        }
        if (filmClamped && !blend && !e.last && filmOx == 0 && filmOy == 0) {  // film_fin would only clamp: film_pd did it
          D = filmH;
          continue;
        }
        if (!filmFinish(e, D, W, Hh, ox, oy, M)) return false;
        continue;
      }
      switch (t0) {
        case NodeType::Resize:
        case NodeType::Geometry:
        case NodeType::Gain:
        case NodeType::ToneColour:
        case NodeType::Vignette:
          new (&rs.point) PointNode{&P, src, lut, W, Hh, ox, oy, e};
          res = &rs.point;
          ports.out[0] = D;
          impl = gpuImplFor(NodeType::ToneColour);  // the point recipe, whichever node of the chain comes first (Resize has no record function of its own)
          break;
        case NodeType::NRLuma:
          new (&rs.nrl) NrLumaNode{&P, &R, W, Hh, ox, oy, e};
          res = &rs.nrl;
          ports.in[0] = D;
          break;
        case NodeType::NRChroma:
          new (&rs.nrc) NrChromaNode{&P, &R, W, Hh, ox, oy, e};
          res = &rs.nrc;
          ports.in[0] = D;
          break;
        case NodeType::Dehaze: {
          const H dcH = P.dehaze > 0 ? (whole ? dc : (glob ? glob->dc : -1)) : -1;
          if (P.dehaze > 0 && dcH < 0) return r.err = "dehaze solve missing from the plan", false;
          new (&rs.dz) DehazeNode{&P, W, Hh, ox, oy, gridW(), gridH(), dcH, e};
          res = &rs.dz;
          ports.in[0] = D;
          break;
        }
        case NodeType::Detail: {
          const H abH = P.clarity != 0 ? (whole ? ab : (glob ? glob->ab : -1)) : -1;
          if (P.clarity != 0 && abH < 0) return r.err = "clarity solve missing from the plan", false;
          new (&rs.dt) DetailNode{&P, &R, W, Hh, ox, oy, gridW(), gridH(), abH, e};
          res = &rs.dt;
          ports.in[0] = D;
          break;
        }
        default: return r.err = "node without a record function", false;
      }
      if (!impl || !impl->recordPorts || !impl->recordPorts(r, res, ports)) return false;
    }
    ran = int(st.groups.size());
    return true;
  }

  // the end of a sweep: the 1/4 grid the following GlobalSolve needs
  bool accumulate(const Stage& st) {
    if (st.accum == Accum::None) return true;
    const int mode = st.accum == Accum::DehazeGrid ? 0 : 1;
    if (whole) {
      H& grid = mode == 0 ? col : yy;
      grid = r.tr(kF32, gridW(), gridH());
      return r.down(D, 4, mode, grid, WRect{0, 0, work.w(), work.h()}, 0, 0, gridW(), gridH()) >= 0;
    }
    const H dst = mode == 0 ? glob->color : glob->yy;
    return r.down(D, 4, mode, dst, interiorWin(), interior.x0 / 4, interior.y0 / 4, cdiv(interior.w(), 4), cdiv(interior.h(), 4)) >= 0;
  }

  // whole mode: solves recorded into the same graph
  bool solve(const Stage& st) {
    const NodeType t = typeOf(st.solveNode);
    if (t == NodeType::Dehaze) {
      dc = recordDehazeAnalysis(r, R, col, gridW(), gridH());
      return dc >= 0;
    }
    ab = recordClarityAnalysis(r, R, yy, gridW(), gridH());
    return ab >= 0;
  }
};

Status Executor::finish(const GpuRun& in, GpuOut& out, Status s) {
  (void)in;
  out.failure = failure_;
  out.error = err_;
  return s;
}

Status Executor::run(const GpuRun& in, GpuOut& out) {
  std::lock_guard<std::mutex> lk(ctx_->mutex());
  [[maybe_unused]] detail::BigCoreScope pin;
  checkEpoch();
  failure_ = GpuFailure::None;
  err_.clear();
  out.failure = GpuFailure::None;
  if (injectCount_ > 0) {
    --injectCount_;
    failure_ = injected_;
    err_ = "injected failure (test)";
    return finish(in, out, failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal);
  }
  if (dev().state() == hal::DeviceState::Lost) return fail(GpuFailure::DeviceLost, "device lost"), finish(in, out, Status::Internal);
  if (!validate(in)) return finish(in, out, Status::Unsupported);
  if (in.cancel.cancelled()) return fail(GpuFailure::None, "cancelled"), finish(in, out, Status::Cancelled);
  bindSource(in.store);
  const Status s = in.plan.mode == ExecMode::TiledExport ? runTiled(in, out) : runWhole(in, out);
  if (s != Status::Ok && s != Status::Cancelled && failure_ == GpuFailure::None) failure_ = GpuFailure::Transient;
  return finish(in, out, s);
}

// ---- whole frame ----------------------------------------------------------------------------------------------------------------------
Status Executor::runWhole(const GpuRun& in, GpuOut& out) {
  const auto t0 = Clock::now();
  hal::Device& d = dev();
  const FramePlan& P = in.frame;
  const hal::DeviceCaps& caps = d.caps();
  const int fw = P.outW, fh = P.outH;
  if (fw <= 0 || fh <= 0 || std::uint32_t(fw) > caps.maxImage2D || std::uint32_t(fh) > caps.maxImage2D)
    return fail(GpuFailure::Unsupported, "frame size outside the device limits"), Status::Unsupported;
  if (!canResident(W_, H_)) return fail(GpuFailure::Unsupported, "source too large for a resident GPU copy"), Status::Unsupported;
  if (!ensureResident()) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
  hal::ImageH* S = resizedSource(P.srcW, P.srcH);
  if (!S) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
  const bool filmFrame = in.plan.hasFilm;
  std::shared_ptr<DeviceTables> L;
  std::shared_ptr<DeviceFilm> DF;
  std::shared_ptr<DeviceDisplay> DD;
  filmnode::FilmResolved fres;
  if (filmFrame) {  // the film replaces tone + colour: no colour lattice, the film tables instead
    // all intermediates of the film (E, E2, q, D, grain, linear light, ...) are RGBA32F frames: bounded by the device memory
    if (std::uint64_t(fw) * fh * 288 > std::uint64_t(double(caps.deviceLocalBytes) * 0.45))
      return fail(GpuFailure::Unsupported, "film frame too large for the device memory"), Status::Unsupported;
    if (!filmResolve(in, fres)) return Status::Unsupported;
    DF = ensureFilm(fres);
    if (!DF) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
    DD = ensureDisplay(P);
    if (!DD) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
    if (planHasBlend(in)) {  // film strength < 1: the normal branch (colour lattice) is rendered next to the film
      L = ensureTables(in);
      if (!L) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
    }
  } else {
    L = ensureTables(in);
    if (!L) return failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
  }
  const double uploadMs = msSince(t0);
  if (in.cancel.cancelled()) return fail(GpuFailure::None, "cancelled"), Status::Cancelled;

  auto cl = d.begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), Status::Internal;
  const int slot = cl->slot();
  // zero readback only when the device can write the presenter's image and the frame fits it; otherwise the frame is read back as
  // usual (the caller sees rgba != null and grows its image for the next frame)
  const hal::ExternalImage* ext = in.target.external;
  const bool external = ext && caps.externalImageWrite && ext->handle && std::uint32_t(fw) <= ext->w && std::uint32_t(fh) <= ext->h;
  const std::size_t bytes = std::size_t(fw) * fh * 4;
  std::string err;
  if (!external && !core().ensureReadback(slot, bytes, err)) return fail(GpuFailure::Transient, err), Status::Internal;
  GpuGraph g(d, core().pool());
  GpuRecorder rec(d, g, err);
  rec.weights = core().slot(slot).weights;
  rec.taps = core().slot(slot).taps;
  rec.boxScanMin = core().boxScanOk ? -1 : 0;
  rec.gBuf = core().gBuf;
  rec.partBuf = core().partBuf;

  const bool down = P.srcW < W_ || P.srcH < H_;
  Walk w(rec, in, down ? in.resizeSharpen : 0.f, true);
  w.interior = w.work = WRect{0, 0, fw, fh};
  w.src.img = g.external(*S);
  if (L) w.lut = lutBind(g, *L);
  if (DF) {
    w.fres = &fres;
    w.ftab = &DF->gt;
    w.fplan = &DF->pr;
    w.disp = DD.get();
    w.strength = P.film.strength;
  }
  // frame-global solves: a hit binds the kept grid (the plan pruned the sweep and the solve), a miss is copied into a kept image for the next frame
  struct SolveStore {
    NodeType type;
    Hash128 key;
    std::shared_ptr<DeviceSolve> keep;
  };
  std::vector<SolveStore> stores;
  std::vector<std::shared_ptr<DeviceSolve>> hits;
  for (const CacheOp& op : in.plan.cache) {
    if (op.type != NodeType::Dehaze && op.type != NodeType::Detail) continue;
    const bool dz = op.type == NodeType::Dehaze;
    const int gw = cdiv(fw, 4), gh = cdiv(fh, 4);
    if (op.kind == CacheOp::Lookup && op.hit) {
      ResultCache::Entry* e = solves_.findLru(op.type, op.key);
      auto sp = e ? std::static_pointer_cast<DeviceSolve>(e->payload) : nullptr;
      if (!sp || sp->w != gw || sp->h != gh) return fail(GpuFailure::Transient, "kept solve missing"), Status::Internal;
      hits.push_back(sp);
      (dz ? w.dc : w.ab) = g.external(sp->img);
    } else if (op.kind == CacheOp::Store) {
      auto sp = std::make_shared<DeviceSolve>();
      sp->ctx = ctx_;
      sp->epoch = epoch_;
      sp->w = gw;
      sp->h = gh;
      const hal::Status cs = d.createImage({dz ? hal::Format::R32F : hal::Format::RGBA32F, std::uint32_t(gw), std::uint32_t(gh), 1}, &sp->img);
      if (cs != hal::Status::Ok) return failStatus(cs, ""), Status::Internal;
      stores.push_back({op.type, op.key, sp});
    }
  }
  for (const Stage& st : in.plan.stages) {
    if (st.kind == StageKind::FilmFrame) {
      if (!w.filmStage()) {  // an effect the GPU film does not port (error text "film GPU: ..."): the CPU renders the frame
        const bool unsup = err.rfind("film GPU:", 0) == 0;
        return fail(unsup ? GpuFailure::Unsupported : GpuFailure::Transient, err), unsup ? Status::Unsupported : Status::Internal;
      }
    } else if (st.kind == StageKind::TileSweep) {
      if (!w.sweep(st) || !w.accumulate(st)) return fail(GpuFailure::Transient, err), Status::Internal;
    } else if (st.kind == StageKind::GlobalSolve) {
      if (!w.solve(st)) return fail(GpuFailure::Transient, err), Status::Internal;
      for (const SolveStore& ss : stores)
        if (ss.type == typeOfNode(in, st.solveNode)) g.copyImage(ss.type == NodeType::Dehaze ? w.dc : w.ab, g.external(ss.keep->img), std::uint32_t(ss.keep->w), std::uint32_t(ss.keep->h));
    }
  }
  if (w.o8 < 0) return fail(GpuFailure::Unsupported, "plan has no output sweep"), Status::Unsupported;
  if (external) g.copyImageToExternal(w.o8, *ext, std::uint32_t(fw), std::uint32_t(fh));
  else g.copyImageToBuffer(w.o8, core().slot(slot).readback, 0, 0, std::uint32_t(fw), std::uint32_t(fh));

  const auto tSub = Clock::now();
  hal::FenceH fence;
  hal::Status s = runGraph(d, g, *cl, &fence, err);
  if (s != hal::Status::Ok) return failStatus(s, err), Status::Internal;
  const auto tWait = Clock::now();
  for (SolveStore& ss : stores) {
    ResultCache::Entry e;
    e.key = e.family = ss.key;
    e.payload = ss.keep;
    e.bytes = std::size_t(ss.keep->w) * ss.keep->h * (ss.type == NodeType::Dehaze ? 4 : 16);
    solves_.put(ss.type, std::move(e));
  }
  Clock::time_point t1 = tWait;
  double copyMs = 0;
  if (!external) {
    s = d.wait(fence, kWaitNs);
    if (s != hal::Status::Ok) return failStatus(s, ""), Status::Internal;
    t1 = Clock::now();
    auto* px = static_cast<std::uint8_t*>(std::malloc(bytes));
    if (!px) return fail(GpuFailure::Transient, "out of memory"), Status::OutOfMemory;
    std::memcpy(px, core().slot(slot).readback.mapped, bytes);
    copyMs = msSince(t1);
    out.rgba = px;
  }
  out.w = fw;
  out.h = fh;
  const auto t2 = Clock::now();
  GpuPool& pool = core().pool();
  if (pool.freeBytes() > d.poolBudget()) pool.trim(d.poolBudget());
  const double trimMs = msSince(t2);
  GpuStats& st = out.stats;
  const Core::Stats cs = core().statsOf(fence);
  st.wallMs = msSince(t0);
  st.gpuMs = cs.gpuMs;
  st.passes = cs.passes;
  st.barriers = cs.barriers;
  st.parts = cs.parts;
  st.tiles = 1;
  st.sweeps = in.plan.sweeps();
  st.passTimes.clear();
  for (const auto& t : cs.times) st.passTimes.push_back({t.name, t.ms});
  {  // DAPPLE_GPU_PROFILE=1: the per-pass GPU times of every frame go to the log (wave 8: where a strength / kept-grain frame spends its time)
    static const bool logPasses = std::getenv("DAPPLE_GPU_PROFILE") != nullptr && std::getenv("DAPPLE_GPU_PASSLOG") != nullptr;
    if (logPasses && !cs.times.empty()) {
      std::string line = "gpu passes:";
      char b[64];
      for (const auto& t : cs.times) {
        std::snprintf(b, sizeof b, " %s %.2f;", t.name.c_str(), t.ms);
        line += b;
      }
#if defined(__ANDROID__)
      __android_log_print(ANDROID_LOG_INFO, "dapple_engine", "%s", line.c_str());
#else
      std::fprintf(stderr, "dapple_engine: %s\n", line.c_str());
#endif
    }
  }
  st.uploadMs = uploadMs;
  st.readbackMs = copyMs;
  st.trimMs = trimMs;
  st.recordMs = std::chrono::duration<double, std::milli>(tSub - t0).count() - uploadMs;
  st.submitMs = std::chrono::duration<double, std::milli>(tWait - tSub).count();
  st.waitMs = std::chrono::duration<double, std::milli>(t1 - tWait).count();
  return Status::Ok;
}

// ---- tiled export -------------------------------------------------------------------------------------------------------------------------
Status Executor::runTiled(const GpuRun& in, GpuOut& out) {
  const auto t0 = Clock::now();
  hal::Device& d = dev();
  GpuPool& pool = core().pool();
  const double gpu0 = d.gpuAccumMs();
  const FramePlan& P = in.frame;
  auto bad = [&](Status s) { return failure_ == GpuFailure::Unsupported ? Status::Unsupported : s; };
  if (!in.target.sink) return fail(GpuFailure::Unsupported, "no tile sink"), Status::Unsupported;
  const int rw = P.srcW, rh = P.srcH;
  const bool down = rw < W_ || rh < H_;
  const float rsharp = down ? in.resizeSharpen : 0.f;
  const SpatialRadii R = spatialRadii(P, rsharp);
  const int maxDim = int(d.caps().maxImage2D);
  int tile = std::max(64, in.plan.grid.tile / 4 * 4);
  while (tile + 2 * R.halo > maxDim && tile > 64) tile = std::max(64, (tile / 2) / 4 * 4);
  if (tile + 2 * R.halo > maxDim) return fail(GpuFailure::Unsupported, "device image limit too small for the halo"), Status::Unsupported;

  const bool filmFrame = in.plan.hasFilm;
  std::shared_ptr<DeviceTables> L;
  if (!filmFrame || planHasBlend(in)) {  // a film frame needs the colour lattice only for the blend with the normal branch
    L = ensureTables(in);
    if (!L) return bad(Status::Internal);
  }
  std::shared_ptr<DeviceFilm> DF;
  std::shared_ptr<DeviceDisplay> DD;
  filmnode::FilmResolved fres;
  if (filmFrame) {  // the whole-frame part of the film (E2, q) is kept on the device while the tiles run
    if (std::uint64_t(P.outW) * P.outH * 128 > std::uint64_t(double(d.caps().deviceLocalBytes) * 0.5))
      return fail(GpuFailure::Unsupported, "film frame too large for the device memory"), Status::Unsupported;
    if (!filmResolve(in, fres)) return Status::Unsupported;
    DF = ensureFilm(fres);
    if (!DF) return bad(Status::Internal);
    DD = ensureDisplay(P);
    if (!DD) return bad(Status::Internal);
  }

  // the source: resident (small enough) or streamed per tile; a film export keeps it resident (the whole-frame film part reads all of it)
  const bool resident = filmFrame ? (canResident(W_, H_) && canResident(rw, rh))
                                  : (!in.forceStream && canResident(W_, H_) && canResident(rw, rh) && std::uint64_t(W_) * H_ <= 40000000ull);
  if (filmFrame && !resident) return fail(GpuFailure::Unsupported, "film export needs the source resident on the device"), Status::Unsupported;
  hal::ImageH* S = nullptr;
  if (resident) {
    if (!ensureResident()) return bad(Status::Internal);
    S = resizedSource(rw, rh);
    if (!S) return bad(Status::Internal);
  }

  bool wantDz = false, wantClar = false;
  for (const Stage& st : in.plan.stages) {
    wantDz |= st.kind == StageKind::TileSweep && st.accum == Accum::DehazeGrid;
    wantClar |= st.kind == StageKind::TileSweep && st.accum == Accum::LumaGrid;
  }
  const int glw = cdiv(P.outW, 4), glh = cdiv(P.outH, 4);
  hal::ImageH gColor, gDc, gYy, gAb;
  struct Free {
    hal::Device& dev;
    hal::ImageH* im[4];
    ~Free() {
      dev.waitIdle();
      for (hal::ImageH* i : im) dev.destroy(*i);
    }
  } freer{d, {&gColor, &gDc, &gYy, &gAb}};
  auto mk = [&](hal::ImageH& im, hal::Format f) {
    const hal::Status s = d.createImage({f, std::uint32_t(glw), std::uint32_t(glh), 1}, &im);
    return s == hal::Status::Ok || (failStatus(s, ""), false);
  };
  if (wantDz && (!mk(gColor, hal::Format::RGBA32F) || !mk(gDc, hal::Format::R32F))) return Status::Internal;
  if (wantClar && (!mk(gYy, hal::Format::RGBA32F) || !mk(gAb, hal::Format::RGBA32F))) return Status::Internal;

  // ---- film: exposure, mixtures and the DIR field over the whole frame, once; the tiles read them as persistent images ----
  struct FilmKeep {
    hal::Device& dev;
    hal::ImageH e2, q, disp;
    bool any = false;
    ~FilmKeep() {
      dev.waitIdle();
      dev.destroy(e2);
      dev.destroy(q);
      dev.destroy(disp);
    }
  } keep{d, {}, {}, {}};
  if (filmFrame) {
    auto cl = d.begin();
    if (!cl) return failStatus(hal::Status::Timeout, ""), Status::Internal;
    const int slot = cl->slot();
    std::string perr;
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, perr);
    rec.weights = core().slot(slot).weights;
    rec.taps = core().slot(slot).taps;
    rec.boxScanMin = core().boxScanOk ? -1 : 0;
    rec.gBuf = core().gBuf;
    rec.partBuf = core().partBuf;
    Walk wp(rec, in, rsharp, true);
    wp.interior = wp.work = WRect{0, 0, P.outW, P.outH};
    wp.src.img = g.external(*S);
    wp.fres = &fres;
    wp.ftab = &DF->gt;
    wp.fplan = &DF->pr;
    filmnode::FilmPre pre;
    if (!wp.filmPrePass(pre)) {
      const bool unsup = perr.rfind("film GPU:", 0) == 0;
      return fail(unsup ? GpuFailure::Unsupported : GpuFailure::Transient, perr), unsup ? Status::Unsupported : Status::Internal;
    }
    keep.any = pre.any;
    hal::Status ms = hal::Status::Ok;
    auto mkImg = [&](hal::ImageH& im) {
      if (ms == hal::Status::Ok) ms = d.createImage({hal::Format::RGBA16F, std::uint32_t(P.outW), std::uint32_t(P.outH), 1}, &im);
    };
    if (pre.any) {
      mkImg(keep.e2);
      if (pre.q >= 0) mkImg(keep.q);
    } else {
      mkImg(keep.disp);
    }
    if (ms != hal::Status::Ok) return failStatus(ms, ""), Status::Internal;
    if (pre.any) {
      g.copyImage(pre.e2, g.external(keep.e2), std::uint32_t(P.outW), std::uint32_t(P.outH));
      if (pre.q >= 0) g.copyImage(pre.q, g.external(keep.q), std::uint32_t(P.outW), std::uint32_t(P.outH));
    } else {
      g.copyImage(pre.disp, g.external(keep.disp), std::uint32_t(P.outW), std::uint32_t(P.outH));
    }
    hal::FenceH fence;
    ms = runGraph(d, g, *cl, &fence, perr);
    if (ms == hal::Status::Ok) ms = d.wait(fence, kWaitNs);
    if (ms != hal::Status::Ok) return failStatus(ms, perr), Status::Internal;
    if (in.cancel.cancelled()) return fail(GpuFailure::None, "cancelled"), Status::Cancelled;
  }

  std::vector<WRect> tiles;
  for (int y = 0; y < P.outH; y += tile)
    for (int x = 0; x < P.outW; x += tile) tiles.push_back({x, y, std::min(P.outW, x + tile), std::min(P.outH, y + tile)});

  struct Pending {
    int slot = -1;
    hal::FenceH fence;
    WRect interior;
  };
  Pending pend;
  int passes = 0, barriers = 0, parts = 0;
  std::string err;
  auto flush = [&]() -> bool {
    if (pend.slot < 0) return true;
    const hal::Status ws = d.wait(pend.fence, kWaitNs);
    if (ws != hal::Status::Ok) return failStatus(ws, "");
    const bool ok = in.target.sink(pend.interior.x0, pend.interior.y0, pend.interior.w(), pend.interior.h(),
                                   static_cast<const std::uint8_t*>(core().slot(pend.slot).readback.mapped), std::size_t(pend.interior.w()) * 4);
    pend.slot = -1;
    d.pinSlot(-1);
    if (!ok) fail(GpuFailure::Unsupported, "export aborted by the sink");
    return ok;
  };

  auto bindGlobals = [&](GpuGraph& g, GlobalRes& gl) {
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
  };

  // one sweep over one tile
  auto runTile = [&](const Stage& st, const WRect& interior) -> bool {
    const WRect work{std::max(0, interior.x0 - R.halo), std::max(0, interior.y0 - R.halo), std::min(P.outW, interior.x1 + R.halo),
                     std::min(P.outH, interior.y1 + R.halo)};
    const bool readback = st.output;
    // 1. the source window of a streamed export is uploaded first (the upload takes frame slots of its own)
    hal::ImageH acquired;
    bool haveAcq = false;
    const WRect fp = footprint(P, work);
    SrcWin win{fp.x0, fp.y0, fp.x1, fp.y1};
    if (!resident) {
      if (down) win = lanczosWindow(W_, H_, rw, rh, fp);
      const hal::Status as = pool.acquire(hal::Format::RGBA16F, std::uint32_t(win.w()), std::uint32_t(win.h()), &acquired);
      if (as != hal::Status::Ok) return failStatus(as, "");
      haveAcq = true;
      std::size_t pitch = 0;
      const float* base = store_->window(win.x0, win.y0, win.w(), win.h(), pitch);
      if (!base || !core().uploadRgbF32(base, pitch, win.w(), win.h(), acquired, 0, 0, err)) {
        pool.release(acquired);
        return fail(!base ? GpuFailure::Transient : d.state() == hal::DeviceState::Lost ? GpuFailure::DeviceLost : GpuFailure::Transient, base ? err : "source store cannot be read");
      }
    }
    auto cl = d.begin();
    if (!cl) {
      if (haveAcq) pool.release(acquired);
      return failStatus(hal::Status::Timeout, "");
    }
    const int slot = cl->slot();
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, err);
    rec.weights = core().slot(slot).weights;
    rec.taps = core().slot(slot).taps;
    rec.boxScanMin = core().boxScanOk ? -1 : 0;
    rec.gBuf = core().gBuf;
    rec.partBuf = core().partBuf;
    rec.allocW = int(pool.bucket(std::uint32_t(std::min(P.outW, tile + 2 * R.halo))));
    rec.allocH = int(pool.bucket(std::uint32_t(std::min(P.outH, tile + 2 * R.halo))));
    Walk w(rec, in, rsharp, false);
    w.interior = interior;
    w.work = work;
    if (L) w.lut = lutBind(g, *L);
    if (filmFrame) {
      w.fres = &fres;
      w.ftab = &DF->gt;
      w.fplan = &DF->pr;
      w.disp = DD.get();
      w.strength = P.film.strength;
      w.pre.any = keep.any;
      if (keep.any) {
        w.pre.e2 = g.external(keep.e2);
        if (keep.q) w.pre.q = g.external(keep.q);
      } else {
        w.pre.disp = g.external(keep.disp);
      }
    }
    GlobalRes gl;
    bindGlobals(g, gl);
    w.glob = &gl;
    bool ok = true;
    if (resident) {
      w.src.img = g.external(*S);
    } else {
      const H a = g.external(acquired);
      if (down) {
        const H s2 = recordResize(rec, a, win, W_, H_, rw, rh, fp);
        if (s2 < 0) ok = false;
        w.src.img = s2;
        w.src.ox = fp.x0;
        w.src.oy = fp.y0;
      } else {
        w.src.img = a;
        w.src.ox = win.x0;
        w.src.oy = win.y0;
      }
    }
    if (ok && filmFrame) ok = w.filmWindowStage();
    if (ok) ok = w.sweep(st) && w.accumulate(st);
    if (ok && readback) {
      if (w.o8 < 0) ok = false;
      const std::size_t bytes = std::size_t(interior.w()) * interior.h() * 4;
      if (ok && !core().ensureReadback(slot, bytes, err)) ok = false;
      else if (ok) g.copyImageToBuffer(w.o8, core().slot(slot).readback, std::uint32_t(interior.x0 - work.x0), std::uint32_t(interior.y0 - work.y0), std::uint32_t(interior.w()), std::uint32_t(interior.h()));
    }
    if (!ok) {
      if (haveAcq) pool.release(acquired);
      return fail(err.rfind("film GPU:", 0) == 0 ? GpuFailure::Unsupported : GpuFailure::Transient, err);
    }
    hal::FenceH fence;
    const hal::Status s = runGraph(d, g, *cl, &fence, err);
    if (haveAcq) pool.release(acquired);
    if (s != hal::Status::Ok) return failStatus(s, err);
    {
      const auto sst = d.stats(fence);
      passes += sst.passes;
      barriers += sst.barriers;
      parts += sst.parts;
    }
    if (readback) {
      if (!flush()) return false;  // previous tile: its GPU work ran while this one was recorded
      pend.slot = slot;
      pend.fence = fence;
      pend.interior = interior;
      d.pinSlot(slot);
    }
    return true;
  };

  // the GlobalSolve stage of a tiled run: its own small graph over the accumulated 1/4 grid
  auto globalPass = [&](const Stage& st) -> bool {
    const bool dehaze = in.graph.nodes()[st.solveNode].type == NodeType::Dehaze;
    auto cl = d.begin();
    if (!cl) return failStatus(hal::Status::Timeout, "");
    const int slot = cl->slot();
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, err);
    rec.weights = core().slot(slot).weights;
    rec.taps = core().slot(slot).taps;
    rec.boxScanMin = core().boxScanOk ? -1 : 0;
    rec.gBuf = core().gBuf;
    rec.partBuf = core().partBuf;
    GlobalRes gl;
    bindGlobals(g, gl);
    rec.W = glw;
    rec.H_ = glh;
    if (dehaze) {
      const H dc = recordDehazeAnalysis(rec, R, gl.color, glw, glh);
      if (dc < 0) return fail(GpuFailure::Transient, err);
      g.copyImage(dc, gl.dc, std::uint32_t(glw), std::uint32_t(glh));
    } else {
      const H ab = recordClarityAnalysis(rec, R, gl.yy, glw, glh);
      if (ab < 0) return fail(GpuFailure::Transient, err);
      g.copyImage(ab, gl.ab, std::uint32_t(glw), std::uint32_t(glh));
    }
    const hal::Status s = runGraph(d, g, *cl, nullptr, err);
    return s == hal::Status::Ok || failStatus(s, err);
  };

  int sweeps = 0;
  for (const Stage& st : in.plan.stages) {
    if (st.kind == StageKind::TileSweep) {
      ++sweeps;
      for (const WRect& t : tiles) {
        if (in.cancel.cancelled()) return fail(GpuFailure::None, "cancelled"), Status::Cancelled;
        if (!runTile(st, t)) return bad(Status::Internal);
      }
      if (st.output && !flush()) return bad(Status::Internal);
    } else if (st.kind == StageKind::GlobalSolve) {
      if (!globalPass(st)) return bad(Status::Internal);
    }
  }
  if (d.waitAll() != hal::Status::Ok) return failStatus(hal::Status::Timeout, ""), Status::Internal;
  if (pool.freeBytes() > d.poolBudget()) pool.trim(d.poolBudget());
  out.w = P.outW;
  out.h = P.outH;
  out.stats.wallMs = msSince(t0);
  out.stats.gpuMs = d.gpuAccumMs() - gpu0;
  out.stats.passes = passes;
  out.stats.barriers = barriers;
  out.stats.parts = parts;
  out.stats.tiles = int(tiles.size());
  out.stats.sweeps = sweeps;
  return Status::Ok;
}

std::unique_ptr<IGpuExecutor> Context::createExecutor() { return std::make_unique<Executor>(shared_from_this()); }

}  // namespace dapple::engine::gpu
