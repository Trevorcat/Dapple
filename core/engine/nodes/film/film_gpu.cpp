// Film nodes as GPU passes. See film_gpu.h.
#include "engine/nodes/film/film_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/nodes/film/film_node_internal.h"

namespace dapple::engine::filmnode {

float halfToFloat(std::uint16_t h) {
  const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
  std::uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {  // denormal
      int e = -1;
      std::uint32_t m = man;
      do {
        ++e;
        m <<= 1;
      } while (!(m & 0x400u));
      bits = sign | std::uint32_t(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | man << 13;
  } else {
    bits = sign | (exp + 112) << 23 | man << 13;
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

using gpu::GpuGraph;
using gpu::H;

namespace {

struct PointU {
  float m0[4], m1[4], m2[4];
  float gam[4];
  float dq[4][4];
  float dmin[4], dscale[4];
  std::int32_t dim[4];
  std::int32_t lutN[4];
  float misc[4];
  std::int32_t src[4];
  float dsub[4];
};

struct GrainU {
  std::int32_t dim[4], org[4];
  std::uint32_t stream[4];
  float pa[3][4], pb[3][4], pc[3][4], pd[3][4];
};
struct PostU {
  std::int32_t dim[4], org[4];
  float glare[4], white[4];
  std::uint32_t stream[4];
  float taps[20];
};

PointU pointUniforms(const FilmResolved& r, const GpuFilmTables& tb, int w, int h) {
  PointU u{};
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 4; ++i) {
      const float v = tb.mIn[c * 4 + i] * std::max(0.f, r.gain[c]);
      (c == 0 ? u.m0 : c == 1 ? u.m1 : u.m2)[i] = v;
    }
  for (int c = 0; c < 3; ++c) u.gam[c] = tb.gamma[c];
  u.gam[3] = tb.curveX0;
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 4; ++i) u.dq[k][i] = tb.dirQ[k * 4 + i];
  for (int i = 0; i < 4; ++i) u.dq[3][i] = tb.dirQ0[i];
  for (int i = 0; i < 4; ++i) {
    u.dmin[i] = tb.dmin[i];
    u.dscale[i] = tb.dscale[i];
  }
  u.dim[0] = w;
  u.dim[1] = h;
  u.dim[2] = tb.rawN;
  u.dim[3] = tb.curveN;
  u.lutN[0] = tb.lutN;
  u.misc[0] = tb.curveInvDx;
  u.misc[1] = 1.f;
  return u;
}

// ---- Gaussian mixtures (mirror of film_spatial.cpp mixImpl / levelCombine) --------------------------------------------------------------
struct MixU {
  std::int32_t dim[4];
  float s[4];
  float taps[20];
};
constexpr int kMaxTaps = 19;

struct LevelComp {
  std::vector<float> w;  // half taps, unit sum
  float weight = 0;
  int r = 0;
};

struct Mix {
  gpu::GpuRecorder& r;
  bool ok = true;
  H img(int w, int h) { return r.g.transient(hal::Format::R32F, std::uint32_t(w), std::uint32_t(h)); }
  void disp(const char* shader, const MixU& u, std::initializer_list<GpuGraph::Use> uses, int w, int h) {
    const hal::PipelineH p = r.pl(shader, {});
    if (!p) {
      ok = false;
      return;
    }
    r.g.dispatch(shader, p, uses, r.gx(w), r.gy(h), &u, sizeof u);
  }
  H down(H src, int sw, int sh) {
    const int dw = (sw + 1) / 2, dh = (sh + 1) / 2;
    const H d = img(dw, dh);
    MixU u{};
    u.dim[0] = dw;
    u.dim[1] = dh;
    u.dim[2] = sw;
    u.dim[3] = sh;
    disp("film_down", u, {GpuGraph::R(src), GpuGraph::W(d)}, dw, dh);
    return d;
  }
  // dst = w0 * src + sum of components of src (+ upsample of `up`), size w x h
  H levelCombine(H src, int w, int h, double w0, const std::vector<LevelComp>& comps, H up, int upW = 0, int upH = 0) {
    const H dst = img(w, h);
    MixU u{};
    u.dim[0] = w;
    u.dim[1] = h;
    u.dim[2] = up >= 0 ? upW : 0;
    u.dim[3] = up >= 0 ? upH : 0;
    u.s[0] = float(w0);
    disp("film_base", u, {GpuGraph::R(src), GpuGraph::R(up >= 0 ? up : src), GpuGraph::W(dst)}, w, h);
    for (const LevelComp& c : comps) {
      if (c.r > kMaxTaps) {
        ok = false;
        r.err = "film GPU: mixture component too wide for the tap table";
        return dst;
      }
      MixU f{};
      f.dim[0] = w;
      f.dim[1] = h;
      f.dim[2] = c.r;
      f.s[0] = c.weight;
      for (int i = 0; i <= c.r; ++i) f.taps[i] = c.w[std::size_t(i)];
      const H tmp = img(w, h);
      disp("film_firh", f, {GpuGraph::R(src), GpuGraph::W(tmp)}, w, h);
      disp("film_firv", f, {GpuGraph::R(tmp), GpuGraph::RW(dst)}, w, h);
    }
    return dst;
  }
  // `src`: full resolution (w x h) input, or the half-resolution input when `half`.
  H mixture(H src, int W, int Hh, const fd::Mixture& mix, bool half) {
    const double kMinLowSigma = half ? 1.6 : 2.0;
    double w0 = mix.w0;
    int avail = 0;
    {
      int w = W, h = Hh;
      while (avail < 14) {
        const int nw = (w + 1) / 2, nh = (h + 1) / 2;
        if (nw < 2 || nh < 2) break;
        w = nw;
        h = nh;
        ++avail;
      }
    }
    std::vector<std::vector<LevelComp>> per(std::size_t(avail) + 1);
    int maxLevel = 0;
    for (const fd::GaussComp& g : mix.comps) {
      if (g.w == 0) continue;
      LevelComp lc;
      int k = 0;
      double sl = g.sigma;
      if (g.sigma >= 3.0) {
        k = int(std::floor(std::log2(g.sigma / kMinLowSigma)));
        k = std::clamp(k, half ? 1 : 0, avail);
        if (k > 0) {
          const double f = double(1 << std::min(k, 30));
          sl = std::sqrt(std::max(g.sigma * g.sigma - (f * f - 1.0) / 12.0, 0.25)) / f;
        }
      }
      std::vector<float> taps;
      int rr;
      if (g.sigma < 3.0) {
        fd::gaussTaps(g.sigma, taps, rr);
      } else {
        rr = int(std::ceil(4.0 * sl));
        std::vector<double> t(std::size_t(rr) + 1);
        double sum = 0;
        for (int i = 0; i <= rr; ++i) {
          t[std::size_t(i)] = std::exp(-double(i) * i / (2.0 * sl * sl));
          sum += i == 0 ? t[std::size_t(i)] : 2 * t[std::size_t(i)];
        }
        taps.resize(std::size_t(rr) + 1);
        for (int i = 0; i <= rr; ++i) taps[std::size_t(i)] = float(t[std::size_t(i)] / sum);
      }
      if (rr == 0) {
        w0 += g.w;
        continue;
      }
      lc.r = rr;
      lc.w = taps;
      lc.weight = float(g.w);
      per[std::size_t(k)].push_back(std::move(lc));
      maxLevel = std::max(maxLevel, k);
    }
    if (maxLevel == 0) return levelCombine(src, W, Hh, w0, per[0], -1);
    std::vector<H> P(std::size_t(maxLevel) + 1, -1), L(std::size_t(maxLevel) + 2, -1);
    std::vector<int> pw(std::size_t(maxLevel) + 1), ph(std::size_t(maxLevel) + 1);
    pw[0] = W;
    ph[0] = Hh;
    if (!half) P[0] = src;
    for (int k = 1; k <= maxLevel; ++k) {
      pw[std::size_t(k)] = (pw[std::size_t(k) - 1] + 1) / 2;
      ph[std::size_t(k)] = (ph[std::size_t(k) - 1] + 1) / 2;
      if (k == 1 && half) {
        P[1] = src;
        continue;
      }
      P[std::size_t(k)] = down(P[std::size_t(k) - 1], pw[std::size_t(k) - 1], ph[std::size_t(k) - 1]);
    }
    for (int k = maxLevel; k >= 1; --k)
      L[std::size_t(k)] = levelCombine(P[std::size_t(k)], pw[std::size_t(k)], ph[std::size_t(k)], 0.0, per[std::size_t(k)], k < maxLevel ? L[std::size_t(k) + 1] : -1,
                              k < maxLevel ? pw[std::size_t(k) + 1] : 0, k < maxLevel ? ph[std::size_t(k) + 1] : 0);
    // final level: dst = w0 * src + comps(src) + up2(L1); the half input has no level-0 components
    return half ? levelCombine(L[1], W, Hh, 0.0, {}, L[1], pw[1], ph[1]) : levelCombine(src, W, Hh, w0, per[0], L[1], pw[1], ph[1]);
  }

  H split(H rgba, int w, int h, int c) {
    const H d = img(w, h);
    MixU u{};
    u.dim[0] = w;
    u.dim[1] = h;
    u.dim[3] = c;
    disp("film_split", u, {GpuGraph::R(rgba), GpuGraph::W(d)}, w, h);
    return d;
  }
  H merge(H a, H b, H c, int w, int h) {
    const H d = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
    MixU u{};
    u.dim[0] = w;
    u.dim[1] = h;
    disp("film_merge", u, {GpuGraph::R(a), GpuGraph::R(b), GpuGraph::R(c), GpuGraph::W(d)}, w, h);
    return d;
  }
};

// ---- Gaussian mixtures of all three channels at once (film_mix3.comp, section 20.3) -------------------------------------------------------
// The same decomposition as Mix::mixture (pyramid level per component, direct taps below sigma 3), but the planes are RGBA32F with one
// channel per colour and a pass filters up to four component slots of all channels while it reads its input once. A V pass sums its
// slots and carries the accumulator, so a level costs one H and one V pass per four component slots (the base pass is folded in).
struct Comp3 {
  int r = 0;
  std::vector<float> w;  // half taps, unit sum
  float weight = 0;
};
struct Mix3U {
  std::int32_t dim[4];
  std::int32_t sz[4];
  float w0[4];
};
struct Mix3 {
  gpu::GpuRecorder& r;
  bool ok = true;
  H img(int w, int h) { return r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h)); }
  H tapsH = -1;
  // `n` vec4 of tap table in the slot's host-visible buffer; returns the offset (vec4 units) or -1
  float* tapTable(int n, std::int32_t& off) {
    if (!r.taps.mapped || std::uint64_t(r.tapNext + n) * 16 > r.taps.bytes) {
      ok = false;
      r.err = "film GPU: tap table buffer full";
      return nullptr;
    }
    if (tapsH < 0) tapsH = r.g.externalBuf(r.taps);
    off = std::int32_t(r.tapNext);
    float* p = static_cast<float*>(r.taps.mapped) + std::size_t(r.tapNext) * 4;
    r.tapNext += std::uint32_t(n);
    std::memset(p, 0, std::size_t(n) * 16);
    return p;
  }
  bool pass(const char* shader, std::initializer_list<hal::SpecConst> spec, const Mix3U& u, std::initializer_list<GpuGraph::Use> uses, int w, int h) {
    const hal::PipelineH p = r.pl(shader, spec);
    if (!p) return ok = false;
    r.g.dispatch(shader, p, uses, r.gx(w), r.gy(h), &u, sizeof u);
    return true;
  }
  H down(H src, int sw, int sh) {
    const int dw = (sw + 1) / 2, dh = (sh + 1) / 2;
    const H d = img(dw, dh);
    Mix3U u{};
    u.dim[0] = dw;
    u.dim[1] = dh;
    u.sz[0] = sw;
    u.sz[1] = sh;
    pass("film_d3", {}, u, {GpuGraph::R(src), GpuGraph::W(d)}, dw, dh);
    return d;
  }
  // dst = w0 * src + comps(src) (+ upsample of `up`), size w x h; comps[c] = the components of channel c at this level
  H combine(H src, int w, int h, const double w0[3], std::vector<Comp3> comps[3], H up, int upW, int upH) {
    for (int c = 0; c < 3; ++c)
      std::stable_sort(comps[c].begin(), comps[c].end(), [](const Comp3& a, const Comp3& b) { return a.r > b.r; });
    std::size_t nj = 0;
    for (int c = 0; c < 3; ++c) nj = std::max(nj, comps[c].size());
    const int hasUp = up >= 0 ? 1 : 0;
    Mix3U base{};
    base.dim[0] = w;
    base.dim[1] = h;
    base.sz[0] = hasUp ? upW : 0;
    base.sz[1] = hasUp ? upH : 0;
    for (int c = 0; c < 3; ++c) base.w0[c] = float(w0[c]);
    if (nj == 0) {
      const H dst = img(w, h);
      pass("film_b3", {{2, hasUp}}, base, {GpuGraph::R(src), GpuGraph::R(hasUp ? up : src), GpuGraph::W(dst)}, w, h);
      return dst;
    }
    H acc = -1;
    for (std::size_t j0 = 0; j0 < nj; j0 += 4) {
      const int nc = int(std::min<std::size_t>(4, nj - j0));
      Mix3U hu = base, vu = base;
      float* ht = tapTable(nc * 20, hu.dim[3]);
      float* vt = tapTable(nc * 20, vu.dim[3]);
      if (!ht || !vt) return src;
      int R = 0;
      for (int jj = 0; jj < nc; ++jj)
        for (int c = 0; c < 3; ++c) {
          if (j0 + jj >= comps[c].size()) continue;
          const Comp3& cp = comps[c][j0 + jj];
          if (cp.r > kMaxTaps) {
            ok = false;
            r.err = "film GPU: mixture component too wide for the tap table";
            return src;
          }
          R = std::max(R, cp.r);
          for (int k = 0; k <= cp.r; ++k) {
            ht[(jj * 20 + k) * 4 + c] = cp.w[std::size_t(k)];
            vt[(jj * 20 + k) * 4 + c] = cp.weight * cp.w[std::size_t(k)];
          }
        }
      hu.dim[2] = vu.dim[2] = R;
      vu.sz[2] = r.lx;
      vu.sz[3] = r.ly;
      H t[4];
      for (int jj = 0; jj < 4; ++jj) t[jj] = r.g.transient(hal::Format::RGBA16F, std::uint32_t(jj < nc ? w : 1), std::uint32_t(jj < nc ? h : 1));
      pass("film_h3", {{0, nc}}, hu,
           {GpuGraph::R(src), GpuGraph::W(t[0]), GpuGraph::W(t[1]), GpuGraph::W(t[2]), GpuGraph::W(t[3]), GpuGraph::R(tapsH)}, w, h);
      const bool first = j0 == 0;
      const H A = first ? src : acc;
      const H dst = img(w, h);
      const H d = r.dummy(hal::Format::RGBA16F);
      pass("film_v3", {{0, nc}, {1, first ? 1 : 0}, {2, first && hasUp ? 1 : 0}}, vu,
           {GpuGraph::R(t[0]), GpuGraph::R(nc > 1 ? t[1] : d), GpuGraph::R(nc > 2 ? t[2] : d), GpuGraph::R(nc > 3 ? t[3] : d), GpuGraph::R(A),
            GpuGraph::R(first && hasUp ? up : A), GpuGraph::W(dst), GpuGraph::R(tapsH)},
           w, h);
      acc = dst;
    }
    return acc;
  }
  // `src`: full resolution (W x Hh) RGBA32F input, or the half-resolution input when `half`.
  H mixture(H src, int W, int Hh, const fd::Mixture* mix, bool half) {
    const double kMinLowSigma = half ? 1.6 : 2.0;
    int avail = 0;
    {
      int w = W, h = Hh;
      while (avail < 14) {
        const int nw = (w + 1) / 2, nh = (h + 1) / 2;
        if (nw < 2 || nh < 2) break;
        w = nw;
        h = nh;
        ++avail;
      }
    }
    double w0[3];
    std::vector<std::vector<Comp3>> per[3];  // [channel][level]
    int maxLevel = 0;
    for (int c = 0; c < 3; ++c) {
      w0[c] = mix[c].w0;
      per[c].resize(std::size_t(avail) + 1);
      for (const fd::GaussComp& g : mix[c].comps) {
        if (g.w == 0) continue;
        Comp3 lc;
        int k = 0;
        double sl = g.sigma;
        if (g.sigma >= 3.0) {
          k = int(std::floor(std::log2(g.sigma / kMinLowSigma)));
          k = std::clamp(k, half ? 1 : 0, avail);
          if (k > 0) {
            const double f = double(1 << std::min(k, 30));
            sl = std::sqrt(std::max(g.sigma * g.sigma - (f * f - 1.0) / 12.0, 0.25)) / f;
          }
        }
        std::vector<float> taps;
        int rr;
        if (g.sigma < 3.0) {
          fd::gaussTaps(g.sigma, taps, rr);
        } else {
          rr = int(std::ceil(4.0 * sl));
          std::vector<double> t(std::size_t(rr) + 1);
          double sum = 0;
          for (int i = 0; i <= rr; ++i) {
            t[std::size_t(i)] = std::exp(-double(i) * i / (2.0 * sl * sl));
            sum += i == 0 ? t[std::size_t(i)] : 2 * t[std::size_t(i)];
          }
          taps.resize(std::size_t(rr) + 1);
          for (int i = 0; i <= rr; ++i) taps[std::size_t(i)] = float(t[std::size_t(i)] / sum);
        }
        if (rr == 0) {
          w0[c] += g.w;
          continue;
        }
        lc.r = rr;
        lc.w = taps;
        lc.weight = float(g.w);
        per[c][std::size_t(k)].push_back(std::move(lc));
        maxLevel = std::max(maxLevel, k);
      }
    }
    auto level = [&](int k, std::vector<Comp3> (&out)[3]) {
      for (int c = 0; c < 3; ++c) out[c] = per[c][std::size_t(k)];
    };
    std::vector<Comp3> lv[3];
    if (maxLevel == 0) {
      level(0, lv);
      return combine(src, W, Hh, w0, lv, -1, 0, 0);
    }
    std::vector<H> P(std::size_t(maxLevel) + 1, -1), L(std::size_t(maxLevel) + 2, -1);
    std::vector<int> pw(std::size_t(maxLevel) + 1), ph(std::size_t(maxLevel) + 1);
    pw[0] = W;
    ph[0] = Hh;
    if (!half) P[0] = src;
    for (int k = 1; k <= maxLevel; ++k) {
      pw[std::size_t(k)] = (pw[std::size_t(k) - 1] + 1) / 2;
      ph[std::size_t(k)] = (ph[std::size_t(k) - 1] + 1) / 2;
      if (k == 1 && half) {
        P[1] = src;
        continue;
      }
      P[std::size_t(k)] = down(P[std::size_t(k) - 1], pw[std::size_t(k) - 1], ph[std::size_t(k) - 1]);
    }
    const double zero[3] = {0, 0, 0};
    for (int k = maxLevel; k >= 1; --k) {
      level(k, lv);
      L[std::size_t(k)] = combine(P[std::size_t(k)], pw[std::size_t(k)], ph[std::size_t(k)], zero, lv, k < maxLevel ? L[std::size_t(k) + 1] : -1,
                                  k < maxLevel ? pw[std::size_t(k) + 1] : 0, k < maxLevel ? ph[std::size_t(k) + 1] : 0);
    }
    // final level: dst = w0 * src + comps(src) + up2(L1); the half input has no level-0 components
    if (half) {
      std::vector<Comp3> none[3];
      return combine(L[1], W, Hh, zero, none, L[1], pw[1], ph[1]);
    }
    level(0, lv);
    return combine(src, W, Hh, w0, lv, L[1], pw[1], ph[1]);
  }
};

// Separable FIR on a 3-channel image (half taps, unit sum), reflect boundary.
H fir4(gpu::GpuRecorder& r, H in, int w, int h, const Taps& t) {
  if (t.r > kMaxTaps) {
    r.err = "film GPU: blur kernel too wide";
    return -1;
  }
  const hal::PipelineH ph = r.pl("film_f4h", {}), pv = r.pl("film_f4v", {});
  if (!ph || !pv) return -1;
  PostU u{};
  u.dim[0] = w;
  u.dim[1] = h;
  u.dim[2] = t.r;
  for (int i = 0; i <= t.r; ++i) u.taps[i] = t.w[std::size_t(i)];
  const H tmp = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
  const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
  r.g.dispatch("film_f4h", ph, {GpuGraph::R(in), GpuGraph::W(tmp)}, r.gx(w), r.gy(h), &u, sizeof u);
  r.g.dispatch("film_f4v", pv, {GpuGraph::R(tmp), GpuGraph::W(out)}, r.gx(w), r.gy(h), &u, sizeof u);
  return out;
}

}  // namespace

