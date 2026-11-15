#pragma once
// Internal to the GPU executor (gpu_executor.cpp, gpu_device_copies.cpp, gpu_walk.cpp).
//
//   device copies  what the executor uploads for a frame: the resident source (shared by the executors of a context), the colour tables, the
//                  HDR / reshaping / local adjustment buffers, the display stages, the film tables, the frame-global solves. Everything but the
//                  source and the film tables lives in the session's ResultCache (device slots, with its device bytes); a released copy is
//                  handed to Core::retire (destroyed once the work submitted before has completed, never a waitIdle).
//   Walk           the recording of one job (a whole frame, or one tile of one sweep): the plan's groups through the nodes' GpuImpls.
//   Executor       IGpuExecutor: validation, the whole-frame and the tiled run, failure reporting (GpuFailure + GpuReason).
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/exec/gpu_executor.h"
#include "engine/exec/result_cache.h"
#include "engine/nodes/film/film_gpu.h"
#include "engine/nodes/film/film_node_internal.h"
#include "engine/nodes/gpu_common.h"
#include "engine/nodes/gpu_nodes.h"

namespace dapple::engine::gpu {

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
constexpr std::uint64_t kWaitNs = 30ull * 1000 * 1000 * 1000;

// must match core/engine/nodes/point/kernels.h (kBakeMax, kToneOctLo/Hi); the start-up self-test notices a drift
constexpr float kBakeMax = 1.5f;
constexpr int kToneBase = (127 - 24) << 8;
constexpr std::size_t kToneN = ((6 + 24) << 8) + 2;
constexpr int kLat2N = 33;
constexpr float kLat2Max = 8.f;

// ---- device copies ------------------------------------------------------------------------------------------------------------------
// Base: the context and device generation the handles belong to; a copy of an older generation is only forgotten (the device died).
struct DeviceCopy {
  std::shared_ptr<Context> ctx;
  std::uint64_t epoch = 0;
  bool current() const { return ctx && epoch == ctx->epoch(); }
  void retire(hal::ImageH h) const {
    if (current()) ctx->core().retire(h);
  }
  void retire(hal::BufferH h) const {
    if (current()) ctx->core().retire(h);
  }
};
// colour lattice + overflow lattice of one colour stage (cache slot: slotOf(ColourLattice, true), keyed by BakedLut::latticeKey): uploaded
// once per lattice, whatever the tone stage, the exposure or the white balance do
struct DeviceLattice : DeviceCopy {
  hal::ImageH lat, lat2;
  int n = 0, n2 = 0;
  ~DeviceLattice() {
    retire(lat);
    retire(lat2);
  }
};
// a read-only SSBO (HDR field + reshaping field, local adjustment descriptors / plane store, the tone table)
struct DeviceBuffer : DeviceCopy {
  hal::BufferH buf;
  ~DeviceBuffer() { retire(buf); }
};
// What the point kernels bind: the lattice of the frame's colour stage + the tone table of its bake (slot kDeviceToneSlot, by bake key).
// Cached apart, so a drag that moves only the tone table uploads 30 KB instead of the 3.6 MB of the two lattices.
struct DeviceTables {
  std::shared_ptr<DeviceLattice> lattice;
  std::shared_ptr<DeviceBuffer> tone;
};
// the frame-global solve of a node (cache slot: slotOf(type, true))
struct DeviceSolve : DeviceCopy {
  hal::ImageH img;
  int w = 0, h = 0;
  ~DeviceSolve() { retire(img); }
};
// the display stages after a film: curves + look LUT (cache slot kDeviceDisplaySlot)
struct DeviceDisplay : DeviceCopy {
  hal::BufferH curves;
  hal::ImageH lut;
  int lutN = 0;
  unsigned curveMask = 0;
  int curveSamples = 0;
  float lutMix = 0.f;
  float domMin[3] = {0, 0, 0}, domMax[3] = {1, 1, 1};
  ~DeviceDisplay() {
    retire(curves);
    retire(lut);
  }
  DisplayBind bind(GpuGraph& g) const;
};
// The device copy of one SourceStore (RGBA16F) and its Lanczos-resized variants. Shared by every executor of a context that reads the same
// store (a frame session and its export session do not upload the 24 MP twice); lives while one of them uses it. Its fields change inside
// device turns only; a frame holds what it binds (the source, a resized variant) by shared pointer across its turns, so another session
// evicting a variant between two tiles of an export retires it only after the export let go.
struct ResidentSource : DeviceCopy {
  hal::ImageH img;
  bool have = false;
  struct Resized : DeviceCopy {
    int w = 0, h = 0;
    hal::ImageH img;
    std::uint64_t used = 0;
    ~Resized() { retire(img); }
  };
  std::vector<std::shared_ptr<Resized>> resized;
  std::uint64_t tick = 0;
  ~ResidentSource() {
    if (have) retire(img);
  }
};
// The film tables of the current film frame (raw LUT, print + scan LUT, curves, sRGB decode) and its grain sampler tables, kept by the
// executor while the CPU bake's payloads (process-wide film table cache) stay the same objects.
struct DeviceFilm : DeviceCopy {
  filmnode::FilmTablesHandle tb;
  filmnode::GpuFilmTables gt;
  bool haveTables = false;
  std::shared_ptr<const filmnode::FilmPlanData> planKeep;
  filmnode::GpuFilmPlanRes pr;
  bool havePlan = false;
  ~DeviceFilm() {
    if (!current()) return;
    if (havePlan) filmnode::releaseGpuPlan(ctx->core(), pr);
    if (haveTables) filmnode::releaseGpuTables(ctx->core(), gt);
  }
};
constexpr CacheSlot kDeviceDisplaySlot = 0xC004;

// ---- the recording of one job ---------------------------------------------------------------------------------------------------------
// A frame-global solve of a job: the grid the sweep before it accumulates and the solved image (whole frame: transients of the job's
// graph; tiled export: persistent images shared by every tile).
struct SolveRes {
  NodeId node = 0;
  Accum accum = Accum::None;
  H grid = -1, solved = -1;
};

struct Walk {
  GpuRecorder& r;
  const GpuRun& in;
  const NodeRegistry& reg = NodeRegistry::instance();
  FrameView view;
  bool whole;
  WRect interior, work;
  GpuJob job;
  bool needAux = false;
  std::vector<SolveRes> solves;
  H D = -1, o8 = -1;
  int ran = 0;  // groups of the stage list already recorded for D (a single-tile run keeps D across sweeps)
  Epilogue ep;
  // film frame (section 20)
  const filmnode::FilmResolved* fres = nullptr;
  const filmnode::GpuFilmTables* ftab = nullptr;
  const filmnode::GpuFilmPlanRes* fplan = nullptr;
  filmnode::FilmPre pre;

