#pragma once
// The develop render of a film-match proxy (no neighbourhood stages, float, display-encoded) through the engine: graph + planner + the CPU
// executor, the exact variant, with the proxy as the tone reference (`Renderer::setToneProxy`). DAPPLE_ENGINE=off keeps the old
// Renderer so the golden oracle stays independent of the engine (same split as tone.cpp).
#include <stdexcept>
#include <string>

#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"

namespace dapple {

inline ImageF renderMatchProxy(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy) {
  Renderer r(p, camera, lut);
  r.setToneProxy(&proxy);
  if (engine::mode() == engine::Mode::Off) return r.renderFloat(proxy, false);
  engine::EngineSession session;
  engine::EngineSession::ExactOptions o;
  o.spatial = false;
  o.hasProxy = true;
  ImageF out;
  o.outFloat = &out;
  Image8 unused;
  std::string err;
  if (!session.renderExact(p, r, proxy, o, unused, &err)) throw std::runtime_error("film match: " + err);
  return out;
}

}  // namespace dapple
