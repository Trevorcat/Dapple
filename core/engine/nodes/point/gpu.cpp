// GPU implementation of the point recipe Geometry + Gain + ToneColour + Vignette (+ Grain/Pack8 epilogue) in one pass.
// Shader: point.comp. Mirrors Renderer::renderBakedImpl + LutSampler (core/src/renderer.cpp).
#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
struct PointU {
  float gain[4];
  std::int32_t a[4], b[4];
  float geoA[4], geoB[4], geoC[4];
  std::int32_t c[4], d[4];
  float e[4];
};
}  // namespace

bool recordPoint(GpuRecorder& r, const PointNode& n, H D) {
  const FramePlan& P = *n.plan;
  const int geo = !P.useGeo ? 0 : (P.exact ? 1 : 2);
  const hal::PipelineH p = r.pl("point", {{0, geo}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  PointU u{};
  for (int k = 0; k < 3; ++k) u.gain[k] = P.gain[k];
  u.a[0] = P.srcW;
  u.a[1] = P.srcH;
  u.a[2] = n.W;
  u.a[3] = n.Hh;
  u.b[0] = n.src.ox;
  u.b[1] = n.src.oy;
  u.b[2] = n.ox;
  u.b[3] = n.oy;
  u.geoA[0] = P.cs;
  u.geoA[1] = P.sn;
  u.geoA[2] = P.fw;
  u.geoA[3] = P.fh;
  u.geoB[0] = P.ox0;
  u.geoB[1] = P.oy0;
  u.geoB[2] = P.stepX;
  u.geoB[3] = P.stepY;
  u.geoC[0] = P.fullW;
  u.geoC[1] = P.fullH;
  u.geoC[2] = P.vignette;
  u.c[0] = P.rot;
  u.c[1] = P.flip ? 1 : 0;
  u.c[2] = n.lut.n;
  u.c[3] = n.lut.toneN;
  u.d[0] = n.lut.toneBase;
  u.d[1] = n.lut.n2;
  u.e[0] = n.lut.max1;
  u.e[1] = n.lut.max2;
  u.e[2] = n.ep.amp;
  u.e[3] = n.ep.period;
  r.g.dispatch("point", p,
               {GpuGraph::R(n.src.img), GpuGraph::R(n.lut.tone), GpuGraph::R(n.lut.lat), GpuGraph::R(n.lut.lat2), GpuGraph::W(D),
                n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               std::uint32_t(cdiv(n.W, 16)), std::uint32_t(cdiv(n.Hh, 16)), &u, sizeof u);
  return true;
}

}  // namespace dapple::engine::gpu
