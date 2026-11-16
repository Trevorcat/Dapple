#pragma once
// GpuExecutor: runs a Plan (plan/plan.h) on a HAL device (docs/engine/pipeline-architecture.md 5.1 - 5.9, section 50).
//
//   gpu::Core      device + transient pool + per-frame-slot host buffers + deferred destruction (one per device generation; gpu_core.cpp)
//   gpu::DeviceGate who may call the device now: one turn per job (a submission, an upload chunk, an export tile), interactive first (52)
//   gpu::Context   process-wide: backend selection (Vulkan -> GLES 3.1), start-up self-test, device rebuild, epoch (gpu_context.cpp)
//   gpu::Executor  per EngineSession (IGpuExecutor): runs the Plan's stages over the plan's tiles (gpu_executor.cpp), keeps the device copies
//                  of the source, tables, solves and buffers in the session's ResultCache (gpu_device_copies.cpp), records a job (a frame or
//                  a tile) through the nodes' GpuImpls (gpu_walk.cpp)
//
// Which executor runs a frame, and what a failure means, is decided by EngineSession (exec/engine_session.cpp, 5.9).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/exec/gpu_iface.h"
#include "engine/exec/gpu_runtime.h"
#include "engine/exec/gpu_scheduler.h"
#include "engine/hal/hal.h"

namespace dapple::engine::gpu {

// Host-visible buffers tied to a frame slot: valid to touch once Device::begin() returned that slot.
struct SlotRes {
  hal::BufferH staging;   // host -> device uploads (grown on demand)
  hal::BufferH readback;  // device -> host (grown on demand)
  hal::BufferH taps;      // film mixture tap tables (host written while recording, 256 KB)
  hal::BufferH weights;   // filter weight table
};

class Core {
 public:
  // Vulkan device created here (film tests, tools that want exactly Vulkan).
  static std::unique_ptr<Core> create(const hal::DeviceOptions& opt, std::string& err);
  // Takes over a device the selection produced.
  static std::unique_ptr<Core> adopt(std::unique_ptr<hal::Device> dev, std::string& err);
  // Non-owning view of a device (the start-up self-test runs on a device the selection still owns).
  static std::unique_ptr<Core> view(hal::Device& dev, std::string& err);
  ~Core();
  hal::Device& dev() { return *dev_; }
  GpuPool& pool() { return *pool_; }
  SlotRes& slot(int i) { return slots_[i]; }
  bool ensureStaging(int slot, std::uint64_t bytes, std::string& err);
  bool ensureReadback(int slot, std::uint64_t bytes, std::string& err);
  // Gives the slots' grown host buffers back (staging to its initial 8 MB, readback freed; both grow again on use). Inside a device turn (no
  // readback crosses a turn in a frame slot); waits for the device itself. Returns the bytes released.
  std::size_t trimStaging();
  hal::BufferH gBuf, partBuf;  // dehaze statistics + reduction partials (device local)
  std::atomic<double> upCopyMs_{0}, upSubmitMs_{0}, upWaitMs_{0};
  bool boxScanOk = true;       // the running-sum box passed its start-up check on this device (else the direct sum is used)
  // Frame statistics of a finished submission
  struct Stats {
    double gpuMs = 0;
    int passes = 0, barriers = 0, parts = 1;
    std::vector<hal::PassTime> times;
  };
  Stats statsOf(hal::FenceH f);
  // Uploads packed float RGB rows of `rgb` (row pitch `pitchFloats`) into `dst` at (dx, dy) through the convert pass, in submissions of
  // 24 MB. `between` (optional) runs between two submissions (a background upload hands the device to a waiting interactive frame there);
  // false stops the upload. waitDevice = false: do not wait for the copy on the CPU (the caller's next submission is ordered after it on the GPU: Vulkan, GLES).
  bool uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, const hal::ImageH& dst, int dx, int dy, std::string& err,
                    const std::function<bool()>& between = {}, bool waitDevice = true);
  // Where uploadRgbF32 spent its time since the process started (stage log of a streamed export: the host copy into the staging buffer, the convert pass's
  // recording and submission, the wait for the device); read as differences around an export
  struct UploadTimes {
    double copyMs = 0, submitMs = 0, waitMs = 0;
  };
  UploadTimes uploadTimes() const { return {upCopyMs_.load(), upSubmitMs_.load(), upWaitMs_.load()}; }

  // Deferred destruction (no waitIdle): a device resource released while work already submitted may still read it (a cache eviction, a
  // session closing) is destroyed once the first fenced submission after its release has completed. retire(): any thread. submitted():
  // every fenced submission (inside a device turn). collect(): destroys what is safe (inside a device turn; `all` after the device went
  // idle); returns the bytes freed.
  void retire(hal::ImageH h);
  void retire(hal::BufferH h);
  void submitted(hal::FenceH f);
  std::size_t collect(bool all = false);
  std::size_t retiredCount();

 private:
  Core() = default;
  bool init(std::string& err);
  std::unique_ptr<hal::Device> own_;
  hal::Device* dev_ = nullptr;
  std::unique_ptr<GpuPool> pool_;
  SlotRes slots_[hal::Device::kSlots];
  struct Retired {
    hal::ImageH image;
    hal::BufferH buffer;
    hal::FenceH after;
    bool fenced = false;
  };
  std::mutex retireMtx_;
  std::vector<Retired> retired_;
};

