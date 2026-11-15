// GPU implementations of NRLuma (box blur of luma + edge preserving blend) and NRChroma (low-resolution chroma blur).
// Shaders: nr_luma.comp, nr_chroma.comp, sepconv.comp / boxscan.comp, down.comp. Radii, blend and edge stop come from the NR blocks
// (nr.h), the numbers the CPU stages (denoiseLuma / denoiseChroma) use.
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
  const nr::LumaBlock& b = *n.b;
  const H Yb = r.blur(0, 3, D, n.W, n.Hh, {b.radius, 0, 0, 0}, 2, 0, kR32);
  if (Yb < 0) return false;
  const hal::PipelineH p = r.pl("nr_luma", {{10, n.ep.last ? 1 : 0}, {11, n.ep.last && n.ep.grain ? 1 : 0}});
  if (!p) return false;
  NrU u{};
  u.dims[0] = n.W;
  u.dims[1] = n.Hh;
  u.dims[2] = n.ox;
  u.dims[3] = n.oy;
  u.nr[0] = b.blend;
  u.nr[1] = b.inv2;
  u.nr[2] = n.ep.amp;
  u.nr[3] = n.ep.period;
  r.g.dispatch("nr_luma", p, {GpuGraph::RW(D), GpuGraph::R(Yb), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

bool recordNrChroma(GpuRecorder& r, const NrChromaNode& n, H D) {
  const nr::ChromaBlock& b = *n.b;
  const int f = b.factor, lw = cdiv(n.W, f), lh = cdiv(n.Hh, f);
  const H low = r.tr(kF32, lw, lh);
  r.down(D, f, 2, low, WRect{0, 0, n.W, n.Hh}, 0, 0, lw, lh);
  const H bl = r.blur(2, 2, low, lw, lh, {b.lowRadius, b.lowRadius, b.lumaRadius, 0}, 2, 0, kF32);
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
  u.nr[0] = b.blend;
  u.nr[1] = b.inv2;
  u.nr[2] = n.ep.amp;
  u.nr[3] = n.ep.period;
  r.g.dispatch("nr_chroma", p, {GpuGraph::RW(D), GpuGraph::R(bl), n.ep.last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return true;
}

// ---- GpuImpl (engine/nodes/node_list.inc) ----------------------------------------------------------------------------------------------
namespace {
H nodeLuma(GpuRecorder& r, const NodeCall& c) {
  const GpuJob& j = c.job;
  return recordNrLuma(r, NrLumaNode{static_cast<const nr::LumaBlock*>(c.block), j.W, j.Hh, j.ox, j.oy, c.ep}, c.D) ? c.D : -1;
}
H nodeChroma(GpuRecorder& r, const NodeCall& c) {
  const GpuJob& j = c.job;
  return recordNrChroma(r, NrChromaNode{static_cast<const nr::ChromaBlock*>(c.block), j.W, j.Hh, j.ox, j.oy, c.ep}, c.D) ? c.D : -1;
}
const char* const kNrL[] = {"nr_luma", "sep_d2y", "sep_d2y_s", "sep_y", "sep_y", "sep_y_s", "sep_y_v", "bx_d2y", "bx_y", "bx_y_v"};
const char* const kNrC[] = {"down", "nr_chroma", "sep_c", "sep_c_s"};
const GpuImpl kLuma{kNrL, nullptr, nodeLuma};
const GpuImpl kChroma{kNrC, nullptr, nodeChroma};
}  // namespace
const GpuImpl* nrLumaGpu() { return &kLuma; }
const GpuImpl* nrChromaGpu() { return &kChroma; }

}  // namespace dapple::engine::gpu