  Walk(GpuRecorder& rec, const GpuRun& run, bool wholeFrame);
  SolveRes* solveOf(NodeId id);
  WRect interiorWin() const { return WRect{interior.x0 - work.x0, interior.y0 - work.y0, interior.x1 - work.x0, interior.y1 - work.y0}; }
  // FilmFrame stage: the film input (the resident source, its geometry gather, or the local adjustments' stage L on it) -> the film chain
  H filmInput();
  bool filmStage();
  bool filmPrePass(filmnode::FilmPre& out);  // tiled export: the whole-frame part of the film, once
  bool filmWindowStage();                    // tiled export: the film over the work rect grown by the film halo
  bool sweep(const Stage& st);
  bool accumulate(const Stage& st);
  bool solve(const Stage& st);  // whole frame: the solve recorded into the same graph
};

// ---- the executor ---------------------------------------------------------------------------------------------------------------------
class Executor final : public IGpuExecutor {
 public:
  explicit Executor(std::shared_ptr<Context> c);
  ~Executor() override;
  hal::Api api() const override { return ctx_->api(); }
  const hal::DeviceCaps& caps() const override { return ctx_->caps(); }
  Status run(const GpuRun& in, GpuOut& out) override;
  void injectFailure(GpuFailure f, int count) override {
    injected_ = f;
    injectCount_ = count;
  }
  void release() override;

 private:
  Core& core() { return ctx_->core(); }
  hal::Device& dev() { return ctx_->core().dev(); }
  void releaseLocked();
  void checkEpoch(const GpuRun& in);