bool uploadGpuTables(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err) {
  hal::Device& dev = core.dev();
  const fd::FilmTables& raw = tb.raw->t;
  const fd::FilmTables& cur = tb.curves->t;
  const fd::FilmTables& pr = tb.print->t;
  out.rawN = raw.rawN;
  out.curveN = cur.curveN;
  out.lutN = pr.lutN;
  std::copy(raw.mIn, raw.mIn + 12, out.mIn);
  std::copy(cur.gamma, cur.gamma + 4, out.gamma);
  out.curveX0 = cur.curveX0;
  out.curveInvDx = cur.curveInvDx;
  std::copy(cur.dirQ, cur.dirQ + 12, out.dirQ);
  std::copy(cur.dirQ0, cur.dirQ0 + 4, out.dirQ0);
  std::copy(pr.dmin, pr.dmin + 4, out.dmin);
  std::copy(pr.dscale, pr.dscale + 4, out.dscale);
  std::copy(pr.glareRgb, pr.glareRgb + 4, out.glareRgb);
  const std::uint64_t rawBytes = raw.raw.size() * 4, lutBytes = pr.lut.size() * 4, curveBytes = (cur.dn.size() + cur.d0.size()) * 4,
                      decBytes = pr.srgbDecode.size() * 4;
  const std::uint32_t rp = std::uint32_t(raw.rawN + 2);
  if (dev.createImage({hal::Format::RGBA32F, rp, rp, 1}, &out.raw) != hal::Status::Ok ||
      dev.createImage({hal::Format::RGBA32F, std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN)}, &out.lut) != hal::Status::Ok ||
      dev.createBuffer({curveBytes, hal::MemKind::DeviceLocal, true}, &out.curves) != hal::Status::Ok ||
      dev.createBuffer({decBytes, hal::MemKind::DeviceLocal, true}, &out.decode) != hal::Status::Ok)
    return err = dev.lastError(), false;
  auto cl = dev.begin();
  if (!cl) return err = dev.lastError(), false;
  if (!core.ensureStaging(cl->slot(), rawBytes + lutBytes + curveBytes + decBytes, err)) return false;
  gpu::SlotRes& s = core.slot(cl->slot());
  auto* base = static_cast<std::uint8_t*>(s.staging.mapped);
  std::memcpy(base, raw.raw.data(), rawBytes);
  std::memcpy(base + rawBytes, pr.lut.data(), lutBytes);
  std::memcpy(base + rawBytes + lutBytes, cur.dn.data(), cur.dn.size() * 4);
  std::memcpy(base + rawBytes + lutBytes + cur.dn.size() * 4, cur.d0.data(), cur.d0.size() * 4);
  std::memcpy(base + rawBytes + lutBytes + curveBytes, pr.srgbDecode.data(), decBytes);
  GpuGraph g(dev, core.pool());
  g.copyBufferToImage(s.staging, g.external(out.raw), rp, rp, 1, 0);
  g.copyBufferToImage(s.staging, g.external(out.lut), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), rawBytes);
  g.copyBufferToBuffer(s.staging, g.externalBuf(out.curves), curveBytes, rawBytes + lutBytes);
  g.copyBufferToBuffer(s.staging, g.externalBuf(out.decode), decBytes, rawBytes + lutBytes + curveBytes);
  if (gpu::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok) return false;
  if (dev.waitAll() != hal::Status::Ok) return err = dev.lastError(), false;
  return true;
}

