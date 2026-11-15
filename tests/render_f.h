#pragma once
// Test helper: the exact-quality float render of one edit through the engine (Planner + CpuExecutor), the one-shot render the unit tests use.
// A failure aborts the test binary with the engine's message.
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

// One frame of the engine's baked variant (the app's preview / final frame: a colour lattice of `n` nodes and the 1D tone table, FastOptions as the C layer
// passes them), as float. EngineSession::renderFrame on the CPU executor; a failure aborts with the engine's message.
inline ImageF renderFrameF(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, int n, const FastOptions& fo,
                          engine::EngineSession* shared = nullptr) {
  engine::point::Params point = engine::point::resolve(params, camera, lut);
  engine::EngineSession own;
  engine::EngineSession& session = shared ? *shared : own;  // a shared session keeps its result cache between frames (lattice reuse)
  engine::FrameJob job{params, point, src, nullptr, camera, lut, {}, {}, {}};
  job.request.bakeSize = n;
  job.request.fastAnalysis = false;
  job.request.noise = fo.noise;
  job.request.interactive = fo.spatial == SpatialQuality::Interactive;
  job.fastOptions = fo;
  ImageF out;
  job.outFloat = &out;
  engine::FrameResult res;
  if (!session.renderFrame(job, res)) {
    std::fprintf(stderr, "renderFrameF: engine frame failed: %s\n", res.error.c_str());
    std::abort();
  }
  return out;
}

// The same frame as malloc()ed RGBA8 (the caller free()s it), the 8-bit result of the C layer's frame path.
inline std::uint8_t* renderFrame8(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, int n, const FastOptions& fo,
                                  int& w, int& h) {
  engine::point::Params point = engine::point::resolve(params, camera, lut);
  engine::EngineSession session;
  engine::FrameJob job{params, point, src, nullptr, camera, lut, {}, {}, {}};
  job.request.bakeSize = n;
  job.request.fastAnalysis = false;
  job.request.noise = fo.noise;
  job.request.interactive = fo.spatial == SpatialQuality::Interactive;
  job.fastOptions = fo;
  engine::FrameResult res;
  if (!session.renderFrame(job, res)) {
    std::fprintf(stderr, "renderFrame8: engine frame failed: %s", res.error.c_str());
    std::fputc(10, stderr);
    std::abort();
  }
  w = res.w;
  h = res.h;
  return res.rgba;
}

// The same, 8-bit (the final-quality frame of one edit; the engine's exact variant).
inline Image8 render8(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src) {
  Image8 out;
  std::string err;
  if (!engine::renderImage8(params, camera, lut, src, out, &err)) {
    std::fprintf(stderr, "render8: engine render failed: %s\n", err.c_str());
    std::abort();
  }
  return out;
}

}  // namespace dapple
