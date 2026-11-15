// GPU implementation of the point recipe Geometry + Gain + ToneColour + Vignette (+ Grain/Pack8 epilogue) in one pass.
// Shader: point.comp. Mirrors the CPU point head (point::CpuFrame::headGainTone + LutSampler).
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
  WarpU w;
};
}  // namespace

bool recordPoint(GpuRecorder& r, const PointNode& n, H D) {
  const point::Blocks& F = *n.frame;
  const point::Geometry& P = F.geo;
  const int geo = !P.useGeo ? 0 : (P.exact ? 1 : 2);
  const bool hdrOn = F.hdr.active() && n.lut.hdr >= 0;
  const bool warpOn = F.warp.active() && n.src.warp >= 0;
  const bool localOn = n.ldesc >= 0 && n.lplanes >= 0;  // bound only when the frame has local adjustments (Executor::ensureLocal)
  const bool localW = localOn && n.aux0 >= 0 && n.aux1 >= 0;
  const hal::PipelineH p = localOn ? r.pl("point_local", {{0, geo}, {1, hdrOn ? 1 : 0}, {3, localW ? 1 : 0}, {4, warpOn ? 1 : 0}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}})
                                   : r.pl("point", {{0, geo}, {1, hdrOn ? 1 : 0}, {4, warpOn ? 1 : 0}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  PointU u{};
  for (int k = 0; k < 3; ++k) u.gain[k] = F.gain[k];
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
  u.geoC[2] = n.vig ? n.vig->amount : 0.f;
  u.geoC[3] = n.vig && n.vig->p3 == 1 ? 1.f : 0.f;
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
    const point::Hdr& h = F.hdr;
    u.h0[0] = h.s, u.h0[1] = h.ev, u.h0[2] = h.key, u.h0[3] = h.yn;
    u.h1[0] = h.yclip, u.h1[1] = float(h.field->gw), u.h1[2] = float(h.field->gh), u.h1[3] = float(h.field->nb);
    u.h2[0] = h.field->lmin, u.h2[1] = h.field->lstep, u.h2[2] = h.lo, u.h2[3] = h.hi;
    u.h3[0] = h.ls, u.h3[1] = h.hp;
  }
  if (warpOn) warpUniform(F.warp, n.src, u.w);
  // binding 7: the HDR buffer, which also carries the reshaping field after the HDR grid
  const H hb = warpOn ? n.src.warp : (hdrOn ? n.lut.hdr : n.lut.tone);
  if (localOn) {
    r.g.dispatch("point_local", p,
                 {GpuGraph::R(n.src.img), GpuGraph::R(n.lut.tone), GpuGraph::R(n.lut.lat), GpuGraph::R(n.lut.lat2), GpuGraph::W(D),
                  n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8)), GpuGraph::R(hb),
                  GpuGraph::R(n.ldesc), GpuGraph::R(n.lplanes), localW ? GpuGraph::W(n.aux0) : GpuGraph::R(r.dummy(kF16)),
                  localW ? GpuGraph::W(n.aux1) : GpuGraph::R(r.dummy(kF16))},
                 r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
    return true;
  }
  r.g.dispatch("point", p,
               {GpuGraph::R(n.src.img), GpuGraph::R(n.lut.tone), GpuGraph::R(n.lut.lat), GpuGraph::R(n.lut.lat2), GpuGraph::W(D),
                n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8)), GpuGraph::R(hb)},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

