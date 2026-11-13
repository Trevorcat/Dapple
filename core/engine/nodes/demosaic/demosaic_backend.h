#pragma once
// Seam between the decoder (dapple_core) and the GPU Demosaic runner (dapple_gpu, optional library): the decoder asks for a GPU demosaic of a
// rectangle, the GPU library installs the implementation (demosaic::installGpuBackend(), called by the C API next to gpu::installProvider()).
// Without a backend, with the GPU runtime switch off, or when the device fails, the decoder runs the CPU node.
#include <cstddef>
#include <string>

#include "engine/nodes/demosaic/demosaic.h"

namespace dapple::demosaic {

class GpuBackend {
 public:
  virtual ~GpuBackend() = default;
  // True when the GPU may be used now (runtime switch on and a device usable). `why` says why not.
  virtual bool available(std::string& why) = 0;
  // Whether the GPU is the faster node for this work on this device (measured once per device, see demosaic_gpu.cpp; separately for a whole frame and for a
  // region). The decoder runs the CPU node when this says no: the slower node is never used. Default: yes.
  virtual bool prefer(const Desc& d, Rect roi, std::string& why) { (void)d, (void)roi, (void)why; return true; }
  // Demosaic `roi` into out (interleaved float RGB). False = failed / unavailable: the caller falls back to the CPU node.
  virtual bool run(const Desc& d, Rect roi, float* out, std::size_t strideFloats, std::string& why) = 0;
};

void setGpuBackend(GpuBackend* b);
GpuBackend* gpuBackend();

#ifdef DAPPLE_HAVE_GPU
// Defined in dapple_gpu (demosaic_gpu.cpp): creates the backend (own device, created on first use) and registers it. Idempotent.
void installGpuBackend();
#endif

}  // namespace dapple::demosaic
