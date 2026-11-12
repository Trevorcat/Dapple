// GPU implementation of Resize (Lanczos-3, separable, any output rectangle from its source window) and the source
// conversion (float RGB rows -> RGBA16F). Shaders: resize.comp, convert.comp.
#include <algorithm>
#include <cmath>

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
struct ResizeU {
  std::int32_t src[4], dst[4], out_[4];
  float sc[4];
};
}  // namespace

// Source window (clamped, exclusive end) the Lanczos-3 filter reads to produce `dst` of a srcW x srcH -> dstW x dstH resize.
SrcWin lanczosWindow(int srcW, int srcH, int dstW, int dstH, const WRect& dst) {
  SrcWin w;
  auto axis = [](int srcLen, int dstLen, int d0, int d1, int& lo, int& hi) {
    const float s = float(srcLen) / float(dstLen), S = std::max(1.f, s);
    lo = int(std::floor((float(d0) + 0.5f) * s - 0.5f - 3.f * S)) - 1;
    hi = int(std::floor((float(d1) - 0.5f) * s - 0.5f + 3.f * S)) + 3;
    lo = std::max(0, lo);
    hi = std::min(srcLen, hi);
  };
  axis(srcW, dstW, dst.x0, dst.x1, w.x0, w.x1);
  axis(srcH, dstH, dst.y0, dst.y1, w.y0, w.y1);
  return w;
}

H recordResize(GpuRecorder& r, H src, const SrcWin& win, int srcW, int srcH, int dstW, int dstH, const WRect& dst) {
  const hal::PipelineH ph = r.pl("resize", {{0, 0}}), pv = r.pl("resize", {{0, 1}});
  if (!ph || !pv) return -1;
  const float sx = float(srcW) / float(dstW), sy = float(srcH) / float(dstH);
  const int bw = dst.w(), bh = dst.h();
  const H tmp = r.g.transient(kF16, std::uint32_t(bw), std::uint32_t(win.h()));
  const H out = r.g.transient(kF16, std::uint32_t(bw), std::uint32_t(bh));
  ResizeU u{};
  u.src[0] = srcW;
  u.src[1] = srcH;
  u.src[2] = win.x0;
  u.src[3] = win.y0;
  u.dst[0] = dstW;
  u.dst[1] = dstH;
  u.dst[2] = dst.x0;
  u.dst[3] = dst.y0;
  u.out_[0] = bw;
  u.out_[1] = win.h();
  u.out_[2] = win.y0;
  u.sc[0] = sx;
  u.sc[1] = std::max(1.f, sx);
  r.g.dispatch("resize_h", ph, {GpuGraph::R(src), GpuGraph::W(tmp)}, r.gx(bw), r.gy(win.h()), &u, sizeof u);
  ResizeU v = u;
  v.src[2] = 0;
  v.out_[0] = bw;
  v.out_[1] = bh;
  v.out_[2] = 0;
  v.sc[0] = sy;
  v.sc[1] = std::max(1.f, sy);
  r.g.dispatch("resize_v", pv, {GpuGraph::R(tmp), GpuGraph::W(out)}, r.gx(bw), r.gy(bh), &v, sizeof v);
  return out;
}

bool recordConvert(GpuRecorder& r, hal::BufferH staging, H dst, int w, int h, int dx, int dy) {
  const hal::PipelineH p = r.pl("convert", {});
  if (!p) return false;
  const H in = r.g.externalBuf(staging);
  const std::int32_t u[4] = {w, h, dx, dy};
  r.g.dispatch("convert", p, {GpuGraph::R(in), GpuGraph::W(dst)}, r.gx(w), r.gy(h), u, sizeof u);
  return true;
}

}  // namespace dapple::engine::gpu
