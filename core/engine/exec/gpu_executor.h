#pragma once
// GpuExecutor core (step b2/b3): the device, the transient pool, the per-frame-slot host buffers and the shared
// reduction buffers. One per process (owned by gpu::Backend); rebuilt after a device loss (pipeline-architecture.md 5.9).
//
// Frame execution itself lives in gpu_executor.cpp (gpu::Session: resident source, lattice cache, whole-frame render, tiled
// export) and is built from the node record functions through exec/gpu_frame.h and the scheduler in exec/gpu_scheduler.h.
#include <memory>
#include <string>

#include "engine/exec/gpu_scheduler.h"
#include "engine/hal/hal.h"

namespace dapple::engine::gpu {

// Host-visible buffers tied to a frame slot: valid to touch once Device::begin() returned that slot.
struct SlotRes {
  hal::BufferH staging;   // host -> device uploads (grown on demand)
  hal::BufferH readback;  // device -> host (grown on demand)
  hal::BufferH weights;   // filter weight table
};

class Core {
 public:
  static std::unique_ptr<Core> create(const hal::DeviceOptions& opt, std::string& err);
  ~Core();
  hal::Device& dev() { return *dev_; }
  GpuPool& pool() { return *pool_; }
  SlotRes& slot(int i) { return slots_[i]; }
  bool ensureStaging(int slot, std::uint64_t bytes, std::string& err);
  bool ensureReadback(int slot, std::uint64_t bytes, std::string& err);
  hal::BufferH gBuf, partBuf;  // dehaze statistics + reduction partials (device local)
  bool boxScanOk = true;       // the running-sum box passed its start-up check on this device (else the direct sum is used)
  // Frame statistics of a finished submission
  struct Stats {
    double gpuMs = 0;
    int passes = 0, barriers = 0;
    std::vector<hal::PassTime> times;
  };
  Stats statsOf(hal::FenceH f);
  // Uploads packed float RGB rows of `rgb` (row pitch `pitchFloats`) into `dst` at (dx, dy) through the convert pass.
  bool uploadRgbF32(const float* rgb, std::size_t pitchFloats, int w, int h, const hal::ImageH& dst, int dx, int dy, std::string& err);

 private:
  Core() = default;
  std::unique_ptr<hal::Device> dev_;
  std::unique_ptr<GpuPool> pool_;
  SlotRes slots_[hal::Device::kSlots];
};

}  // namespace dapple::engine::gpu
