// Recorder helpers shared by the GPU node implementations: pipelines, working images, separable filters (box / weight
// table / minimum, O(1) running-sum box), area downscale, radii of the spatial stages. Ported from phase-1 frame.cpp.
#include "engine/nodes/gpu_common.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dapple::engine::gpu {

namespace {
int lroundi(float v) { return int(std::lround(v)); }

struct SepU {
  std::int32_t size[4], radii[4];
};
struct BoxU {
  std::int32_t size[4];
};
struct DownU {
  std::int32_t a[4], b[4], c[4];
};

// Radii from which the running-sum box (boxscan.comp, cost independent of R) replaces the direct sum of sepconv.comp.
// Env DAPPLE_GPU_BOX_SCAN_MIN overrides (0 = never use the scan, unset = device default).
int scanMinRadius() {
  static const int v = [] {
    const char* e = std::getenv("DAPPLE_GPU_BOX_SCAN_MIN");
    return e ? std::atoi(e) : -1;
  }();
  return v;
}
}  // namespace

hal::PipelineH GpuRecorder::pl(const char* shader, std::initializer_list<hal::SpecConst> spec) {
  hal::PipelineDesc d;
  d.shader = shader;
  d.spec.assign(spec.begin(), spec.end());
  hal::PipelineH p;
  if (dev.createPipeline(d, &p) != hal::Status::Ok) {
    err = dev.lastError();
    return {};
  }
  return p;
}

H GpuRecorder::dummy(Format f) {
  H& h = f == kU8 ? d8_ : f == kF16 ? d16_ : f == kF32 ? d32_ : dR_;
  if (h < 0) h = g.external(dev.dummyImage(f));
  return h;
}
H GpuRecorder::weightsH() {
  if (wbuf_ < 0) wbuf_ = g.externalBuf(weights);
  return wbuf_;
}

H GpuRecorder::tr(Format f, int w, int h) {
  std::uint32_t aw = 0, ah = 0;
  if (allocW > 0) {
    if (w == W && h == H_) {
      aw = std::uint32_t(allocW);
      ah = std::uint32_t(allocH);
    } else {
      for (int f2 : {2, 4})
        if (w == cdiv(W, f2) && h == cdiv(H_, f2)) {
          aw = std::uint32_t(cdiv(allocW, f2));
          ah = std::uint32_t(cdiv(allocH, f2));
        }
    }
  }
  return g.transient(f, std::uint32_t(w), std::uint32_t(h), aw, ah);
}

H GpuRecorder::sep(int kind, H in, int w, int h, int dir, int op, const std::array<int, 4>& rad, Format outFmt) {
  int rmax = 0;
  for (int c = 0; c < 4; ++c) rmax = std::max(rmax, std::min(rad[c], 64));

  // O(1) running-sum box for single channel data once the direct sum costs more than the scan
  // default: 12 on unified-memory (mobile / integrated) GPUs, where shared-memory traffic and ALU are the scarce resources;
  // 24 on discrete GPUs, where the direct sum is as fast up to about that radius (gpu_engine_tests bench-box, RTX 4060 Ti)
  const int envMin = scanMinRadius();
  const int scanMin = boxScanMin >= 0 ? boxScanMin : envMin >= 0 ? envMin : (dev.caps().unifiedMemory ? 12 : 24);
  if (op == 0 && scanMin > 0 && (kind == 0 || kind == 3) && std::min(rad[0], 64) >= scanMin && !(kind == 0 && dir == 1) && outFmt == kR32) {
    const bool vert = dir == 1;
    const char* name = kind == 0 ? "bx_d2y" : vert ? "bx_y_v" : "bx_y";
    const hal::PipelineH p = pl(name, {});
    if (!p) return -1;
    const H out = tr(outFmt, w, h);
    BoxU u{};
    u.size[0] = w;
    u.size[1] = h;
    u.size[2] = std::min(rad[0], 64);
    const std::uint32_t gx = vert ? std::uint32_t(cdiv(w, 4)) : std::uint32_t(cdiv(w, 1024));
    const std::uint32_t gy = vert ? std::uint32_t(cdiv(h, 512)) : std::uint32_t(cdiv(h, 2));
    g.dispatch("boxscan", p, {GpuGraph::R(in), GpuGraph::W(out)}, gx, gy, &u, sizeof u, 1, 1.5);
    return out;
  }

  static const char* names[3][4] = {{"sep_d2y", "sep_c2y", "sep_c", "sep_y"},
                                    {"sep_d2y_s", "sep_c2y_s", "sep_c_s", "sep_y_s"},
                                    {"sep_d2y_v", "sep_c2y_v", "", "sep_y_v"}};
  // Small radii: 16 x 16 groups with a small tile. Large radii: 64 along the axis x 4 lines; vertical passes of single
  // channel images use 16 along x 16 lines with 4 outputs per thread.
  const bool small = rmax <= 8;
  const bool bigV = !small && dir == 1 && kind != 2;
  const int variant = small ? 1 : bigV ? 2 : 0;
  const int lx = (small || bigV) ? 16 : 64, ly = (small || bigV) ? 16 : 4;
  const int outsAlong = bigV ? 64 : lx;  // outputs along the axis per group
  const int perp = ly;                  // lines per group
  const hal::PipelineH p = pl(names[variant][kind], {{0, dir}, {1, lx}, {2, ly}, {3, op}});
  if (!p) return -1;
  const H out = tr(outFmt, w, h);
  SepU u{};
  u.size[0] = w;
  u.size[1] = h;
  for (int c = 0; c < 4; ++c) u.radii[c] = std::min(rad[c], 64);
  u.size[2] = rmax;
  const std::uint32_t gx = dir == 0 ? std::uint32_t(cdiv(w, outsAlong)) : std::uint32_t(cdiv(w, perp));
  const std::uint32_t gy = dir == 0 ? std::uint32_t(cdiv(h, perp)) : std::uint32_t(cdiv(h, outsAlong));
  g.dispatch(op == 2 ? "min" : op == 1 ? "gauss" : "box", p, {GpuGraph::R(in), GpuGraph::W(out), GpuGraph::R(weightsH())}, gx, gy, &u, sizeof u, 1,
             2.0);
  return out;
}

