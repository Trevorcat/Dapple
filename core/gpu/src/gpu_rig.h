#pragma once
// Test / bench rig: one photo on the engine with a given GPU executor, driven through the public engine API
// (EngineSession::renderFrame / exportGpu -> Planner -> GpuExecutor). Shared by dapple_gpu_tests and `dapple gpu-bench`.
//
// A frame: tables are baked by the engine (lattice `latticeN`), `cpuSrc` is the CPU copy the bake and the frame size come from (the
// full source, or its CPU-scaled copy); the executor reads the full-resolution `store` and resizes it to `cpuSrc`'s size itself.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#include "engine/exec/gpu_executor.h"

namespace dapple::gpu {

// A 17^3 look LUT with cross-channel terms (not separable, not close to the identity): the display stages of the film tests apply it.
inline const Lut3D& syntheticLut() {
  static const Lut3D l = [] {
    std::string t = "LUT_3D_SIZE 17\n";
    char b[96];
    for (int k = 0; k < 17; ++k)
      for (int j = 0; j < 17; ++j)
        for (int i = 0; i < 17; ++i) {
          const float r = i / 16.f, g = j / 16.f, bl = k / 16.f;
          std::snprintf(b, sizeof b, "%.6f %.6f %.6f\n", 0.9f * std::pow(r, 0.9f) + 0.08f * g + 0.02f, 0.93f * g + 0.05f * bl * r + 0.03f,
                        0.82f * bl + 0.12f * r * g + 0.04f);
          t += b;
        }
    auto r = Lut3D::parseCube(t);
    if (!r) {
      std::fprintf(stderr, "synthetic LUT failed: %s\n", r.error().message.c_str());
      std::abort();
    }
    return std::move(*r);
  }();
  return l;
}

struct Rig {
  std::shared_ptr<engine::gpu::Context> ctx;
  std::unique_ptr<engine::IGpuExecutor> ex;
  engine::EngineSession es;
  engine::MemorySourceStore store;
  const CameraProfile& cam;

  Rig(std::shared_ptr<engine::gpu::Context> c, const SourceImage& full, const CameraProfile& camera)
      : ctx(std::move(c)), ex(ctx->createExecutor()), store(full), cam(camera) {
    es.useGpuExecutor(ex.get(), true);  // forced: a plan the GPU cannot run is an error here, not a CPU fallback
  }

  // malloc()ed RGBA8 (alpha 255) like Renderer::renderBakedRgba, or null (err set)
  std::uint8_t* render(const EditParams& p, Renderer& r, const SourceImage& cpuSrc, const FastOptions& fo, int latticeN, int& w, int& h,
                       engine::GpuStats* st, std::string& err, int srcLongEdge = 0, float resizeSharpen = 0.f, const Lut3D* lut = nullptr) {
    engine::FrameJob job{p, r, cpuSrc, nullptr, cam, lut, {}, {}, {}};
    job.request.bakeSize = latticeN;
    job.request.fastAnalysis = false;
    job.request.noise = fo.noise;
    job.fastOptions = fo;
    job.store = &store;
    job.resizeSharpen = resizeSharpen;
    job.srcLongEdge = srcLongEdge;
    engine::FrameResult res;
    if (!es.renderFrame(job, res)) return err = res.error.empty() ? "render failed" : res.error, nullptr;
    w = res.w;
    h = res.h;
    if (st) *st = res.gpu;
    return res.rgba;
  }

  // Zero readback: the frame is copied into an externally owned image (hal::ExternalImage). True = the GPU did it.
  bool renderTo(const hal::ExternalImage& ext, const EditParams& p, Renderer& r, const SourceImage& cpuSrc, const FastOptions& fo, int latticeN, int& w, int& h,
                std::string& err) {
    engine::FrameJob job{p, r, cpuSrc, nullptr, cam, nullptr, {}, {}, {}};
    job.request.bakeSize = latticeN;
    job.request.fastAnalysis = false;
    job.request.noise = fo.noise;
    job.fastOptions = fo;
    job.store = &store;
    job.gpuTarget = &ext;
    engine::FrameResult res;
    if (!es.renderFrame(job, res)) return err = res.error.empty() ? "render failed" : res.error, false;
    w = res.w;
    h = res.h;
    std::free(res.rgba);
    return res.external;
  }

  // Tiled export of the baked pipeline (lattice 65, Lanczos + output sharpening when `outLongEdge` downscales). `onSize` allocates.
  bool exportTiled(const EditParams& p, Renderer& r, const SourceImage& full, int outLongEdge, int latticeN, int tile, bool forceStream, float resizeSharpen,
                   std::function<void(int, int)> onSize,
                   std::function<bool(int, int, int, int, const std::uint8_t*, std::size_t)> sink, engine::GpuStats* st, std::string& err, const Lut3D* lut = nullptr) {
    engine::EngineSession::GpuExport e{p, r, full, store, cam, lut};
    e.outLongEdge = outLongEdge;
    e.tile = tile;
    e.latticeN = latticeN;
    e.forceStream = forceStream;
    e.resizeSharpen = resizeSharpen;
    e.onSize = std::move(onSize);
    e.target.sink = std::move(sink);
    return es.exportGpu(e, st, &err) == 0;
  }
};

}  // namespace dapple::gpu
