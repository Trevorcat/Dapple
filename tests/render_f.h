#pragma once
// Test helper: the exact-quality float render of one edit through the engine (Planner + CpuExecutor), the replacement for
// `Renderer(params, camera, lut).renderFloat(source, spatial)` in the unit tests (docs/engine/old-pipeline-removal.md, step 3).
// Bit-identical to the old call on the parameter sets the golden selfcheck covers; a failure aborts the test binary with the engine's message.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine/exec/engine_session.h"

namespace dapple {

inline ImageF renderF(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, bool spatial = true) {
  ImageF out;
  std::string err;
  if (!engine::renderImageF(params, camera, lut, src, spatial, out, &err)) {
    std::fprintf(stderr, "renderF: engine render failed: %s\n", err.c_str());
    std::abort();
  }
  return out;
}

}  // namespace dapple
