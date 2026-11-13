#pragma once
// GPU Demosaic (docs/engine/pipeline-architecture.md section 32): the same node as demosaic_cpu.cpp on any compute HAL device (Vulkan, GLES 3.1,
// Metal through the shared single-source GLSL). Tiled: each tile is a window of the mosaic (tile + halo, coordinate mapped on the host) uploaded as
// packed 16-bit words, scaled (black, white, WB, clip) into a float plane, demosaiced in 2 - 3 passes and written into an RGBA16F / RGBA32F image
// that is read back here (the engine integration keeps it on the device and feeds the point chain instead: see the hunk in the docs).
#include <memory>
#include <string>
#include <vector>

#include "engine/hal/hal.h"
#include "engine/nodes/demosaic/demosaic.h"

namespace dapple::demosaic {

struct GpuStats {
  double totalMs = 0;     // wall time of run()
  double prepMs = 0;      // host: window build + staging copy, all tiles
  double gpuMs = 0;       // submit + wait, all tiles (upload copy + passes + output copy)
  double readbackMs = 0;  // host: conversion of the read-back tiles
  int tiles = 0;
  std::uint64_t deviceBytes = 0;  // device memory allocated for the planes and the output tile
  hal::SubmitStats submit;
  std::vector<hal::PassTime> passTimes;  // per pass GPU ms summed over the tiles (only with DeviceOptions::profile and a device with timestamps)
};

class GpuDemosaic {
 public:
  // `half`: the output tile image is RGBA16F (the pipeline format, rounds like the other kernels) instead of RGBA32F (parity with the CPU node).
  static std::unique_ptr<GpuDemosaic> create(hal::Device& dev, bool half, std::string& err);
  ~GpuDemosaic();
  // Demosaic `roi` of `d` in tiles of at most tile x tile output pixels. `out` (interleaved RGB float, row stride strideFloats) may be null: the
  // result then stays on the device and is dropped (timing of the compute alone).
  hal::Status run(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int tile, GpuStats* st = nullptr);
  const std::string& error() const { return err_; }

 private:
  GpuDemosaic() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
  std::string err_;
};

}  // namespace dapple::demosaic
