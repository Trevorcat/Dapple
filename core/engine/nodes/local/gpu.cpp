// GPU implementation of LocalDetail (local adjustments, stage W; docs/engine/pipeline-architecture.md section 40): the fine / very fine box
// blurs of the luminance, the guided-filter base on the window's 1/4 grid (down + gf_ab, the clarity solve of the Detail node at the
// local radius) and the combine pass local_detail.comp. Stage L is part of the point recipe (point_local). Twin of local::detailWindow.
#include "engine/nodes/gpu_common.h"
#include "local_eval.h"

namespace dapple::engine::gpu {

namespace {
struct LdU {
  std::int32_t dims[4], low[4];
  float grain[4];
};
}  // namespace

H recordLocalDetail(GpuRecorder& r, const LocalDetailNode& n, H D) {
  const FramePlan& P = *n.plan;
  if (!P.local) return r.err = "LocalDetail without local adjustments", -1;
  const local::Frame& f = *P.local;
  const local::DetailRadii R = local::detailRadii(f, P.spatialScale);
  const bool fine = f.winUsed[1] || f.winUsed[3], vfine = f.winUsed[2];
  H bf = -1, bs = -1, ab = -1;
  if (fine) {
    bf = r.blur(0, 3, D, n.W, n.Hh, {R.rt, 0, 0, 0}, 2, 0, kR32);
    if (bf < 0) return -1;
  }
  if (vfine) {
    bs = r.blur(0, 3, D, n.W, n.Hh, {R.rs, 0, 0, 0}, 1, 0, kR32);
    if (bs < 0) return -1;
  }
  const int lw = cdiv(n.W, 4), lh = cdiv(n.Hh, 4);
  if (R.guided) {
    const H yy = r.tr(kF32, lw, lh);
    if (r.down(D, 4, 1, yy, WRect{0, 0, n.W, n.Hh}, 0, 0, lw, lh) < 0) return -1;
    SpatialRadii SR;
    SR.clarityR = R.rg;
    ab = recordClarityAnalysis(r, SR, yy, lw, lh);
    if (ab < 0) return -1;
  }
  const hal::PipelineH p = r.pl("local_detail", {{0, R.guided ? 1 : 0}, {1, fine ? 1 : 0}, {2, vfine ? 1 : 0}, {10, n.ep.last ? 1 : 0},
                                                 {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return -1;
  const H out = n.ep.last ? -1 : r.tr(kF16, n.W, n.Hh);
  LdU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  u.low[0] = lw;
  u.low[1] = lh;
  u.grain[0] = n.ep.amp;
  u.grain[1] = n.ep.period;
  r.g.dispatch("local_detail", p,
               {GpuGraph::R(D), GpuGraph::R(n.aux0), GpuGraph::R(n.aux1), GpuGraph::R(bf >= 0 ? bf : r.dummy(kR32)),
                GpuGraph::R(bs >= 0 ? bs : r.dummy(kR32)), GpuGraph::R(ab >= 0 ? ab : r.dummy(kF32)),
                out >= 0 ? GpuGraph::W(out) : GpuGraph::R(r.dummy(kF16)), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return out >= 0 ? out : D;
}

namespace {
struct FiU {
  float gain[4];
  std::int32_t a[4], b[4];
  float geoA[4], geoB[4], geoC[4];
  std::int32_t c[4];
};
}  // namespace

bool recordLocalFilmIn(GpuRecorder& r, const LocalFilmInNode& n, H out) {
  const FramePlan& P = *n.plan;
  const int geo = !P.useGeo ? 0 : (P.exact ? 1 : 2);
  const bool w = n.aux0 >= 0 && n.aux1 >= 0;
  const hal::PipelineH p = r.pl("local_film_in", {{0, geo}, {3, w ? 1 : 0}});
  if (!p) return false;
  FiU u{};
  for (int k = 0; k < 3; ++k) u.gain[k] = P.gain[k];
  u.a[0] = P.srcW, u.a[1] = P.srcH, u.a[2] = n.W, u.a[3] = n.Hh;
  u.b[0] = n.src.ox, u.b[1] = n.src.oy, u.b[2] = n.ox, u.b[3] = n.oy;
  u.geoA[0] = P.cs, u.geoA[1] = P.sn, u.geoA[2] = P.fw, u.geoA[3] = P.fh;
  u.geoB[0] = P.ox0, u.geoB[1] = P.oy0, u.geoB[2] = P.stepX, u.geoB[3] = P.stepY;
  u.geoC[0] = P.fullW, u.geoC[1] = P.fullH;
  u.c[0] = P.rot, u.c[1] = P.flip ? 1 : 0;
  r.g.dispatch("local_film_in", p,
               {GpuGraph::R(n.src.img), GpuGraph::W(out), GpuGraph::R(n.ldesc), GpuGraph::R(n.lplanes), w ? GpuGraph::W(n.aux0) : GpuGraph::R(r.dummy(kF16)),
                w ? GpuGraph::W(n.aux1) : GpuGraph::R(r.dummy(kF16))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

}  // namespace dapple::engine::gpu
