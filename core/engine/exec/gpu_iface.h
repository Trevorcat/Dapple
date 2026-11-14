#pragma once
// The seam between EngineSession (dapple_core) and the GPU executor (dapple_gpu, optional library).
//
//   EngineSession::renderFull / exportBaked
//        Planner::plan  ->  Plan
//        IGpuExecutor::run(GpuRun)     <- Vulkan, or GLES 3.1 when no Vulkan device passed its self-test
//        CpuExecutor::run              <- the last resort, and the reference
//
// Selection and the failure semantics of docs/engine/pipeline-architecture.md 5.9 live in the session (engine_session.cpp);
// the executor only reports what happened (Status + GpuFailure). The provider is installed by the GPU library
// (gpu::installProvider(), called by the C API in a build with DAPPLE_GPU and by the GPU tests).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dapple/frame_plan.h"
#include "dapple/renderer.h"
#include "engine/exec/source_store.h"
#include "engine/graph/graph.h"
#include "engine/hal/hal.h"
#include "engine/plan/plan.h"

namespace dapple::engine {

class ResultCache;

// What a failed run means for the selection (5.9).
//   Unsupported  this plan cannot run on the GPU (a node without GpuImpl, a film frame, limits): CPU for this frame, GPU stays
//   Transient    OOM / timeout / failed submission: CPU for this frame, pools released, GPU stays
//   DeviceLost   the device is gone: rebuild once (re-upload from SourceStore + tables), second loss -> next API / CPU
enum class GpuFailure : std::uint8_t { None, Unsupported, Transient, DeviceLost };

// Latest-wins cancellation (5.8), same contract as CancelToken (cpu_executor.h): polled between tiles and sweeps.
struct GpuCancel {
  const std::atomic<std::uint64_t>* current = nullptr;
  std::uint64_t mine = 0;
  bool cancelled() const { return current && current->load(std::memory_order_relaxed) != mine; }
};

struct GpuStats {
  double wallMs = 0, gpuMs = 0, uploadMs = 0, readbackMs = 0, recordMs = 0, submitMs = 0, waitMs = 0, trimMs = 0;
  int passes = 0, barriers = 0, parts = 0, tiles = 0, sweeps = 0;
  std::vector<std::pair<std::string, double>> passTimes;  // profile only
};

// Where the final 8-bit picture goes.
struct GpuTarget {
  // default: readback into a malloc()ed RGBA8 buffer (GpuOut::rgba)
  // external: the final pass is copied into an image owned by someone else (the presenter's swapchain image); no readback
  const hal::ExternalImage* external = nullptr;
  // TiledExport: receives each finished tile (RGBA8, `stride` bytes per row); return false to abort
  std::function<bool(int x, int y, int w, int h, const std::uint8_t* rgba, std::size_t stride)> sink;
};

struct GpuRun {
  const Plan& plan;
  const Graph& graph;
  const FramePlan& frame;       // resolved frame (geometry, gain, neighbourhood amounts) for the source size the plan was made for
  SourceStore& store;           // full-resolution source; the Resize node scales it to frame.srcW x frame.srcH
  const BakedLut& bake;         // HostTable payloads: lattice + tone table
  const std::vector<float>* overflow = nullptr;  // LatticeOverflow payload (33^3 RGB), null = compute from the store
  const Renderer* renderer = nullptr;            // only for the overflow lattice when `overflow` is null
  float resizeSharpen = 0.f;
  GpuCancel cancel;
  GpuTarget target;
  bool forceStream = false;     // tests: never keep the source resident (streaming path of a tiled export)
};

struct GpuOut {
  std::uint8_t* rgba = nullptr;  // malloc()ed, caller free()s (readback target, Whole mode)
  int w = 0, h = 0;
  GpuFailure failure = GpuFailure::None;
  std::string error;
  GpuStats stats;
};

class IGpuExecutor {
 public:
  virtual ~IGpuExecutor() = default;
  virtual hal::Api api() const = 0;
  virtual const hal::DeviceCaps& caps() const = 0;
  // Runs every stage of `run.plan`. Ok, Unsupported (CPU takes the frame), Cancelled, OutOfMemory (transient); out.failure says
  // why a run failed.
  virtual Status run(const GpuRun& run, GpuOut& out) = 0;
  // Test seam: the next `count` runs fail with this classification before touching the device.
  virtual void injectFailure(GpuFailure f, int count) = 0;
  // Releases the device copies (resident source, lattices) of this session.
  virtual void release() = 0;
  // The frame-global solves (Dehaze / Detail node keys) this executor keeps on the device: pass it to Planner::plan so that a frame whose solve is
  // still there is planned without the accumulation sweep and the solve. Null: the executor keeps none.
  virtual const ResultCache* solveCache() const { return nullptr; }
};

// Process-wide device selection (Vulkan -> GLES 3.1 -> CPU), created by the GPU library.
class GpuProvider {
 public:
  virtual ~GpuProvider() = default;
  virtual bool enabled() const = 0;  // runtime flag (dapple_gpu_set_enabled / DAPPLE_GPU / debug.dapple.gpu)
  // Bumps whenever the active device changed (rebuild, demotion): executors of older generations must be replaced.
  virtual std::uint64_t generation() = 0;
  // A new per-session executor on the active device; null (why set) when no device is usable.
  virtual std::unique_ptr<IGpuExecutor> createExecutor(std::string& why) = 0;
  // Rebuild after a device loss: true = the same API is usable again (generation bumped), false = this API is given up and the
  // next candidate (or none) is active now.
  virtual bool recover(std::string& why) = 0;
  virtual std::string statusJson() = 0;
  virtual std::size_t pooledBytes() = 0;
  // Zero-readback seam for the presenter: native handles of the active Vulkan device (null/false on GLES).
  virtual bool nativeHandles(hal::NativeHandles* out) = 0;
  // Adopted-device mode of the presenter (docs/engine/present.md): the native handles plus what makes using them safe.
  //   keepAlive   holds the context (VkInstance / VkDevice) alive for as long as the presenter has objects on it, also after a
  //               demotion / rebuild replaced it in the provider
  //   queueMutex  serialises every use of handles.queue (vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle) with the executor,
  //               which holds it for a whole frame
  //   generation  changes when the device behind the handles was replaced
  // False on GLES, when the GPU is off or unavailable, or when the device was created without the present extensions.
  struct DeviceLease {
    hal::NativeHandles handles;
    std::shared_ptr<void> keepAlive;
    std::mutex* queueMutex = nullptr;
    std::uint64_t generation = 0;
  };
  virtual bool lease(DeviceLease* out) { (void)out; return false; }
  // Why enabled() is false (diagnostics, FrameResult::why): "GPU disabled (debug.dapple.gpu=0)", "GPU self-test running", "GPU self-test failed: ...".
  virtual std::string inactiveReason() { return "GPU disabled"; }
  // Memory (pipeline-architecture.md 42.9): frees the free images of the active device's pool beyond `keepFree` bytes (0 = all) and the
  // device's descriptor caches; returns the bytes released. Waits for the frame in flight (context mutex).
  virtual std::size_t trimPool(std::size_t keepFree) { (void)keepFree; return 0; }
  // The active device: unified memory?, the pool's budget, and the driver's budget / usage of the device-local heaps where the backend
  // reports them (VK_EXT_memory_budget; *budget = 0 when unknown). False = no active device.
  struct MemoryInfo {
    bool unified = false;
    std::uint64_t poolBudget = 0, pooled = 0, budget = 0, usage = 0, deviceLocal = 0;
  };
  virtual bool memoryInfo(MemoryInfo* out) { (void)out; return false; }
};

void installGpuProvider(GpuProvider* p);
GpuProvider* gpuProvider();

}  // namespace dapple::engine
