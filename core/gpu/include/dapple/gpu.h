#pragma once
// Dapple GPU compute backend (docs/engine/gpu-backend.md): Vulkan 1.0 compute, no windowing. Built only with
// -DDAPPLE_GPU=ON (default OFF). The CPU renderer (core/src/renderer.cpp) stays the reference and the fallback:
// every entry point reports failure instead of throwing and the caller renders on the CPU.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dapple/image.h"
#include "dapple/renderer.h"

namespace dapple::gpu {

struct DeviceInfo {
  std::string name;
  std::string driver;
  std::string memoryTypes;  // one line per Vulkan memory type: heap, size, property flags (diagnostics)
  std::uint32_t apiVersion = 0, driverVersion = 0, vendorId = 0, deviceId = 0, deviceType = 0;
  std::uint32_t maxImageDim2D = 0, maxImageDim3D = 0, maxSharedMem = 0, maxInvocations = 0;
  std::uint32_t subgroupSize = 0, subgroupOps = 0;
  bool fp16Arith = false, storage16 = false, linearFilterRgba32f = false, timestamps = false, unifiedMemory = false;
  std::uint64_t deviceLocalBytes = 0, maxAllocation = 0;
  std::string text() const;
  // vendor:device:driverVersion, what a host app can persist to remember a failed self-test
  std::string key() const;
};

struct BackendOptions {
  bool allowSoftware = false;       // accept CPU Vulkan implementations (SwiftShader, llvmpipe): tests / emulator only
  int deviceIndex = -1;             // -1 = best; env DAPPLE_GPU_DEVICE overrides
  bool selfTest = true;             // render a known pattern and compare with the CPU reference before first use
  bool profile = false;             // GPU timestamps per pass (FrameStats::passes)
  std::string blocklist;            // extra block rules, see blocklist in docs/engine/gpu-backend.md (also env DAPPLE_GPU_BLOCKLIST)
  std::string pipelineCachePath;    // persisted VkPipelineCache (empty = none)
  std::size_t poolBudgetBytes = 0;  // free working-image pool (0 = automatic)
};

struct PassTime {
  std::string name;
  double ms = 0;
};

struct FrameStats {
  double wallMs = 0;      // record + submit + wait
  double gpuMs = 0;       // sum of the pass timestamps (profile only)
  double uploadMs = 0;    // source / lattice upload (wall)
  double readbackMs = 0;  // copy to the caller's buffer (malloc + memcpy out of the mapped readback buffer)
  double recordMs = 0;    // building the pass graph and recording (host)
  double submitMs = 0;    // vkQueueSubmit
  double trimMs = 0;      // pool trimming after the frame
  double waitMs = 0;      // fence wait (GPU execution as seen by the host)
  int passes = 0, barriers = 0;
  std::vector<PassTime> passTimes;  // profile only
};

struct FrameOptions {
  int srcLongEdge = 0;        // resize the source (Lanczos-3) to this long edge first; 0 or >= its own = natural size
  float resizeSharpen = 0.f;  // extra sharpening amount when the frame is a downscale (export), same radius as `sharpen`
};

struct ExportOptions {
  int tile = 2048;            // interior tile edge (rounded to a multiple of 4)
  bool forceStream = false;   // never keep the source resident (tests of the streaming path)
  FrameOptions frame;
  // a tile of the final picture: pixels (x, y, w, h) of an RGBA8 buffer with `stride` bytes per row. Return false to abort.
  std::function<bool(int x, int y, int w, int h, const std::uint8_t* rgba, std::size_t stride)> sink;
};

class Session;

// The device, its pipelines and the pooled resources; one per process (see sharedBackend()).
class Backend : public std::enable_shared_from_this<Backend> {
 public:
  ~Backend();
  // Null (and `err` set) when there is no usable device: no Vulkan, blocked device, failed self-test, ...
  static std::shared_ptr<Backend> create(const BackendOptions& opt, std::string& err);

  const DeviceInfo& info() const;
  const std::string& selfTestReport() const;  // one line per check (mean / max dE2000)
  std::unique_ptr<Session> createSession();
  // Device memory currently held by pooled working images (bytes).
  std::size_t pooledBytes() const;

  struct Impl;

 private:
  Backend();
  std::unique_ptr<Impl> d_;
  friend class Session;
};

// Per-photo state: the resident source (uploaded once per photo, resized copies cached per size) and the lattice of
// the last bakes. Not thread safe; the caller serialises (the C API holds the session mutex).
class Session {
 public:
  ~Session();
  // Identity = object address + size + `stamp`; a repeat call with the same identity is free.
  bool setSource(const SourceImage& src, std::uint64_t stamp, std::string& err);
  bool hasSource() const;
  // True when the resident-source render() can serve a source of this size on this device (image limits, memory).
  bool canRenderResident(int w, int h) const;

  // Output size of render() for the given request (CPU Renderer::fastSize semantics, resized source applied).
  bool frameSize(const Renderer& r, const FastOptions& opt, const FrameOptions& fo, int& w, int& h) const;

  // Same contract as Renderer::renderBakedRgba: malloc()ed RGBA8 (alpha 255), caller free()s; null (err set) when the
  // frame cannot be rendered on the GPU (film simulation, source too large, device lost, ...).
  std::uint8_t* render(const Renderer& r, const BakedLut& lut, const FastOptions& opt, const FrameOptions& fo, int& w,
                       int& h, FrameStats* stats, std::string& err);

  // Full-picture export in tiles (interior + halo; large-scale operators are analysed once for the whole picture), so
  // 60 MP fits mobile memory and the maximum image size. The source is streamed per tile when it is not resident.
  bool exportTiled(const Renderer& r, const SourceImage& src, const BakedLut& lut, const FastOptions& opt,
                   const ExportOptions& eo, FrameStats* stats, std::string& err);

  struct Impl;

 private:
  explicit Session(std::shared_ptr<Backend> b);
  std::unique_ptr<Impl> d_;
  friend class Backend;
};

// ---- process-wide runtime flag + shared backend (what the C API hook uses) ------------------------------------
// Enabled when (in this order) dapple_gpu_set_enabled() was called, env DAPPLE_GPU=1, or on Android the system
// property debug.dapple.gpu=1. Default: off.
bool runtimeEnabled();
void setRuntimeEnabled(int enabled);  // 0 off, 1 on, -1 back to environment / property
// The lazily created backend (self-test included); null when disabled or unusable, `why` then says so. A failure is
// remembered: it is not retried in the same process.
std::shared_ptr<Backend> sharedBackend(std::string* why = nullptr);

int gpuBench(const std::vector<std::string>& args);

}  // namespace dapple::gpu