void releaseGpuTables(gpu::Core& core, GpuFilmTables& t) {
  hal::Device& dev = core.dev();
  dev.waitIdle();
  dev.destroy(t.raw);
  dev.destroy(t.lut);
  dev.destroy(t.curves);
  dev.destroy(t.decode);
  t = GpuFilmTables{};
}

bool uploadGpuPlan(gpu::Core& core, const FilmResolved& res, GpuFilmPlanRes& out, std::string& err) {
  out = GpuFilmPlanRes{};
  const auto pd = planOf(res);
  const fd::GrainPlan& gp = pd->plan.grain;
  if (!gp.active) return true;
  hal::Device& dev = core.dev();
  const int N = gp.ch[0].tableN;
  const std::size_t per = std::size_t(N + 2) * 4;
  const std::uint64_t bytes = per * 2 * 3 * 4;
  if (dev.createBuffer({bytes, hal::MemKind::DeviceLocal, true}, &out.grainTab) != hal::Status::Ok) return err = dev.lastError(), false;
  auto cl = dev.begin();
  if (!cl || !core.ensureStaging(cl->slot(), bytes, err)) return false;
  auto* dst = static_cast<float*>(core.slot(cl->slot()).staging.mapped);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(dst + std::size_t(c) * 2 * per, gp.ch[c].inv.data(), per * 4);
    std::memcpy(dst + std::size_t(c) * 2 * per + per, gp.ch[c].merged.data(), per * 4);
  }
  GpuGraph g(dev, core.pool());
  g.copyBufferToBuffer(core.slot(cl->slot()).staging, g.externalBuf(out.grainTab), bytes, 0);
  if (gpu::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok) return false;
  if (dev.waitAll() != hal::Status::Ok) return err = dev.lastError(), false;
  out.grainTableN = N;
  return true;
}