// The device is externally synchronised as a whole (no HAL call is thread-safe, every back end; pipeline-architecture.md 52). A *turn* is the
// right to call it for one job: one begin..submit (+ the fence wait and readback copy of a whole frame), one upload chunk, one export tile,
// one keep-warm unit, a trim, a rebuild. Interactive waiters are served first (FIFO among them), then background waiters (FIFO): a drag frame
// waits for the job in flight, never for a whole background frame. The holder also holds queueMutex(), the lock the presenter takes alone
// for its own use of the VkQueue (the lease). Lock order: Executor::runMtx_ -> gate -> queueMutex -> leaves (retire, cache, sources).
// A turn holds only device work: host work that takes as long as a job (a band demosaic of a streamed export, an encoder) runs outside it.
// DAPPLE_GPU_TURNLOG=<ms>: every turn held longer than that is logged with what held it (`what`).
class DeviceGate {
 public:
  enum class Prio : int { Background = 0, Interactive = 1 };
  void lock(Prio p, const char* what = nullptr);
  void unlock();
  bool interactiveWaiting();
  std::mutex& queueMutex() { return queue_; }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool held_ = false;
  std::uint64_t next_[2] = {0, 0}, serving_[2] = {0, 0};  // tickets per priority
  std::mutex queue_;
  const char* what_ = nullptr;   // the holder (log)
  std::int64_t since_ = 0;       // steady clock ns of the holder's lock (log)
};

// RAII turn. yield(): a background holder hands the device to a waiting interactive turn and takes it back (true when it did; device state
// the holder did not keep alive may have changed meanwhile, the epoch included). release() / acquire(): give the turn up for a while.
// waitedMs(): the time this turn waited for the gate in all, so a frame's stage times do not count the wait as their own.
class DeviceTurn {
 public:
  DeviceTurn(DeviceGate& g, DeviceGate::Prio p, const char* what = nullptr) : g_(g), p_(p), what_(what) { lockTimed(); }
  ~DeviceTurn() {
    if (held_) g_.unlock();
  }
  DeviceTurn(const DeviceTurn&) = delete;
  DeviceTurn& operator=(const DeviceTurn&) = delete;
  bool yield() {
    if (!held_ || p_ == DeviceGate::Prio::Interactive || !g_.interactiveWaiting()) return false;
    g_.unlock();
    lockTimed();
    return true;
  }
  void release() {
    if (held_) g_.unlock(), held_ = false;
  }
  void acquire() {
    if (!held_) lockTimed(), held_ = true;
  }
  bool held() const { return held_; }
  double waitedMs() const { return waitedMs_; }

 private:
  void lockTimed();
  DeviceGate& g_;
  DeviceGate::Prio p_;
  const char* what_;
  bool held_ = true;
  double waitedMs_ = 0;
};

struct ContextOptions {
  hal::DeviceOptions device;  // allowSoftware, deviceIndex, profile, blocklist, pipelineCachePath, poolBudgetBytes, presentExtensions
  bool selfTest = true;       // render a known pattern through the whole pipeline before the first use
  std::string api;            // "" (env DAPPLE_GPU_API, then Vulkan -> GLES), "vulkan", "gles"
  // Persisted self-test verdicts (the provider keys them by Build.FINGERPRINT + app version; `devKey` = DeviceCaps::key(), i.e. includes the driver
  // version). lookup: 1 = passed before (box scan verdict in *boxOk), -1 = failed before (*detail), 0 = unknown (run the self-test).
  std::function<int(const std::string& devKey, bool* boxOk, std::string* detail)> cacheLookup;
  std::function<void(const std::string& devKey, bool ok, bool boxOk, const std::string& detail)> cacheStore;
};

class Context : public std::enable_shared_from_this<Context> {
 public:
  // Null (err set) when no backend is usable: no device, blocked, failed self-test, ...
  static std::shared_ptr<Context> create(const ContextOptions& opt, std::string& err);
  ~Context();

  hal::Api api() const;
  const hal::DeviceCaps& caps() const;
  std::string deviceText() const;           // caps text + the workgroup shape the kernels use
  const std::string& selfTestReport() const;
  std::string selectionJson() const;        // hal::Selection::statusJson of the creation (attempts, reasons)
  Core& core();  // the current device generation (any thread; use the device only inside a turn)
  std::size_t pooledBytes() const;
  std::unique_ptr<IGpuExecutor> createExecutor();
  // Device loss (5.9): builds a fresh device of the same API once, in a turn of its own; executors drop their device copies and re-create
  // them lazily from their SourceStore / tables (a frame between two of its turns stops, see Executor::turnOk). False when the rebuild or
  // its self-test failed or was already done once.
  bool rebuild(std::string& err);
  int rebuilds() const;
  bool deviceLost() const;
  std::uint64_t epoch() const;
  DeviceGate& gate();
  // The shared device copy of a SourceStore (executors of one context reading the same store share it); `make` creates it.
  std::shared_ptr<void> residentFor(std::uint64_t identity, const std::function<std::shared_ptr<void>()>& make);
  const ContextOptions& options() const;
  // Writes the device pipeline cache (Vulkan) to options().device.pipelineCachePath (atomically); bytes written, 0 = nothing to write.
  std::size_t savePipelineCache();
  // Zero-readback seam (docs/engine/present.md): the handles of the Vulkan device behind the context, or false.
  bool nativeHandles(hal::NativeHandles* out) const;

  struct Impl;

 private:
  Context();
  std::unique_ptr<Impl> d_;
  friend class Executor;
};

// ---- process-wide runtime flag + provider (gpu_runtime.h) ----
// The lazily created context of the provider (self-test included); null when disabled or unusable, `why` then says so.
std::shared_ptr<Context> sharedContext(std::string* why = nullptr);

}  // namespace dapple::engine::gpu
