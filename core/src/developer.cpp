#include "dapple/developer.h"
#include "file_util.h"

#include <chrono>

#include "dapple/film_match.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"

namespace dapple {

namespace {
double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

Developer::Developer() : Developer(CameraRegistry::withBuiltins()) {}

Developer::Developer(CameraRegistry cameras)
    : cameras_(std::move(cameras)), decoders_(DecoderRegistry::withBuiltins()) {}

Result<DevelopResult> Developer::develop(const DevelopRequest& req) const {
  const ImageDecoder* decoder = decoders_.find(req.input);
  if (!decoder) return fail("Unsupported file type: " + pathToUtf8(req.input));

  DevelopResult result;
  auto t0 = std::chrono::steady_clock::now();
  auto source = decoder->decode(req.input, DecodeOptions{req.quality});
  if (!source) return std::unexpected(source.error());
  result.timings.decodeMs = msSince(t0);

  // Downscale before rendering: the output never needs more pixels.
  if (req.longEdge > 0) source->linear = resizeToLongEdge(source->linear, req.longEdge);

  result.meta = source->meta;
  result.cameraStore = std::make_shared<const CameraProfile>(effectiveCamera(cameras_, source->meta));
  result.camera = result.cameraStore.get();

  result.applied = req.look;
  if (req.autoAdjust) {
    t0 = std::chrono::steady_clock::now();
    result.autoResult = computeAutoAdjust(*source, *result.camera, *req.autoAdjust);
    result.applied += result.autoResult->correction;
    result.timings.analyzeMs = msSince(t0);
  }

  result.applied.filmSceneX = source->meta.asShotIlluminantXy[0];  // film white balance: the scene's as-shot illuminant
  result.applied.filmSceneY = source->meta.asShotIlluminantXy[1];
  if (!result.applied.filmStock.empty() && !isModernId(result.applied.filmStock)) {  // film exposure that keeps the photo's key (film_match.h)
    SourceImage proxy;
    proxy.sceneReferred = source->sceneReferred;
    proxy.linear = resizeToLongEdge(source->linear, 256);
    const FilmExposure fe = matchFilmExposure(result.applied, *result.camera, req.lut, proxy);
    result.applied.filmKeyEv = fe.filmEv;
    result.applied.filmPrintEv = fe.printEv;
  }
  t0 = std::chrono::steady_clock::now();
  std::string renderErr;  // graph + planner + CpuExecutor (the old Renderer::render is the golden oracle only)
  if (!engine::renderImage8(result.applied, *result.camera, req.lut, *source, result.image, &renderErr))
    return fail("render failed: " + renderErr);
  result.timings.renderMs = msSince(t0);
  return result;
}

}  // namespace dapple
