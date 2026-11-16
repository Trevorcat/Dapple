#include "session.h"

namespace capi {

// The exact (reference / export) variant through the Planner and the CpuExecutor. Frames of more than
// 16 MP are split into tiles (TiledExport, halo + frame-global sweeps) so the float frame never exists; DAPPLE_ENGINE_TILE=N
// forces tile size N for every size, =0 never tiles.
bool engineRenderExact(DappleSession* s, const EditParams& params, const engine::point::Params& point, const SourceImage& src,
                       Image8& out) {
  dapple::engine::EngineSession::ExactOptions o;
  o.hasProxy = s->proxy != nullptr;
  int forced = -1;
  if (const char* e = std::getenv("DAPPLE_ENGINE_TILE")) forced = std::atoi(e);
  const std::size_t px = std::size_t(src.linear.width) * src.linear.height;
  if (forced > 0) {
    o.mode = dapple::engine::ExecMode::TiledExport;
    o.tile = forced;
  } else if (forced < 0 && px > 16000000u) {
    o.mode = dapple::engine::ExecMode::TiledExport;
  }
  std::lock_guard<std::mutex> lock(s->exportMtx);
  std::string err;
  if (!s->exportEngine.renderExact(params, point, src, o, out, &err)) {
    setError(err.empty() ? "engine failure" : err);
    return false;
  }
  return true;
}

#ifdef DAPPLE_HAVE_GPU
dapple::engine::SourceStore* sourceStoreOf(DappleSession* s) {
  std::call_once(s->storeOnce, [s] { s->store = std::make_unique<dapple::engine::MemorySourceStore>(s->source); });
  return s->store.get();
}

GpuWhole gpuWholeCheck(int W, int H, bool film) {
  GpuWhole g;
  if (!dapple::engine::gpu::runtimeEnabled()) return g;
  dapple::engine::GpuProvider* p = dapple::engine::gpuProvider();
  dapple::engine::GpuProvider::MemoryInfo mi;
  if (!p || !p->enabled() || !p->memoryInfo(&mi)) return g;
  g.active = true;
  g.unified = mi.unified;
  g.need = std::size_t(double(W) * double(H) * (film ? 32.0 : 8.0)) + std::min<std::size_t>(mi.poolBudget, std::size_t(320) << 20);
  if (mi.unified) g.extra = xstream::predictGpuExtra(W, H, film, mi.poolBudget);
  if (mi.budget > 0) {
    g.free = mi.budget > mi.usage ? std::size_t(mi.budget - mi.usage) : 0;
    g.fits = g.need <= g.free;
  }
  return g;
}
#endif

}  // namespace capi

#ifdef DAPPLE_HAVE_GPU
namespace {

// Full-picture export on the GPU executor (tiled; lattice 65, Lanczos + a little sharpening when downscaled). False = the CPU renders it.
bool gpuExport(DappleSession* s, const EditParams& params, engine::point::Params& point, const Lut3D* lut, int32_t outLongEdge, Image8& out) {
  std::lock_guard<std::mutex> lock(s->exportMtx);
  dapple::engine::EngineSession::GpuExport e{params, point, s->source, *sourceStoreOf(s), *s->camera, lut};
  e.outLongEdge = outLongEdge;
  int w = 0;
  e.onSize = [&](int ow, int oh) {
    w = ow;
    out.width = ow;
    out.height = oh;
    out.data.assign(std::size_t(ow) * oh * 3, 0);
  };
  e.target.sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
    for (int j = 0; j < th; ++j) {
      const std::uint8_t* src = rgba + std::size_t(j) * stride;
      std::uint8_t* dst = out.data.data() + (std::size_t(y + j) * w + x) * 3;
      for (int i = 0; i < tw; ++i) {
        dst[3 * i] = src[4 * i];
        dst[3 * i + 1] = src[4 * i + 1];
        dst[3 * i + 2] = src[4 * i + 2];
      }
    }
    return true;
  };
  if (const char* t = std::getenv("DAPPLE_EXPORT_GPU_OOM"); t && t[0] == '1') {  // tests: the device allocation fails (export_stream.gpuoom)
    s->gpuOom = true;
    return false;
  }
  std::string err;
  const int r = s->exportEngine.exportGpu(e, nullptr, &err);
  if (r != 0) stageLog("export: the whole-frame GPU export did not run (%s): the CPU renders", err.empty() ? "declined" : err.c_str());
  // an allocation failure on the device (pipeline-architecture.md 42.9): the caller switches a full-size export to bands instead of a slower whole-frame
  // CPU render that needs even more memory
  if (r != 0 && (err.find("out of memory") != std::string::npos || err.find("OutOfMemory") != std::string::npos ||
                 err.find("vkAllocateMemory") != std::string::npos || err.find("VkResult -2") != std::string::npos))
    s->gpuOom = true;
  return r == 0;
}

}  // namespace
#endif

