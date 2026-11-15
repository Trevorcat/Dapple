// GPU implementation of Dehaze: dark channel + airlight analysis on a 1/4 grid (min filter, box, four reduction passes) and
// the apply pass. Shaders: sepconv.comp (min / box), reduce.comp, dehaze_apply.comp. Radii and strengths come from the Dehaze block
// (dehaze.h), the numbers of the CPU stages (core/src/spatial.cpp).
#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
struct ReduceU {
  std::int32_t s[4];
};
struct DehU {
  std::int32_t dims[4], low[4];
  float dh[4], grain[4];
};
}  // namespace

H recordDehazeAnalysis(GpuRecorder& r, const dehaze::Block& b, H col, int lw, int lh) {
  const int rm = b.minRadius, rb = b.boxRadius;
  H t = r.sep(1, col, lw, lh, 0, 2, {rm, rm, rm, rm}, kR32);
  if (t < 0) return -1;
  t = r.sep(3, t, lw, lh, 1, 2, {rm, rm, rm, rm}, kR32);
  if (t < 0) return -1;
  const H dc = r.blur(3, 3, t, lw, lh, {rb, 0, 0, 0}, 2, 0, kR32);
  if (dc < 0) return -1;
  const H gH = r.g.externalBuf(r.gBuf), partH = r.g.externalBuf(r.partBuf);
  const int groups = cdiv(lw * lh, r.rwg * 16);  // 16 samples per thread
  ReduceU u{};
  u.s[0] = lw;
  u.s[1] = lh;
  u.s[2] = groups;
  for (int mode = 0; mode < 4; ++mode) {
    const hal::PipelineH p = r.pl("reduce", {{0, mode}});
    if (!p) return -1;
    r.g.dispatch("reduce", p, {GpuGraph::R(dc), GpuGraph::R(col), GpuGraph::RW(gH), GpuGraph::RW(partH)}, (mode & 1) ? 1u : std::uint32_t(groups), 1,
                 &u, sizeof u);
  }
  return dc;
}

bool recordDehazeApply(GpuRecorder& r, const DehazeNode& n, H D) {
  const dehaze::Block& b = *n.b;
  const H gH = r.g.externalBuf(r.gBuf);
  const hal::PipelineH p = r.pl("dehaze_apply", {{0, b.solve() ? 0 : 1}, {10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  DehU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  u.low[0] = n.gridW;
  u.low[1] = n.gridH;
  u.low[2] = 4;
  u.dh[0] = b.omega;
  u.dh[1] = b.veil;
  u.dh[2] = b.veilAir;
  u.grain[0] = n.ep.amp;
  u.grain[1] = n.ep.period;
  r.g.dispatch("dehaze_apply", p,
               {GpuGraph::RW(D), GpuGraph::R(n.dc >= 0 ? n.dc : r.dummy(kR32)), GpuGraph::R(gH), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

// ---- GpuImpl (engine/nodes/node_list.inc) ----------------------------------------------------------------------------------------------
namespace {
H node(GpuRecorder& r, const NodeCall& c) {
  const GpuJob& j = c.job;
  const dehaze::Block& b = *static_cast<const dehaze::Block*>(c.block);
  if (b.solve() && c.solved < 0) return r.err = "dehaze solve missing from the plan", -1;
  return recordDehazeApply(r, DehazeNode{&b, j.W, j.Hh, j.ox, j.oy, j.gridW, j.gridH, b.solve() ? c.solved : -1, c.ep}, c.D) ? c.D : -1;
}
H solve(GpuRecorder& r, const void* block, H grid, int gw, int gh) { return recordDehazeAnalysis(r, *static_cast<const dehaze::Block*>(block), grid, gw, gh); }
const char* const kDz[] = {"sep_c2y", "sep_c2y_s", "sep_y", "sep_y_s", "sep_y_v", "bx_y", "bx_y_v", "reduce", "dehaze_apply"};
const GpuImpl kImpl{kDz, nullptr, node, solve, false};  // the dark channel (R32F)
}  // namespace
const GpuImpl* dehazeGpu() { return &kImpl; }

}  // namespace dapple::engine::gpu
