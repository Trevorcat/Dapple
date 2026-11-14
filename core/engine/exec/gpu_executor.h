#pragma once
// GpuExecutor: runs a Plan (plan/plan.h) on a HAL device (docs/engine/pipeline-architecture.md 5.1 - 5.9, step b3).
//
//   gpu::Core      device + transient pool + per-frame-slot host buffers (one per device generation)
//   gpu::Context   process-wide: backend selection (Vulkan -> GLES 3.1), start-up self-test, device rebuild, epoch
//   gpu::Executor  per EngineSession (IGpuExecutor): consumes the Plan's stages, keeps the device copies of the source
//                  (fed by a SourceStore) and of the tables (on a ResultCache), records through gpuImplFor(...).recordPorts
//
// Which executor runs a frame, and what a failure means, is decided by EngineSession (exec/engine_session.cpp, 5.9).
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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
  // Gives the slots' grown host buffers back (staging to its initial 8 MB, readback freed; both grow again on use). The device must be idle and
  // no frame in flight (memory trim: the context mutex held, after waitIdle). Returns the bytes released.
  std::size_t trimStaging();
  hal::BufferH gBuf, partBuf;  // dehaze statistics + reduction partials (device local)
  bool boxScanOk = true;       // the running-sum box passed its start-up check on this device (else the direct sum is used)
  // Frame statistics of a finished submission
  struct Stats {
    double gpuMs = 0;
    int passes = 0, barriers = 0, parts = 1;
    std::vector<hal::PassTime> times;
  };
  Stats statsOf(hal::FenceH f);
  // Uploads packed float RGB rows of `rgb` (row pitch `pitchFloats`) into `dst` at (dx, dy) through the convert pass.
  bool uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, const hal::ImageH& dst, int dx, int dy, std::string& err);

 private:
  Core() = default;
  bool init(std::string& err);
  std::unique_ptr<hal::Device> own_;
  hal::Device* dev_ = nullptr;
  std::unique_ptr<GpuPool> pool_;
  SlotRes slots_[hal::Device::kSlots];
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
  Core& core();
  std::size_t pooledBytes() const;
  std::unique_ptr<IGpuExecutor> createExecutor();
  // Device loss (5.9): builds a fresh device of the same API once; executors drop their device copies and re-create them lazily
  // from their SourceStore / tables. False when the rebuild or its self-test failed or was already done once.
  bool rebuild(std::string& err);
  int rebuilds() const;
  bool deviceLost() const;
  std::uint64_t epoch() const;
  std::mutex& mutex();
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