// ---- GpuImpls (engine/nodes/node_list.inc) ---------------------------------------------------------------------------------------------
namespace {

PointNode pointNode(const GpuJob& j, const Epilogue& ep) {
  PointNode pn{j.frame, j.src, j.lut, j.W, j.Hh, j.ox, j.oy, ep};
  pn.ldesc = j.ldesc;
  pn.lplanes = j.lplanes;
  return pn;
}

// the point recipe: one pass for [Resize | Geometry] + Gain + Hdr + LocalLinear + ToneColour(Exact) + Vignette (+ the epilogue)
H groupPoint(GpuRecorder& r, const GroupCall& c) {
  PointNode pn = pointNode(c.job, c.ep);
  pn.aux0 = c.job.aux0;
  pn.aux1 = c.job.aux1;
  pn.vig = &c.job.blocks->vignette;
  return recordPoint(r, pn, c.D) ? c.D : -1;
}

// the film finish: the film plane -> display stages (curves, look LUT) -> blend with the normal branch (when a Blend is in the group: Gain +
// ToneColour through the lattice, no vignette, the same local adjustments) -> vignette -> clamp -> D (or the epilogue + RGBA8 when last)
H groupDisplay(GpuRecorder& r, const GroupCall& c) {
  const GpuJob& j = c.job;
  const point::Blocks& P = *j.frame;
  if (j.film < 0) return r.err = "display stages without a film plane", -1;
  if (j.disp.curves < 0 || j.disp.lut < 0) return r.err = "film display resources missing", -1;
  bool blend = false;
  for (NodeType t : c.nodes) blend |= t == NodeType::Blend;
  H M = -1;
  if (blend) {
    if (j.lut.n < 2) return r.err = "film blend without a colour lattice", -1;
    M = r.tr(kF16, j.W, j.Hh);
    if (!recordPoint(r, pointNode(j, Epilogue{}), M)) return -1;
  }
  if (j.filmClamped && !blend && !c.ep.last && j.filmOx == 0 && j.filmOy == 0) return j.film;  // film_fin would only clamp: film_pd did it
  struct U {
    std::int32_t a[4];
    float geoB[4], geoC[4], e[4];
    std::int32_t f[4];
    float g[4], dmin[4], dmax[4];
  } u{};
  u.a[0] = j.W;
  u.a[1] = j.Hh;
  u.a[2] = j.ox;
  u.a[3] = j.oy;
  u.geoB[0] = P.geo.ox0;
  u.geoB[1] = P.geo.oy0;
  u.geoB[2] = P.geo.stepX;
  u.geoB[3] = P.geo.stepY;
  u.geoC[0] = P.geo.fullW;
  u.geoC[1] = P.geo.fullH;
  u.geoC[2] = j.blocks->vignette.amount;
  u.e[0] = c.ep.amp;
  u.e[1] = c.ep.period;
  const DisplayBind& d = j.disp;
  const bool dispOn = d.curveMask != 0 || d.lutN > 0;
  u.f[0] = j.filmOx;
  u.f[1] = j.filmOy;
  u.f[2] = dispOn ? int(d.curveMask) : 0;
  u.f[3] = dispOn ? d.lutN : 0;
  u.g[0] = j.strength;
  u.g[1] = dispOn ? d.lutMix : 0.f;
  u.g[2] = dispOn ? float(d.curveSamples) : 0.f;
  for (int k = 0; k < 3; ++k) {
    u.dmin[k] = d.domMin[k];
    u.dmax[k] = d.domMax[k];
  }
  const hal::PipelineH p = r.pl("film_fin", {{10, c.ep.last ? 1 : 0}, {11, c.ep.last && c.ep.grain ? 1 : 0}, {12, blend ? 1 : 0}, {13, dispOn ? 1 : 0}});
  if (!p) return -1;
  r.g.dispatch("film_fin", p,
               {GpuGraph::R(j.film), GpuGraph::W(c.D), c.ep.last ? GpuGraph::W(c.ep.out8) : GpuGraph::R(r.dummy(kU8)), GpuGraph::R(blend ? M : r.dummy(kF16)),
                GpuGraph::R(d.curves), GpuGraph::R(d.lut)},
               r.gx(j.W), r.gy(j.Hh), &u, sizeof u);
  return c.D;
}

const char* const kPoint[] = {"point", "point_local"};
const char* const kDisplay[] = {"film_fin", "point"};
const GpuImpl kPointImpl{kPoint, groupPoint, nullptr, nullptr, false, 1};
const GpuImpl kToneColourImpl{kPoint, groupPoint, nullptr, nullptr, false, 1, false, false, true};
const GpuImpl kDisplayImpl{kDisplay, groupDisplay, nullptr, nullptr, false, 2};
const GpuImpl kEpilogueImpl{{}, nullptr, nullptr, nullptr, false, 0, true};

}  // namespace

const GpuImpl* pointGpu() { return &kPointImpl; }
const GpuImpl* toneColourGpu() { return &kToneColourImpl; }
const GpuImpl* displayGpu() { return &kDisplayImpl; }
const GpuImpl* epilogueGpu() { return &kEpilogueImpl; }

}  // namespace dapple::engine::gpu
