// GPU implementation of LocalDetail (local adjustments, stage W; docs/engine/pipeline-architecture.md section 40): the fine / very fine box
// blurs of the luminance, the guided-filter base on the window's 1/4 grid (down + gf_ab, the clarity solve of the Detail node at the
// local radius) and the combine pass local_detail.comp. Stage L is part of the point recipe (point_local). Twin of local::detailWindow.
// Portrait retouch (section 43) runs first: local_heal (spots), the retouch planes (sep + local_rt_down + gf_ab) and local_retouch.
#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/nodes/gpu_common.h"
#include "local_eval.h"
#include "local_retouch.h"

namespace dapple::engine::gpu {

namespace {
struct LdU {
  std::int32_t dims[4], low[4];
  float grain[4];
  std::int32_t ch[4], ch2[4];
};
struct HealU {
  std::int32_t dims[4], tab[4], bins[4];
  float grain[4];
};
struct RtDownU {
  std::int32_t dims[4], low[4], ch[4], ch2[4];
};
struct RtU {
  std::int32_t dims[4], low[4];
  float grain[4];
  std::int32_t ch[4], ch2[4];
};
struct AbU {
  std::int32_t s[4];
  float eps[4];
};

// amount channel of op `op` in the frame (-1 = not carried)
int chanOf(const local::Frame& f, int op) {
  for (int c = 0; c < kLocalWindowSlots; ++c)
    if (f.chanOp[c] == op && f.winUsed[op - kLocalWindowFirst]) return c;
  return -1;
}

// one empty bin at the end of the table (a heal pass with no spot in its window still runs the epilogue)
bool emptyBin(GpuRecorder& r, std::uint32_t& off) {
  if (!r.taps.mapped || std::uint64_t(r.tapNext + 1) * 16 > r.taps.bytes) return false;
  float* t = static_cast<float*>(r.taps.mapped) + std::size_t(r.tapNext) * 4;
  t[0] = t[1] = t[2] = t[3] = 0.f;
  off = r.tapNext++;
  return true;
}

// Portrait retouch (docs section 43): heal pass, retouch planes, combine. `last`: this part ends the pipeline (the epilogue is ours).
H recordRetouch(GpuRecorder& r, const LocalDetailNode& n, H D, bool last) {
  const FramePlan& P = *n.plan;
  const local::Frame& f = *P.local;
  const local::Affine A = local::frameAffine(P);
  if (!f.spots.empty()) {
    std::vector<local::SpotPx> sp;
    local::spotsForWindow(f, A, n.ox, n.oy, n.W, n.Hh, sp);
    const bool lastHere = last && !f.retouchW;
    if (!sp.empty() || lastHere) {
      HealU u{};
      u.dims[0] = n.W, u.dims[1] = n.Hh, u.dims[2] = n.ox, u.dims[3] = n.oy;
      u.grain[0] = n.ep.amp, u.grain[1] = n.ep.period;
      if (sp.empty()) {
        std::uint32_t off = 0;
        if (!emptyBin(r, off)) return r.err = "local heal: table buffer full", -1;
        u.tab[2] = 1, u.tab[3] = 1;
        u.bins[0] = 1 << 20, u.bins[1] = std::int32_t(off), u.bins[2] = std::int32_t(off);
      } else {
        // bins of at least 32 px, at most ~4096 of them (the table shares the frame slot's 256 KB host buffer with the film tap tables)
        const int bs = std::max(32, int(std::ceil(std::sqrt(double(n.W) * n.Hh / 4096.0) / 16.0)) * 16);
        const int nbx = cdiv(n.W, bs), nby = cdiv(n.Hh, bs), nb = nbx * nby;
        std::vector<std::vector<int>> lists(static_cast<std::size_t>(nb));
        for (std::size_t i = 0; i < sp.size(); ++i) {
          const local::SpotPx& s = sp[i];
          const int bx0 = std::clamp(int(std::floor((s.cx - s.R) / float(bs))), 0, nbx - 1);
          const int bx1 = std::clamp(int(std::floor((s.cx + s.R) / float(bs))), 0, nbx - 1);
          const int by0 = std::clamp(int(std::floor((s.cy - s.R) / float(bs))), 0, nby - 1);
          const int by1 = std::clamp(int(std::floor((s.cy + s.R) / float(bs))), 0, nby - 1);
          for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx) lists[std::size_t(by * nbx + bx)].push_back(int(i));
        }
        std::size_t nidx = 0;
        for (const auto& l : lists) nidx += l.size();
        const std::size_t hdr = 2 * sp.size(), ix = hdr + std::size_t(nb);
        const std::uint32_t total = std::uint32_t(ix + (nidx + 3) / 4);
        if (!r.taps.mapped || std::uint64_t(r.tapNext + total) * 16 > r.taps.bytes) return r.err = "local heal: table buffer full", -1;
        const std::uint32_t off = r.tapNext;
        float* t = static_cast<float*>(r.taps.mapped) + std::size_t(off) * 4;
        std::fill(t, t + std::size_t(total) * 4, 0.f);
        for (std::size_t i = 0; i < sp.size(); ++i) {
          const local::SpotPx& s = sp[i];
          float* q = t + i * 8;
          q[0] = s.cx, q[1] = s.cy, q[2] = s.R, q[3] = s.alpha;
          q[4] = float(s.ox), q[5] = float(s.oy), q[6] = float(s.stride);
        }
        std::size_t e = 0;
        for (int b = 0; b < nb; ++b) {
          t[(hdr + std::size_t(b)) * 4] = float(e);
          t[(hdr + std::size_t(b)) * 4 + 1] = float(lists[std::size_t(b)].size());
          for (int i : lists[std::size_t(b)]) t[ix * 4 + e++] = float(i);
        }
        r.tapNext += total;
        u.tab[0] = std::int32_t(off), u.tab[1] = std::int32_t(sp.size()), u.tab[2] = nbx, u.tab[3] = nby;
        u.bins[0] = bs, u.bins[1] = std::int32_t(off + hdr), u.bins[2] = std::int32_t(off + ix);
      }
      const hal::PipelineH p = r.pl("local_heal", {{10, lastHere ? 1 : 0}, {11, lastHere && n.ep.grain ? 1 : 0}});
      if (!p) return -1;
      const H out = lastHere ? -1 : r.tr(kF16, n.W, n.Hh);
      r.g.dispatch("local_heal", p,
                   {GpuGraph::R(D), GpuGraph::R(r.g.externalBuf(r.taps)), out >= 0 ? GpuGraph::W(out) : GpuGraph::R(r.dummy(kF16)),
                    lastHere ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
                   r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
      if (lastHere) return D;
      D = out;
    }
  }
  if (!f.retouchW) return D;
  const float ppu = std::min(float(local::affinePpu(A)), local::haloPpu(f, P.spatialScale));
  const local::RetouchRadii R = local::retouchRadii(f.iod * ppu);
  const H lf = r.blur(0, 3, D, n.W, n.Hh, {R.rf, 0, 0, 0}, 2, 0, kR32);
  if (lf < 0) return -1;
  const int lw = cdiv(n.W, 4), lh = cdiv(n.Hh, 4);
  const H yy = r.tr(kF32, lw, lh), s4 = r.tr(kF32, lw, lh);
  const std::int32_t ch[4] = {chanOf(f, kLoSmooth), chanOf(f, kLoEven), chanOf(f, kLoRedness), chanOf(f, kLoUnderEye)};
  const std::int32_t ch2[4] = {chanOf(f, kLoShine), chanOf(f, kLoFaceLight), -1, -1};
  {
    const hal::PipelineH p = r.pl("local_rt_down", {});
    if (!p) return -1;
    RtDownU u{};
    u.dims[0] = n.W, u.dims[1] = n.Hh, u.low[0] = lw, u.low[1] = lh;
    for (int k = 0; k < 4; ++k) u.ch[k] = ch[k], u.ch2[k] = ch2[k];
    r.g.dispatch("local_rt_down", p, {GpuGraph::R(D), GpuGraph::R(n.aux0), GpuGraph::R(n.aux1), GpuGraph::W(yy), GpuGraph::W(s4)}, r.gx(lw),
                 r.gy(lh), &u, sizeof u);
  }
  const H m = r.blur(2, 2, yy, lw, lh, {R.rc, R.rc, R.rc, R.rc}, 1, 0, kF32);
  if (m < 0) return -1;
  const H ab0 = r.tr(kF32, lw, lh);
  {
    const hal::PipelineH pa = r.pl("gf_ab", {});
    if (!pa) return -1;
    AbU u{};
    u.s[0] = lw, u.s[1] = lh;
    u.eps[0] = local::kRtEps;
    r.g.dispatch("gf_ab", pa, {GpuGraph::R(m), GpuGraph::W(ab0)}, r.gx(lw), r.gy(lh), &u, sizeof u);
  }
  const H ab = r.blur(2, 2, ab0, lw, lh, {R.rc, R.rc, R.rc, R.rc}, 1, 0, kF32);
  const H sref = r.blur(2, 2, s4, lw, lh, {R.rs, R.rs, R.rs, R.rs}, 2, 0, kF32);
  const H sblot = r.blur(2, 2, s4, lw, lh, {R.rb, R.rb, R.rb, R.rb}, 2, 0, kF32);  // the blotch-scale colour mean
  if (ab < 0 || sref < 0 || sblot < 0) return -1;
  const hal::PipelineH p = r.pl("local_retouch", {{10, last ? 1 : 0}, {11, last && n.ep.grain ? 1 : 0}});
  if (!p) return -1;
  const H out = last ? -1 : r.tr(kF16, n.W, n.Hh);
  RtU u{};
  u.dims[0] = n.W, u.dims[1] = n.Hh, u.dims[2] = n.ox, u.dims[3] = n.oy;
  u.low[0] = lw, u.low[1] = lh;
  u.grain[0] = n.ep.amp, u.grain[1] = n.ep.period;
  for (int k = 0; k < 4; ++k) u.ch[k] = ch[k], u.ch2[k] = ch2[k];
  r.g.dispatch("local_retouch", p,
               {GpuGraph::R(D), GpuGraph::R(n.aux0), GpuGraph::R(n.aux1), GpuGraph::R(lf), GpuGraph::R(ab), GpuGraph::R(sref), GpuGraph::R(sblot),
                out >= 0 ? GpuGraph::W(out) : GpuGraph::R(r.dummy(kF16)), last ? GpuGraph::W(n.ep.out8) : GpuGraph::R(r.dummy(kU8))},
               r.gx(n.W), r.gy(n.Hh), &u, sizeof u);
  return last ? D : out;
}
}  // namespace

H recordLocalDetail(GpuRecorder& r, const LocalDetailNode& n, H D) {
  const FramePlan& P = *n.plan;
  if (!P.local) return r.err = "LocalDetail without local adjustments", -1;
  const local::Frame& f = *P.local;
  if (f.retouchW || !f.spots.empty()) {  // portrait retouch first (docs section 43), then the legacy window ops on its result
    D = recordRetouch(r, n, D, n.ep.last && !f.legacyW);
    if (D < 0 || !f.legacyW) return D;
  }
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
  // legacy op -> amount channel: the identity on a legacy frame (the point pass wrote every legacy channel), the packed map otherwise
  for (int k = 0; k < 4; ++k) u.ch[k] = f.retouchW ? chanOf(f, kLocalWindowFirst + k) : k;
  u.ch2[0] = f.retouchW ? chanOf(f, kLoDehaze) : 4;
  u.ch2[1] = u.ch2[2] = u.ch2[3] = -1;
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