void releaseGpuPlan(gpu::Core& core, GpuFilmPlanRes& t) {
  core.dev().waitIdle();
  core.dev().destroy(t.grainTab);
  t = GpuFilmPlanRes{};
}

bool filmGpuSupported(const FilmResolved& res, std::string& why) {
  const auto pd = planOf(res);
  if (!pd->error.empty() || pd->split || pd->plan.boost) return why = "film GPU: unsupported effect", false;
  const fd::GrainPlan& gp = pd->plan.grain;
  if (gp.active) {
    if (gp.microActive) return why = "film GPU: grain micro-structure not ported", false;
    for (int ch = 0; ch < 3; ++ch)
      for (int l = 0; l < gp.ch[ch].layers; ++l) {
        const Taps& t = pd->dye[ch][l];
        if (t.r > 0 && t.w.size() > 1 && t.w[1] > 1e-5f) return why = "film GPU: dye-cloud blur not ported", false;
      }
  }
  return true;
}

H recordFilmFrame(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pres, H src, int w, int h) {
  const auto pd = planOf(res);
  if (!pd->error.empty() || pd->split || pd->plan.boost) {
    r.err = "film GPU: unsupported effect";
    return -1;
  }
  const fd::SpatialPlan& pl = pd->plan;
  const PointU u0 = pointUniforms(res, tb, w, h);
  const H rawH = r.g.external(tb.raw), lutH = r.g.external(tb.lut), curH = r.g.externalBuf(tb.curves);
  const std::uint32_t gx16 = 16;
  (void)gx16;
  auto point = [&](int mode, bool hasQ, H in, H aux, int ow, int oh, int offset = 0, const float* dsub = nullptr, int srcW = 0, int srcH = 0) -> H {
    const hal::PipelineH p = r.pl("film_point", {{0, mode}, {1, hasQ ? 1 : 0}});
    if (!p) return -1;
    PointU u = u0;
    u.dim[0] = ow;
    u.dim[1] = oh;
    u.lutN[1] = u.lutN[2] = offset;
    u.src[0] = srcW;
    u.src[1] = srcH;
    if (dsub)
      for (int c = 0; c < 3; ++c) u.dsub[c] = dsub[c];
    const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(ow), std::uint32_t(oh));
    r.g.dispatch("film_point", p, {GpuGraph::R(in), GpuGraph::R(aux), GpuGraph::R(lutH), GpuGraph::R(curH), GpuGraph::W(out)},
                 r.gx(ow), r.gy(oh), &u, sizeof u);
    return out;
  };
  if (!pl.any()) return point(3, false, src, rawH, w, h);  // the fused point chain

  H E = point(0, false, src, rawH, w, h);
  if (E < 0) return -1;
  Mix mix{r};   // single plane mixtures (glare field)
  Mix3 mix3{r}; // three channels at once
  H E2 = E;
  if (pl.preMix) E2 = mix3.mixture(E2, w, h, pl.pre, false);
  if (pl.haloMix) E2 = mix3.mixture(E2, w, h, pl.halo, false);
  H Q = -1;
  if (pl.dirDiffuse) {
    if (pd->dirHalf) {
      const int hw = (w + 1) / 2, hh = (h + 1) / 2;
      const H qh = point(4, false, E2, rawH, hw, hh, 0, nullptr, w, h);
      if (qh < 0) return -1;
      Q = mix3.mixture(qh, w, h, pl.dir, true);
    } else {
      const H q = point(5, false, E2, rawH, w, h);
      if (q < 0) return -1;
      Q = mix3.mixture(q, w, h, pl.dir, false);
    }
  }
  if (!mix.ok || !mix3.ok) return -1;
  H D = point(1, Q >= 0, E2, Q >= 0 ? Q : rawH, w, h);
  if (D < 0) return -1;

  // ---- grain: sampled on the frame grown by the blur halo (hash on absolute coordinates), blurred, minimum density removed -------------
  int off = 0;
  float dsub[4] = {0, 0, 0, 0};
  if (pl.grain.active) {
    const fd::GrainPlan& gp = pl.grain;
    for (int ch = 0; ch < 3; ++ch)
      for (int l = 0; l < gp.ch[ch].layers; ++l) {
        const Taps& t = pd->dye[ch][l];
        // dye-cloud blur of sigma ~0.17 px has numerically zero side taps; a wider one is not ported
        if (t.r > 0 && t.w.size() > 1 && t.w[1] > 1e-5f) {
          r.err = "film GPU: dye-cloud blur not ported";
          return -1;
        }
      }
    if (gp.microActive || !pres.grainTab) {
      r.err = "film GPU: grain micro-structure / tables not available";
      return -1;
    }
    const int Hg = pd->grain.r;
    const int gw = w + 2 * Hg, gh = h + 2 * Hg;
    GrainU gu{};
    gu.dim[0] = gw;
    gu.dim[1] = gh;
    gu.dim[2] = w;
    gu.dim[3] = h;
    gu.org[0] = gu.org[1] = -Hg;
    gu.org[2] = gp.ch[0].layers;
    gu.org[3] = pres.grainTableN;
    for (int c = 0; c < 3; ++c) {
      const fd::GrainChannelPlan& gc = gp.ch[c];
      gu.stream[c] = pd->streamGrain[c];
      for (int l = 0; l < 3; ++l) {
        gu.pa[c][l] = gc.dminL[l];
        gu.pb[c][l] = gc.invDmaxL[l];
        gu.pc[c][l] = gc.nL[l];
        gu.pd[c][l] = gc.scaleL[l];
      }
      gu.pa[c][3] = gc.invScale;
      gu.pb[c][3] = gc.u;
      gu.pc[c][3] = gc.lambdaSwitch;
      dsub[c] = gc.dminCh;
    }
    const hal::PipelineH pg = r.pl("film_grain", {});
    if (!pg) return -1;
    H G = r.g.transient(hal::Format::RGBA16F, std::uint32_t(gw), std::uint32_t(gh));
    r.g.dispatch("film_grain", pg, {GpuGraph::R(D), GpuGraph::R(r.g.externalBuf(pres.grainTab)), GpuGraph::W(G)},
                 r.gx(gw), r.gy(gh), &gu, sizeof gu);
    if (Hg > 0) {
      G = fir4(r, G, gw, gh, pd->grain);
      if (G < 0) return -1;
    }
    D = G;
    off = Hg;
  }
  if (!pd->scanStage) return point(2, false, D, rawH, w, h, off, dsub);

  // ---- FilmGlare + FilmScan --------------------------------------------------------------------------------------------------------
  const std::uint32_t gxw = r.gx(w), gyh = r.gy(h);
  PostU pu{};
  pu.dim[0] = w;
  pu.dim[1] = h;
  // the glare field first (it does not depend on the picture): blurred log-normal hash noise over the frame grown by the blur radius
  H field = -1;
  int rgl = 0;
  if (pl.glare) {
    rgl = pd->rgl;
    if (rgl > kMaxTaps) {
      r.err = "film GPU: glare blur too wide";
      return -1;
    }
    PostU gu = pu;
    const int fw = w + 2 * rgl, fh = h + 2 * rgl;
    gu.dim[0] = fw;
    gu.dim[1] = fh;
    gu.org[0] = gu.org[1] = -rgl;
    gu.glare[0] = float(pl.glareMu);
    gu.glare[1] = float(pl.glareSigma);
    gu.stream[0] = pd->streamGlare;
    const hal::PipelineH p = r.pl("film_gnoise", {});
    if (!p) return -1;
    field = r.g.transient(hal::Format::R32F, std::uint32_t(fw), std::uint32_t(fh));
    r.g.dispatch("film_gnoise", p, {GpuGraph::W(field)}, r.gx(fw), r.gy(fh), &gu, sizeof gu);
    if (rgl > 0) {
      std::vector<LevelComp> comps(1);
      comps[0].r = rgl;
      comps[0].w = pd->glare.w;
      comps[0].weight = 1.f;
      field = mix.levelCombine(field, fw, fh, 0.0, comps, -1);
    }
  }
  // FilmPrintScan + the sRGB decode + the glare add in one pass: linear light
  H L;
  {
    const hal::PipelineH p = r.pl("film_pd", {{0, 2}});
    if (!p) return -1;
    PointU u = u0;
    u.dim[0] = w;
    u.dim[1] = h;
    u.lutN[1] = u.lutN[2] = off;
    for (int c = 0; c < 3; ++c) u.dsub[c] = dsub[c];
    for (int c = 0; c < 3; ++c) u.dq[0][c] = tb.glareRgb[c];  // linear display white
    u.dq[0][3] = tb.glareRgb[3];                              // glare mean
    u.src[0] = u.src[1] = rgl;
    u.src[2] = field >= 0 ? 1 : 0;
    L = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
    r.g.dispatch("film_pd", p,
                 {GpuGraph::R(D), GpuGraph::R(rawH), GpuGraph::R(lutH), GpuGraph::R(curH), GpuGraph::W(L),
                  GpuGraph::R(field >= 0 ? field : r.dummy(hal::Format::R32F)), GpuGraph::R(r.g.externalBuf(tb.decode))},
                 r.gx(w), r.gy(h), &u, sizeof u);
  }
  if (pd->scan.r > 0) {
    L = fir4(r, L, w, h, pd->scan);
    if (L < 0) return -1;
  }
  const bool us = !pd->unsharp.w.empty() && pl.unsharpAmount > 0 && pl.unsharpSigma > 0 && pd->unsharp.r > 0;
  const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
  if (us) {  // unsharp: horizontal blur, then (vertical blur + unsharp + encode) in one pass
    const Taps& t = pd->unsharp;
    if (t.r > kMaxTaps) return r.err = "film GPU: blur kernel too wide", -1;
    const hal::PipelineH ph = r.pl("film_f4h", {}), pe = r.pl("film_f4ve", {});
    if (!ph || !pe) return -1;
    PostU eu = pu;
    eu.dim[2] = t.r;
    for (int i = 0; i <= t.r; ++i) eu.taps[i] = t.w[std::size_t(i)];
    eu.glare[3] = float(pl.unsharpAmount);
    const H tmp = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
    r.g.dispatch("film_f4h", ph, {GpuGraph::R(L), GpuGraph::W(tmp)}, gxw, gyh, &eu, sizeof eu);
    r.g.dispatch("film_f4ve", pe, {GpuGraph::R(tmp), GpuGraph::R(L), GpuGraph::W(out)}, gxw, gyh, &eu, sizeof eu);
  } else {
    const hal::PipelineH pe = r.pl("film_enc", {});
    if (!pe) return -1;
    PostU eu = pu;
    r.g.dispatch("film_enc", pe, {GpuGraph::R(L), GpuGraph::R(L), GpuGraph::W(out)}, gxw, gyh, &eu, sizeof eu);
  }
  return mix.ok && mix3.ok ? out : -1;
}

