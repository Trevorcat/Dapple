// GPU implementation of the point recipe Geometry + Gain + ToneColour + Vignette (+ Grain/Pack8 epilogue) in one pass.
// Shader: point.comp. Mirrors Renderer::renderBakedImpl + LutSampler (core/src/renderer.cpp).
#include "engine/nodes/gpu_common.h"
#include "hdr_field.h"

namespace dapple::engine::gpu {

namespace {
struct PointU {
  float gain[4];
  std::int32_t a[4], b[4];
  float geoA[4], geoB[4], geoC[4];
  std::int32_t c[4], d[4];
  float e[4];
  float h0[4], h1[4], h2[4], h3[4];
};
}  // namespace

bool recordPoint(GpuRecorder& r, const PointNode& n, H D) {
  const FramePlan& P = *n.plan;
  const int geo = !P.useGeo ? 0 : (P.exact ? 1 : 2);
  const bool hdrOn = P.hdr.active() && n.lut.hdr >= 0;
  const hal::PipelineH p = r.pl("point", {{0, geo}, {1, hdrOn ? 1 : 0}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
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
  if (hdrOn) {
    const FramePlan::Hdr& h = P.hdr;
    u.h0[0] = h.s, u.h0[1] = h.ev, u.h0[2] = h.key, u.h0[3] = h.yn;
    u.h1[0] = h.yclip, u.h1[1] = float(h.field->gw), u.h1[2] = float(h.field->gh), u.h1[3] = float(h.field->nb);
    u.h2[0] = h.field->lmin, u.h2[1] = h.field->lstep, u.h2[2] = h.lo, u.h2[3] = h.hi;
    u.h3[0] = h.ls, u.h3[1] = h.hp;
  }
  r.g.dispatch("point", p,
               {GpuGraph::R(n.src.img), GpuGraph::R(n.lut.tone), GpuGraph::R(n.lut.lat), GpuGraph::R(n.lut.lat2), GpuGraph::W(D),
                n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8)), GpuGraph::R(hdrOn ? n.lut.hdr : n.lut.tone)},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

}  // namespace dapple::engine::gpu