  // Device turns of a frame (section 52). A Turn is taken with the run's priority; on its end (while held, same device) the deferred
  // destruction is collected and the cache's device limit follows the pool. turnOk(): right after (re)taking it, the device is still the one
  // this frame's handles belong to (a rebuild in between: transient failure, the session retries). yieldTurn(): a background frame hands the
  // device to a waiting interactive frame between two of its jobs.
  class Turn {
   public:
    Turn(Executor& x, const GpuRun& in);
    ~Turn();
    void release() { t_.release(); }
    void acquire() { t_.acquire(); }

   private:
    Executor& x_;
    const GpuRun& in_;
    DeviceTurn t_;
  };
  bool turnOk();
  bool yieldTurn();

  // failure reporting: the class follows the reason (a device error is classified by its hal::Status)
  bool fail(GpuReason why, const std::string& msg);
  bool failStatus(hal::Status s, const std::string& msg);
  Status failed(Status s) const { return failure_ == GpuFailure::Unsupported ? Status::Unsupported : s; }
  // a failed recording: unsupported when a record function flagged it (GpuRecorder::unsupported), else transient
  bool failRecord(const GpuRecorder& rec, const std::string& err);

  std::uint64_t residentLimit();
  bool canResident(int w, int h);
  // a fenced submission of `g` (the deferred destruction learns of it)
  hal::Status submit(GpuGraph& g, hal::CommandList& cl, hal::FenceH* fence, std::string& err);
  void recorderFor(GpuRecorder& rec, int slot);

  // device copies (gpu_device_copies.cpp)
  void bindSource(SourceStore& s);
  bool ensureResident();
  std::shared_ptr<const hal::ImageH> resizedSource(int w, int h);  // the resident source or a resized variant, held
  std::shared_ptr<DeviceTables> ensureTables(const GpuRun& in);
  // the HDR buffer of the point pass with the reshaping field appended (section 45): HDR grid (when active) then the field; `warpOff` =
  // the field's float offset
  std::shared_ptr<DeviceBuffer> ensureGeoAux(const GpuRun& in, int& warpOff);
  std::shared_ptr<DeviceBuffer> ensureBuffer(ResultCache& cache, CacheSlot slot, std::uint64_t key, const void* data, std::size_t bytes);
  bool ensureLocal(const GpuRun& in, std::shared_ptr<DeviceBuffer>& desc, std::shared_ptr<DeviceBuffer>& planes);
  LutBind lutBind(GpuGraph& g, const DeviceTables& t);
  bool filmResolve(const GpuRun& in, filmnode::FilmResolved& r);
  std::shared_ptr<DeviceFilm> ensureFilm(const filmnode::FilmResolved& r);
  std::shared_ptr<DeviceDisplay> ensureDisplay(const GpuRun& in);
  bool upload(hal::BufferH dst, const void* data, std::size_t bytes);

  Status runWhole(const GpuRun& in, GpuOut& out);
  Status runTiled(const GpuRun& in, GpuOut& out);
  bool validate(const GpuRun& in);

  std::shared_ptr<Context> ctx_;
  std::mutex runMtx_;  // the executor's own state across a frame's turns (run vs release / destruction from another thread)
  DeviceGate::Prio prio_ = DeviceGate::Prio::Background;
  DeviceTurn* turn_ = nullptr;  // the running frame's turn (yieldTurn)
  std::uint64_t epoch_ = 0;
  SourceStore* store_ = nullptr;
  std::uint64_t srcId_ = 0;
  int W_ = 0, H_ = 0;
  std::shared_ptr<ResidentSource> rs_;
  std::shared_ptr<DeviceFilm> film_;
  // a tiled export's two readback buffers (a tile waits in one for the sink, outside the device turn, while the next is written into the
  // other); grown on demand, kept for the next export of this session
  hal::BufferH tileRb_[2];
  filmnode::FilmResolved lastFilmR_{};  // the last film frame that rendered: the stand-in while another stock's tables are baked in the background
  bool haveLastFilmR_ = false;

  GpuFailure failure_ = GpuFailure::None;
  GpuReason reason_ = GpuReason::None;
  std::string err_;
  GpuFailure injected_ = GpuFailure::None;
  int injectCount_ = 0;
};

}  // namespace dapple::engine::gpu
