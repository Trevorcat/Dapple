// GPU implementation of Detail (clarity, texture, sharpen): guided-filter base on a 1/4 grid, texture / sharpen blurs and
// the combine pass. Shaders: gf_ab.comp, sepconv.comp / boxscan.comp, detail.comp. Mirrors guidedBase + detailPass.
#include <algorithm>
#include <cmath>

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
struct AbU {
  std::int32_t s[4];
  float eps[4];
};
struct DetU {
  std::int32_t dims[4], low[4];
  float gains[4], sharp[4], grain[4];
};
}  // namespace

H recordClarityAnalysis(GpuRecorder& r, int rad, H yy, int lw, int lh) {
  const H m = r.blur(2, 2, yy, lw, lh, {rad, rad, rad, rad}, 1, 0, kF32);
  if (m < 0) return -1;
  const H ab0 = r.tr(kF32, lw, lh);
  const hal::PipelineH pa = r.pl("gf_ab", {});
  if (!pa) return -1;
  AbU u{};
  u.s[0] = lw;
  u.s[1] = lh;
  u.eps[0] = engine::detail::kGuidedEps;
  r.g.dispatch("gf_ab", pa, {GpuGraph::R(m), GpuGraph::W(ab0)}, r.gx(lw), r.gy(lh), &u, sizeof u);
  return r.blur(2, 2, ab0, lw, lh, {rad, rad, rad, rad}, 1, 0, kF32);
}

bool recordDetail(GpuRecorder& r, const DetailNode& n, H D) {
  const engine::detail::Block& R = *n.b;
  const bool clar = R.clarity != 0, tex = R.texture != 0, shp = R.sharpAmount > 0;
  H texH = -1, shH = -1;
  if (tex) {
    texH = r.blur(0, 3, D, n.W, n.Hh, {R.textureRadius, 0, 0, 0}, 2, 0, kR32);
    if (texH < 0) return false;
  }
  if (shp) {
    if (R.sharpBox > 0) {
      shH = r.blur(0, 3, D, n.W, n.Hh, {R.sharpBox, 0, 0, 0}, 3, 0, kR32);
    } else {
      // direct Gaussian: weights w[0..R] (symmetric, normalised) in the slot's table
      const int Rg = R.sharpGauss;
      float* w = static_cast<float*>(r.weights.mapped);
      double sum = 0;
      for (int i = -Rg; i <= Rg; ++i) sum += std::exp(-0.5 * double(i * i) / (double(R.sharpSigma) * R.sharpSigma));
      for (int i = 0; i <= Rg; ++i) w[i] = float(std::exp(-0.5 * double(i * i) / (double(R.sharpSigma) * R.sharpSigma)) / sum);
      shH = r.blur(0, 3, D, n.W, n.Hh, {Rg, 0, 0, 0}, 1, 1, kR32);
    }
    if (shH < 0) return false;
  }
  const hal::PipelineH p = r.pl("detail", {{0, clar ? 1 : 0}, {1, tex ? 1 : 0}, {2, shp ? 1 : 0}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  DetU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  u.low[0] = n.gridW;
  u.low[1] = n.gridH;
  u.low[2] = 4;
  u.gains[0] = R.clarityGain;
  u.gains[1] = R.textureGain;
  u.gains[2] = R.texturePos ? 1.f : 0.f;
  u.gains[3] = R.sharpAmount;
  u.sharp[0] = engine::detail::kSharpLimit;
  u.sharp[1] = engine::detail::kSharpGate;
  u.grain[0] = n.ep.amp;
  u.grain[1] = n.ep.period;
  r.g.dispatch("detail", p,
               {GpuGraph::RW(D), GpuGraph::R(texH >= 0 ? texH : r.dummy(kR32)), GpuGraph::R(shH >= 0 ? shH : r.dummy(kR32)),
                GpuGraph::R(n.ab >= 0 ? n.ab : r.dummy(kF32)), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

// ---- GpuImpl (engine/nodes/node_list.inc) ----------------------------------------------------------------------------------------------
namespace {
H node(GpuRecorder& r, const NodeCall& c) {
  const GpuJob& j = c.job;
  const engine::detail::Block& b = *static_cast<const engine::detail::Block*>(c.block);
  if (b.solve() && c.solved < 0) return r.err = "clarity solve missing from the plan", -1;
  return recordDetail(r, DetailNode{&b, j.W, j.Hh, j.ox, j.oy, j.gridW, j.gridH, b.solve() ? c.solved : -1, c.ep}, c.D) ? c.D : -1;
}
H solve(GpuRecorder& r, const void* block, H grid, int gw, int gh) {
  return recordClarityAnalysis(r, static_cast<const engine::detail::Block*>(block)->guidedRadius, grid, gw, gh);
}
const char* const kDetail[] = {"down", "gf_ab", "sep_c", "sep_c_s", "sep_d2y", "sep_d2y_s", "sep_y", "sep_y_s", "sep_y_v", "bx_d2y", "bx_y", "bx_y_v", "detail"};
const GpuImpl kImpl{kDetail, nullptr, node, solve, true};  // the guided-filter coefficients (RGBA32F)
}  // namespace
const GpuImpl* detailGpu() { return &kImpl; }

}  // namespace dapple::engine::gpu