bool gpuMixtureForTest(gpu::Core& core, const float* plane, int w, int h, const void* mixture, bool half, std::vector<float>& out,
                       std::string& err) {
  // the plane goes through all three channels (scaled 1, 2, 0.5): channel 0 is returned, channels 1 and 2 must be 2x and 0.5x of it
  hal::Device& dev = core.dev();
  const int iw = half ? (w + 1) / 2 : w, ih = half ? (h + 1) / 2 : h;
  hal::ImageH in;
  if (dev.createImage({hal::Format::RGBA32F, std::uint32_t(iw), std::uint32_t(ih), 1}, &in) != hal::Status::Ok) return err = dev.lastError(), false;
  {
    auto cl = dev.begin();
    if (!cl || !core.ensureStaging(cl->slot(), std::size_t(iw) * ih * 16, err)) return false;
    auto* st = static_cast<float*>(core.slot(cl->slot()).staging.mapped);
    for (std::size_t i = 0; i < std::size_t(iw) * ih; ++i) {
      st[4 * i] = plane[i];
      st[4 * i + 1] = 2.f * plane[i];
      st[4 * i + 2] = 0.5f * plane[i];
      st[4 * i + 3] = 1.f;
    }
    GpuGraph g(dev, core.pool());
    g.copyBufferToImage(core.slot(cl->slot()).staging, g.external(in), std::uint32_t(iw), std::uint32_t(ih), 1, 0);
    if (gpu::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok || dev.waitAll() != hal::Status::Ok) return false;
  }
  auto cl = dev.begin();
  if (!cl || !core.ensureReadback(cl->slot(), std::size_t(w) * h * 16, err)) return false;
  GpuGraph g(dev, core.pool());
  gpu::GpuRecorder rec(dev, g, err);
  rec.taps = core.slot(cl->slot()).taps;
  Mix3 mix{rec};
  const fd::Mixture m3[3] = {*static_cast<const fd::Mixture*>(mixture), *static_cast<const fd::Mixture*>(mixture), *static_cast<const fd::Mixture*>(mixture)};
  const H o = mix.mixture(g.external(in), w, h, m3, half);
  if (!mix.ok || o < 0) return false;
  g.copyImageToBuffer(o, core.slot(cl->slot()).readback, 0, 0, std::uint32_t(w), std::uint32_t(h));
  hal::FenceH f;
  if (gpu::runGraph(dev, g, *cl, &f, err) != hal::Status::Ok || dev.wait(f, 30ull * 1000 * 1000 * 1000) != hal::Status::Ok) return err = dev.lastError(), false;
  dev.invalidate(core.slot(cl->slot()).readback, 0, std::size_t(w) * h * 8);
  const std::uint16_t* rb = static_cast<const std::uint16_t*>(core.slot(cl->slot()).readback.mapped);  // RGBA16F
  out.resize(std::size_t(w) * h);
  double worst = 0;
  for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
    const float a = halfToFloat(rb[4 * i]), b = halfToFloat(rb[4 * i + 1]), c = halfToFloat(rb[4 * i + 2]);
    out[i] = a;
    worst = std::max(worst, (std::fabs(double(b) - 2.0 * a) + std::fabs(double(c) - 0.5 * a)) / std::max(1.0, std::fabs(double(a))));
  }
  dev.waitAll();
  dev.destroy(in);
  if (worst > 1e-3) return err = "channels of the mixture disagree", false;
  return true;
}

}  // namespace dapple::engine::filmnode
