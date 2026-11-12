// GPU implementations of NRLuma (box blur of luma + edge preserving blend) and NRChroma (low-resolution chroma blur).
// Shaders: nr_luma.comp, nr_chroma.comp, sepconv.comp / boxscan.comp, down.comp. Mirrors denoiseLuma / denoiseChroma.
#include <algorithm>
#include <cmath>

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
struct NrU {
  std::int32_t dims[4];
  float nr[4];
};
struct NrCU {
  std::int32_t dims[4], low[4];
  float nr[4];
};
}  // namespace

bool recordNrLuma(GpuRecorder& r, const NrLumaNode& n, H D) {
  const FramePlan& P = *n.plan;
  const H Yb = r.blur(0, 3, D, n.W, n.Hh, {n.R->nrLumaR, 0, 0, 0}, 2, 0, kR32);
  if (Yb < 0) return false;
  const hal::PipelineH p = r.pl("nr_luma", {{10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  NrU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  const float a = P.nrLuma, sigma = 0.006f + 0.05f * a;
  u.nr[0] = std::min(1.f, std::pow(a, 0.75f) * 1.15f);
  u.nr[1] = 1.f / (sigma * sigma);
  u.nr[2] = n.ep.amp;
  u.nr[3] = n.ep.period;
  r.g.dispatch("nr_luma", p, {GpuGraph::RW(D), GpuGraph::R(Yb), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               std::uint32_t(cdiv(n.W, 16)), std::uint32_t(cdiv(n.Hh, 16)), &u, sizeof u);
  return true;
}

bool recordNrChroma(GpuRecorder& r, const NrChromaNode& n, H D) {
  const FramePlan& P = *n.plan;
  const int f = n.R->nrChromaF, lw = cdiv(n.W, f), lh = cdiv(n.Hh, f);
  const H low = r.tr(kF32, lw, lh);
  r.down(D, f, 2, low, WRect{0, 0, n.W, n.Hh}, 0, 0, lw, lh);
  const int rl = n.R->nrChromaRl;
  const H bl = r.blur(2, 2, low, lw, lh, {rl, rl, std::max(1, rl / 2), 0}, 2, 0, kF32);
  if (bl < 0) return false;
  const hal::PipelineH p = r.pl("nr_chroma", {{10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  NrCU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  u.low[0] = lw;
  u.low[1] = lh;
  u.low[2] = f;
  u.nr[0] = std::min(1.f, P.nrColor * 1.4f);
  u.nr[1] = 1.f / (0.05f * 0.05f);
  u.nr[2] = n.ep.amp;
  u.nr[3] = n.ep.period;
  r.g.dispatch("nr_chroma", p, {GpuGraph::RW(D), GpuGraph::R(bl), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               std::uint32_t(cdiv(n.W, 16)), std::uint32_t(cdiv(n.Hh, 16)), &u, sizeof u);
  return true;
}

}  // namespace dapple::engine::gpu