H GpuRecorder::blur(int first, int work, H in, int w, int h, const std::array<int, 4>& rad, int iters, int op, Format outFmt) {
  H cur = in;
  for (int i = 0; i < iters; ++i) {
    const H t = sep(i == 0 ? first : work, cur, w, h, 0, op, rad, outFmt);
    if (t < 0) return -1;
    cur = sep(work, t, w, h, 1, op, rad, outFmt);
    if (cur < 0) return -1;
  }
  return cur;
}

H GpuRecorder::down(H D, int f, int mode, H dst, const WRect& win, int offX, int offY, int cntX, int cntY) {
  const hal::PipelineH p = pl("down", {{0, mode}});
  if (!p) return -1;
  DownU u{};
  u.a[0] = win.x1;
  u.a[1] = win.y1;
  u.a[2] = win.x0;
  u.a[3] = win.y0;
  u.b[0] = cntX;
  u.b[1] = cntY;
  u.b[2] = offX;
  u.b[3] = offY;
  u.c[0] = f;
  g.dispatch("down", p, {GpuGraph::R(D), GpuGraph::RW(dst)}, std::uint32_t(cdiv(cntX, 16)), std::uint32_t(cdiv(cntY, 16)), &u, sizeof u);
  return dst;
}

void grainArgs(const FramePlan& P, float& amp, float& period) {
  period = std::max(1.f, 1.6f * P.spatialScale);
  amp = P.grain * 0.13f * (period > 1.01f ? 1.5f : 1.f);
}

SpatialRadii spatialRadii(const FramePlan& P, float resizeSharpen) {
  SpatialRadii r;
  const float scale = P.spatialScale;
  int haloL = 0, haloC = 0, haloD = 0;
  if (P.nrLuma > 0) {
    r.nrLumaR = std::max(1, lroundi((0.8f + 1.8f * P.nrLuma) * scale));
    haloL = 2 * r.nrLumaR + 1;
  }
  if (P.nrColor > 0) {
    const float rFull = (1.5f + 5.f * P.nrColor) * scale;
    r.nrChromaF = rFull >= 6.f ? 4 : 2;
    r.nrChromaRl = std::max(1, lroundi(rFull / float(r.nrChromaF)));
    haloC = (2 * r.nrChromaRl + 3) * r.nrChromaF;
  }
  if (P.dehaze > 0) {
    r.dehazeMin = std::max(1, lroundi(2.5f * scale));
    r.dehazeBox = std::max(1, lroundi(3.f * scale));
  }
  if (P.clarity != 0) r.clarityR = std::max(2, lroundi(0.02f * 2048.f * scale / 4.f));
  if (P.texture != 0) {
    r.textureR = std::max(1, lroundi(2.f * scale));
    haloD = std::max(haloD, 2 * r.textureR + 1);
  }
  r.sharpAmount = P.sharpen > 0 || resizeSharpen > 0 ? P.sharpen + resizeSharpen : 0.f;
  if (r.sharpAmount > 0) {
    r.sharpSigma = std::max(0.4f, P.sharpRadius * scale * 0.5f);
    if (r.sharpSigma > 2.6f) {
      r.sharpBox = std::max(1, lroundi(std::sqrt(r.sharpSigma * r.sharpSigma * 4.f / 3.f + 0.25f) - 0.5f));
      haloD = std::max(haloD, 3 * r.sharpBox + 1);
    } else {
      r.sharpGauss = std::max(1, int(std::ceil(r.sharpSigma * 3.f)));
      haloD = std::max(haloD, r.sharpGauss + 1);
    }
  }
  r.halo = (haloL + haloC + haloD + 8 + 3) / 4 * 4;
  return r;
}

}  // namespace dapple::engine::gpu
