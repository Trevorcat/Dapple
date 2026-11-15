#pragma once
// The develop render of a film-match proxy (no neighbourhood stages, float, display-encoded) through the engine: graph + planner + the CPU
// executor, the exact variant, with the proxy as the tone reference (`point::setToneProxy`).
#include <stdexcept>
#include <string>

#include "engine/nodes/point/point.h"
#include "engine/exec/engine_session.h"

namespace dapple {

inline ImageF renderMatchProxy(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy) {
  engine::point::Params r = engine::point::resolve(p, camera, lut);
  engine::point::setToneProxy(r, &proxy);
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