namespace capi {

// Renders the session into 8-bit RGB. Returns false (with last error set) on failure.
bool renderSession(DappleSession* s, const char* paramsJson, const char* lutPath, int32_t outLongEdge, Image8& out) {
  if (!s) {
    setError("null session");
    return false;
  }
  EditParams params;
  if (!parseParams(paramsJson, params)) return false;
  return renderSessionParams(s, params, lutPath, outLongEdge, out);
}

bool renderSessionParams(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach) {
  if (s->lazyOpen.load() && toneReach == 1.f && outLongEdge > 0 && s->lazy && outLongEdge < std::max(s->lazy->width, s->lazy->height)) {
    // a reduced render of a lazy session (Ultra HDR quarter renders, headroom probe): from the exact reduction / the streamed GPU export
    const int r = streamRender(
        s, paramsIn, lutPath, outLongEdge,
        [&](int w, int h) {
          out.width = w, out.height = h;
          out.data.assign(std::size_t(w) * h * 3, 0);
        },
        [&](int y0, int n, const std::uint8_t* rgb) {
          std::memcpy(out.data.data() + std::size_t(y0) * out.width * 3, rgb, std::size_t(n) * out.width * 3);
          return true;
        });
    if (r != 0) return r > 0;
  }
  if (!ensureSource(s)) return false;
  const EditParams params = withFilmKey(s, paramsIn, lutPath);
  if (params.outputSpace == 1 && engine::point::filmActive(params)) {
    // Film stocks (and their blend with the normal render) are authored for sRGB output: rendered as sRGB, then converted to Display P3 (no colour is added).
    EditParams q = paramsIn;
    q.outputSpace = 0;
    if (!renderSessionParams(s, q, lutPath, outLongEdge, out, toneReach)) return false;
    srgb8ToDisplayP3(out);
    return true;
  }
  std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) {
    lut = loadLut(lutFile);
    if (!lut) return false;
  }
  engine::point::Params point = engine::point::resolve(params, *s->camera, lut.get());
  point.toneReach = toneReach;
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
  if (dapple::engine::gpu::runtimeEnabled() && toneReach == 1.f && (outLongEdge <= 0 || outLongEdge >= 1500)) {
    const int W = s->source.linear.width, H = s->source.linear.height;
    const bool fullSize = outLongEdge <= 0 || outLongEdge >= std::max(W, H);
    const GpuWhole g = fullSize ? gpuWholeCheck(W, H, engine::point::filmActive(point.p)) : GpuWhole{};
    if (!g.fits) {  // the driver's budget cannot hold it: no attempt (an allocation failure halfway costs seconds and memory)
      stageLog("export: the GPU has %.0f MB free for this %dx%d frame, it needs ~%.0f MB: not tried on the GPU", double(g.free) / 1048576.0, W, H,
               double(g.need) / 1048576.0);
      s->gpuOom = true;
    } else {
      s->gpuOom = false;
      if (gpuExport(s, params, point, lut.get(), outLongEdge, out)) return true;
    }
    if (s->gpuOom && fullSize && tlExportCall && !s->fullRawPath.empty() && !s->lazyOpen.load() && xstream::envMode() != xstream::Mode::Off) {
      // full size, and the session can stream: the export switches to bands (the caller: retryInBands) rather than a whole-frame CPU render
      s->retryBands = true;
      setError("the GPU ran out of memory: the export continues in bands");
      return false;
    }
  }
#endif
  if (outLongEdge > 0 && std::max(s->source.linear.width, s->source.linear.height) > outLongEdge) {
    // the Resize node of the engine (exports and previews do not keep large copies)
    const std::shared_ptr<const SourceImage> scaled = s->exportEngine.resizedSource(s->source, outLongEdge, false);
    if (!scaled) {
      setError("resize failed");
      return false;
    }
    return engineRenderExact(s, params, point, *scaled, out);
  }
  return engineRenderExact(s, params, point, s->source, out);
}

}  // namespace capi
