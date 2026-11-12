// Film nodes as GPU passes. See film_gpu.h.
#include "engine/nodes/film/film_gpu.h"

#include <algorithm>
#include <atomic>
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
  std::int32_t org[4];  // origin of the window in the frame (frame sized source / inhibitor field), 0 for a whole frame
};

struct GrainU {
  std::int32_t dim[4], org[4];
  std::uint32_t stream[4];
  float pa[3][4], pb[3][4], pc[3][4], pd[3][4];
};
// film_grain.comp: stream[3] = (layer + 1) | mask << 4 (layer -1: the sum of the layers; mask: channels whose layers are blurred separately),
// pd[0][3] / pd[1][3] = int bit patterns frame w | h << 16 and origin x | y << 16 of the density image in the frame.
void packGrainWindow(GrainU& u, int layer, unsigned mask, int fw, int fh, int x0, int y0) {
  u.stream[3] = std::uint32_t(layer + 1) | (mask << 4);
  const std::int32_t f = fw | (fh << 16), o = x0 | (y0 << 16);
  std::memcpy(&u.pd[0][3], &f, 4);
  std::memcpy(&u.pd[1][3], &o, 4);
}
constexpr int kDyeTaps = 255;  // film_dye.comp: half taps per channel (the tap table is a buffer)
struct DyeU {                  // film_dye.comp
  std::int32_t dim[4], rad[4], tr[4], tp[4];
  float ln[4];
  std::uint32_t stream[4];
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
// film_v3 reads its taps straight through the texture path (no shared-memory column tile) on unified-memory GPUs (phone SoCs: Adreno
// 840 runs it 2.2x faster, find-n5.md); the staged version stays for discrete GPUs (a few percent faster there).
// DAPPLE_FILM_V3_DIRECT=0 / 1 forces either.
bool v3Direct(const hal::Device& d) {
  static const int forced = [] { const char* e = std::getenv("DAPPLE_FILM_V3_DIRECT"); return e && (e[0] == '0' || e[0] == '1') ? e[0] - '0' : -1; }();
  return forced >= 0 ? forced == 1 : d.caps().unifiedMemory;
}
struct Mix3 {
  gpu::GpuRecorder& r;
  bool ok = true;
  // interactive tier (quality 1): every component of sigma >= 3 goes to pyramid level >= 1 (kMinLowSigma 1.5 instead of 2.0): the wide halation tail then
  // costs a quarter of the pixels (wave 8, Adreno 710: film_h3 + film_v3 were 60 % of the frame); DAPPLE_FILM_FASTLOW=0 turns it off
  bool fastLow = false;
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
      if (std::getenv("DAPPLE_FILM_MIXDBG")) {
        std::fprintf(stderr, "mix3 %dx%d chunk %zu nc %d R %d hasUp %d comps r:", w, h, j0 / 4, nc, R, hasUp);
        for (int c = 0; c < 3; ++c) {
          std::fprintf(stderr, " [");
          for (std::size_t j = 0; j < comps[c].size(); ++j) std::fprintf(stderr, "%d(%.3f) ", comps[c][j].r, comps[c][j].weight);
          std::fprintf(stderr, "]");
        }
        std::fprintf(stderr, "\n");
      }
      // per-slot radius for the direct vertical pass (tap table entry 0, w; the taps use rgb): a slot of radius 1 next to a slot of radius 16 loops 1, not 16
      for (int jj = 0; jj < nc; ++jj) {
        int rj = 0;
        for (int c = 0; c < 3; ++c)
          if (j0 + jj < comps[c].size()) rj = std::max(rj, comps[c][j0 + jj].r);
        vt[(jj * 20) * 4 + 3] = float(rj);
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
      pass("film_v3", {{0, nc}, {1, first ? 1 : 0}, {2, first && hasUp ? 1 : 0}, {3, v3Direct(r.dev) ? 1 : 0}}, vu,
           {GpuGraph::R(t[0]), GpuGraph::R(nc > 1 ? t[1] : d), GpuGraph::R(nc > 2 ? t[2] : d), GpuGraph::R(nc > 3 ? t[3] : d), GpuGraph::R(A),
            GpuGraph::R(first && hasUp ? up : A), GpuGraph::W(dst), GpuGraph::R(tapsH)},
           w, h);
      acc = dst;
    }
    return acc;
  }
  // `src`: full resolution (W x Hh) RGBA32F input, or the half-resolution input when `half`.
  H mixture(H src, int W, int Hh, const fd::Mixture* mix, bool half) {
    const double kMinLowSigma = half ? 1.6 : (fastLow ? 1.0 : 2.0), lowCut = fastLow ? 2.0 : 3.0;
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
        if (g.sigma >= lowCut) {
          k = int(std::floor(std::log2(g.sigma / kMinLowSigma)));
          k = std::clamp(k, half ? 1 : 0, avail);
          if (k > 0) {
            const double f = double(1 << std::min(k, 30));
            sl = std::sqrt(std::max(g.sigma * g.sigma - (f * f - 1.0) / 12.0, 0.25)) / f;
          }
        }
        std::vector<float> taps;
        int rr;
        if (g.sigma < lowCut) {
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

// Highlight boost on the exposure planes E (RGBA16F): film_boost.comp. Uses the dehaze statistics buffer (g[8..12]) and the reduction
// partials of the slot; nothing reads them between these three passes.
struct BoostU {
  std::int32_t s[4];
  float c[4];
};
H recordBoost(gpu::GpuRecorder& r, H E, int w, int h, const fd::SpatialPlan& pl) {
  if (!r.gBuf || !r.partBuf) return r.err = "film GPU: no reduction buffers for the highlight boost", -1;
  const int groups = gpu::cdiv(w * h, r.rwg * 16);  // 16 samples per thread
  if (std::uint64_t(groups) * 4 > 256u * 1024u) return r.err = "film GPU: frame too large for the boost reduction", -1;
  const H gH = r.g.externalBuf(r.gBuf), partH = r.g.externalBuf(r.partBuf);
  BoostU u{};
  u.s[0] = w;
  u.s[1] = h;
  u.s[2] = groups;
  u.c[0] = float(pl.boostM);
  u.c[1] = float(pl.boostA);
  u.c[2] = float(pl.boostP);
  const hal::PipelineH p0 = r.pl("film_bmax", {}), p1 = r.pl("film_bcon", {}), p2 = r.pl("film_bapp", {});
  if (!p0 || !p1 || !p2) return -1;
  r.g.dispatch("film_bmax", p0, {GpuGraph::R(E), GpuGraph::RW(partH)}, std::uint32_t(groups), 1, &u, sizeof u);
  r.g.dispatch("film_bcon", p1, {GpuGraph::RW(gH), GpuGraph::RW(partH)}, 1, 1, &u, sizeof u);
  const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
  r.g.dispatch("film_bapp", p2, {GpuGraph::R(E), GpuGraph::R(gH), GpuGraph::W(out)}, r.gx(w), r.gy(h), &u, sizeof u);
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
  const bool split = pr.split && !pr.lut1.empty() && !pr.lut3.empty() && !pr.paperCurve.empty() && pr.paperN > 1;
  const std::uint64_t l1Bytes = split ? pr.lut1.size() * 4 : 0, l3Bytes = split ? pr.lut3.size() * 4 : 0,
                      pcBytes = split ? pr.paperCurve.size() * 4 : 0;
  const std::uint32_t rp = std::uint32_t(raw.rawN + 2);
  if (dev.createImage({hal::Format::RGBA32F, rp, rp, 1}, &out.raw) != hal::Status::Ok ||
      dev.createImage({hal::Format::RGBA32F, std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN)}, &out.lut) != hal::Status::Ok ||
      dev.createBuffer({curveBytes, hal::MemKind::DeviceLocal, true}, &out.curves) != hal::Status::Ok ||
      dev.createBuffer({decBytes, hal::MemKind::DeviceLocal, true}, &out.decode) != hal::Status::Ok)
    return err = dev.lastError(), false;
  if (split) {
    if (dev.createImage({hal::Format::RGBA32F, std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN)}, &out.lut1) != hal::Status::Ok ||
        dev.createImage({hal::Format::RGBA32F, std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN)}, &out.lut3) != hal::Status::Ok ||
        dev.createBuffer({pcBytes, hal::MemKind::DeviceLocal, true}, &out.paper) != hal::Status::Ok)
      return err = dev.lastError(), false;
    out.split = true;
    out.paperN = pr.paperN;
    out.paperX0 = pr.paperX0;
    out.paperInvDx = pr.paperInvDx;
    std::copy(pr.dminS, pr.dminS + 4, out.dminS);
    std::copy(pr.dscaleS, pr.dscaleS + 4, out.dscaleS);
  }
  auto cl = dev.begin();
  if (!cl) return err = dev.lastError(), false;
  if (!core.ensureStaging(cl->slot(), rawBytes + lutBytes + curveBytes + decBytes + l1Bytes + l3Bytes + pcBytes + 1024, err)) return false;
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
  if (split) {
    // bufferOffset of a buffer-to-image copy must be a multiple of the texel size (16 B for RGBA32F). The sRGB decode table is 16388 B,
    // so the unaligned offsets shifted lut1 / lut3 by one float on Adreno Vulkan (red read as 0 in the enlarger diffusion path).
    const auto al = [](std::uint64_t v) { return (v + 255) & ~std::uint64_t(255); };
    const std::uint64_t o1 = al(rawBytes + lutBytes + curveBytes + decBytes), o3 = o1 + al(l1Bytes), op = o3 + al(l3Bytes);
    std::memcpy(base + o1, pr.lut1.data(), l1Bytes);
    std::memcpy(base + o3, pr.lut3.data(), l3Bytes);
    std::memcpy(base + op, pr.paperCurve.data(), pcBytes);
    g.copyBufferToImage(s.staging, g.external(out.lut1), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), o1);
    g.copyBufferToImage(s.staging, g.external(out.lut3), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), o3);
    g.copyBufferToBuffer(s.staging, g.externalBuf(out.paper), pcBytes, op);
  }
  if (gpu::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok) return false;
  if (dev.waitAll() != hal::Status::Ok) return err = dev.lastError(), false;
  return true;
}

// Sub-data update of the print LUT of already uploaded tables (wave 8): only the print table changed (a table-moving drag lands a new coarse table), the
// raw / curve / decode resources and the LUT image stay, only its texels are replaced. Not for the split (enlarger diffusion) path or a different size.
bool updateGpuPrintLut(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err) {
  const fd::FilmTables& pr = tb.print->t;
  if (out.split || pr.split || !out.lut.impl) return err = "print LUT update not applicable", false;
  hal::Device& dev = core.dev();
  if (out.lutN != pr.lutN) {  // the exact 33^3 table of the settled frame <-> the coarse 17^3 of a drag: only the LUT image is re-created
    dev.waitIdle();
    dev.destroy(out.lut);
    out.lut = {};
    if (dev.createImage({hal::Format::RGBA32F, std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN)}, &out.lut) != hal::Status::Ok) return err = dev.lastError(), false;
    out.lutN = pr.lutN;
  }
  std::copy(pr.dmin, pr.dmin + 4, out.dmin);
  std::copy(pr.dscale, pr.dscale + 4, out.dscale);
  std::copy(pr.glareRgb, pr.glareRgb + 4, out.glareRgb);
  const std::uint64_t lutBytes = pr.lut.size() * 4;
  auto cl = dev.begin();
  if (!cl) return err = dev.lastError(), false;
  if (!core.ensureStaging(cl->slot(), lutBytes + 256, err)) return false;
  gpu::SlotRes& s = core.slot(cl->slot());
  std::memcpy(s.staging.mapped, pr.lut.data(), lutBytes);
  GpuGraph g(dev, core.pool());
  g.copyBufferToImage(s.staging, g.external(out.lut), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), std::uint32_t(pr.lutN), 0);
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
  dev.destroy(t.lut1);
  dev.destroy(t.lut3);
  dev.destroy(t.paper);
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
  if (!pd->error.empty()) return why = "film GPU: unsupported effect", false;
  // every blur of the grain / scan chain runs from the tap table of its pass
  int rmax = std::max({pd->scan.r, pd->unsharp.r, pd->grain.r, pd->glare.r});
  int dmax = pd->micro.r;
  for (int ch = 0; ch < 3; ++ch)
    for (int l = 0; l < 3; ++l) dmax = std::max(dmax, pd->dye[ch][l].r);
  if (rmax > kMaxTaps || dmax > kDyeTaps) return why = "film GPU: blur kernel too wide", false;
  return true;
}

// The enlarger diffusion filter blurs the whole frame between the grain and the print step: no window decomposition.
bool filmWindowable(const FilmResolved& res) { return !planOf(res)->split; }

int filmWindowHalo(const FilmResolved& res) {
  const auto pd = planOf(res);
  if (!pd->error.empty() || !pd->plan.any()) return 0;
  int h = 0;
  if (pd->scanStage) h += pd->Hs;
  if (pd->plan.grain.active) h += pd->Hg + pd->Hd;
  return h;
}

namespace {

// Everything one film pass needs from the resolved film and its device tables.
struct FilmCtx {
  gpu::GpuRecorder& r;
  const GpuFilmTables& tb;
  std::shared_ptr<const FilmPlanData> pd;
  PointU u0;
  H rawH, lutH, curH;
  // the enlarger diffusion path's second LUT pair: paper exposure LUT (MODE 6) and paper density -> display LUT with the paper curves (MODE 7)
  H lut1H = -1, lut3H = -1, paperH = -1;
  FilmCtx(gpu::GpuRecorder& rec, const FilmResolved& res, const GpuFilmTables& t)
      : r(rec), tb(t), pd(planOf(res)), u0(pointUniforms(res, t, 0, 0)), rawH(rec.g.external(t.raw)), lutH(rec.g.external(t.lut)),
        curH(rec.g.externalBuf(t.curves)) {
    if (t.split) {
      lut1H = rec.g.external(t.lut1);
      lut3H = rec.g.external(t.lut3);
      paperH = rec.g.externalBuf(t.paper);
    }
  }
  // uniforms of MODE 7: the paper curve takes the place of the film density curve (pxCurves), the paper density grid that of the print grid
  PointU paperU(PointU u) const {
    for (int i = 0; i < 4; ++i) {
      u.dmin[i] = tb.dminS[i];
      u.dscale[i] = tb.dscaleS[i];
    }
    u.gam[0] = u.gam[1] = u.gam[2] = 1.f;
    u.gam[3] = tb.paperX0;
    u.dim[3] = tb.paperN;
    u.misc[0] = tb.paperInvDx;
    return u;
  }
  // one film_point pass producing an ow x oh RGBA16F image; `org` = origin of the window in the (frame sized) inputs
  H point(int mode, bool hasQ, H in, H aux, int ow, int oh, int offset = 0, const float* dsub = nullptr, int srcW = 0, int srcH = 0, int orgX = 0,
          int orgY = 0) {
    const hal::PipelineH p = r.pl("film_point", {{0, mode}, {1, hasQ ? 1 : 0}});
    if (!p) return -1;
    PointU u = mode == 7 ? paperU(u0) : u0;
    u.dim[0] = ow;
    u.dim[1] = oh;
    u.lutN[1] = u.lutN[2] = offset;
    u.src[0] = srcW;
    u.src[1] = srcH;
    u.org[0] = orgX;
    u.org[1] = orgY;
    if (dsub)
      for (int c = 0; c < 3; ++c) u.dsub[c] = dsub[c];
    const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(ow), std::uint32_t(oh));
    const H lutUse = mode == 6 ? lut1H : mode == 7 ? lut3H : lutH, curUse = mode == 7 ? paperH : curH;
    r.g.dispatch("film_point", p, {GpuGraph::R(in), GpuGraph::R(aux), GpuGraph::R(lutUse), GpuGraph::R(curUse), GpuGraph::W(out)}, r.gx(ow), r.gy(oh), &u,
                 sizeof u);
    return out;
  }
};

// One pass of film_dye.comp MODE 0: per-channel FIR along an axis with the blur margin trimmed (no boundary handling). The taps of the
// three channels go through the tap table of the slot (like the mixtures); false when it is full.
bool dyePass(gpu::GpuRecorder& r, H in, int ow, int oh, bool horizontal, int trim, const Taps* taps[3], int op, H acc, H& out) {
  const hal::PipelineH p = r.pl("film_dfir", {});
  if (!p) return false;
  int rmax = 0;
  for (int c = 0; c < 3; ++c) rmax = std::max(rmax, taps[c]->r);
  const int stride = rmax / 4 + 1;
  if (!r.taps.mapped || std::uint64_t(r.tapNext + 3 * stride) * 16 > r.taps.bytes) return r.err = "film GPU: tap table buffer full", false;
  DyeU u{};
  u.dim[0] = ow;
  u.dim[1] = oh;
  u.dim[2] = op;
  u.tr[0] = horizontal ? trim : 0;
  u.tr[1] = horizontal ? 0 : trim;
  u.tr[2] = horizontal ? 1 : 0;
  u.tr[3] = horizontal ? 0 : 1;
  u.tp[0] = std::int32_t(r.tapNext);
  u.tp[1] = stride;
  float* tab = static_cast<float*>(r.taps.mapped) + std::size_t(r.tapNext) * 4;
  std::memset(tab, 0, std::size_t(3 * stride) * 16);
  r.tapNext += std::uint32_t(3 * stride);
  for (int c = 0; c < 3; ++c) {
    const Taps& t = *taps[c];
    u.rad[c] = t.r;
    for (int i = 0; i <= t.r; ++i) tab[std::size_t(c) * stride * 4 + i] = t.w[std::size_t(i)];
  }
  out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(ow), std::uint32_t(oh));
  r.g.dispatch("film_dfir", p,
               {GpuGraph::R(in), GpuGraph::R(acc >= 0 ? acc : r.dummy(hal::Format::RGBA16F)), GpuGraph::W(out), GpuGraph::R(r.g.externalBuf(r.taps))},
               r.gx(ow), r.gy(oh), &u, sizeof u);
  return true;
}

}  // namespace

namespace {
// Interactive tier, GPU only (the CPU nodes stay bit-identical to the FilmSimulator oracle; the difference is far inside the parity budget): (camera diffusion / lens blur, scatter +
// halation) carry a handful of Gaussians with a combined weight of 1-2 % and nearby widths (sigma 1.5-4 px at preview scale). Each one costs a
// horizontal and a vertical pass, and a group of five or more components costs a second chunk of passes. Variance-matched merging of
// neighbours in sigma (total group weight <= 2 %, widest / narrowest <= 3) is invisible at preview scale (measured, pipeline-architecture.md 24)
// and saves 2-4 full-frame passes. The final tier is never touched. DAPPLE_FILM_MERGE=0 turns it off.
std::atomic<int> g_mergeOverride{-1};
bool mergeEnabled() {
  const int o = g_mergeOverride.load();
  if (o >= 0) return o != 0;
  static const bool on = [] { const char* e = std::getenv("DAPPLE_FILM_MERGE"); return !(e && e[0] == '0'); }();
  return on;
}
// Combined-weight limit of a merged group of small components (DAPPLE_FILM_MERGE_W overrides, for the exactness sweep in dapple_film_gpu_tests merge)
double mergeWeightLimit() {
  static const double w = [] { const char* e = std::getenv("DAPPLE_FILM_MERGE_W"); return e && e[0] ? std::atof(e) : 0.02; }();
  return w;
}
void mergeSmallComps(fd::Mixture& m) {
  const double wLimit = mergeWeightLimit();
  if (m.comps.size() < 3) return;
  std::vector<fd::GaussComp> v = m.comps;
  std::sort(v.begin(), v.end(), [](const fd::GaussComp& a, const fd::GaussComp& b) { return a.sigma < b.sigma; });
  std::vector<fd::GaussComp> out;
  std::size_t i = 0;
  while (i < v.size()) {
    double wsum = v[i].w, vsum = v[i].w * v[i].sigma * v[i].sigma;
    std::size_t j = i + 1;
    if (v[i].w <= wLimit)
      while (j < v.size() && wsum + v[j].w <= wLimit && v[j].sigma <= 3.0 * v[i].sigma) {
        wsum += v[j].w;
        vsum += v[j].w * v[j].sigma * v[j].sigma;
        ++j;
      }
    if (j == i + 1 || wsum <= 0) {
      for (std::size_t k = i; k < j; ++k) out.push_back(v[k]);
    } else {
      out.push_back({std::sqrt(vsum / wsum), wsum});
    }
    i = j;
  }
  m.comps = std::move(out);
}
}  // namespace

void setFilmMergeForTest(int mode) { g_mergeOverride = mode; }

bool recordFilmPre(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, H src, int w, int h, FilmPre& out) {
  out = FilmPre{};
  const auto pd = planOf(res);
  if (!pd->error.empty()) {
    r.err = "film GPU: unsupported effect";
    return false;
  }
  if (pd->split && !tb.split) {
    r.err = "film GPU: the enlarger diffusion tables are not uploaded";
    return false;
  }
  const fd::SpatialPlan& pl = pd->plan;
  FilmCtx c(r, res, tb);
  if (!pl.any()) {  // the fused point chain
    out.disp = c.point(3, false, src, c.rawH, w, h);
    return out.disp >= 0;
  }
  out.any = true;
  H E = c.point(0, false, src, c.rawH, w, h);
  if (E < 0) return false;
  if (pl.boost) {  // highlight boost: the maximum of the frame (reduction), the constants, then the curve; all on the device
    E = recordBoost(r, E, w, h, pl);
    if (E < 0) return false;
  }
  Mix3 mix3{r};  // three channels at once
  { static const bool fl = [] { const char* e = std::getenv("DAPPLE_FILM_FASTLOW"); return !(e && e[0] == '0'); }(); mix3.fastLow = fl && res.quality == 1; }
  H E2 = E;
  fd::Mixture preM[3] = {pl.pre[0], pl.pre[1], pl.pre[2]}, haloM[3] = {pl.halo[0], pl.halo[1], pl.halo[2]};
  if (res.quality == 1 && mergeEnabled())
    for (int c = 0; c < 3; ++c) {
      if (pl.preMix) mergeSmallComps(preM[c]);
      if (pl.haloMix) mergeSmallComps(haloM[c]);
    }
  if (pl.preMix) E2 = mix3.mixture(E2, w, h, preM, false);
  if (pl.haloMix) E2 = mix3.mixture(E2, w, h, haloM, false);
  H Q = -1;
  if (pl.dirDiffuse) {
    if (pd->dirHalf) {
      const int hw = (w + 1) / 2, hh = (h + 1) / 2;
      const H qh = c.point(4, false, E2, c.rawH, hw, hh, 0, nullptr, w, h);
      if (qh < 0) return false;
      Q = mix3.mixture(qh, w, h, pl.dir, true);
    } else {
      const H q = c.point(5, false, E2, c.rawH, w, h);
      if (q < 0) return false;
      Q = mix3.mixture(q, w, h, pl.dir, false);
    }
  }
  if (!mix3.ok || E2 < 0) return false;
  out.e2 = E2;
  out.q = Q;
  return true;
}

bool filmEndsClamped(const FilmResolved& res) {
  if (!res.clampOut) return false;
  const auto pd = planOf(res);
  if (!pd->error.empty() || !pd->plan.any()) return false;
  const fd::SpatialPlan& pl = pd->plan;
  const bool fuseDev = !pl.grain.active && !pd->split;
  const bool us = !pd->unsharp.w.empty() && pl.unsharpAmount > 0 && pl.unsharpSigma > 0 && pd->unsharp.r > 0;
  return fuseDev && pd->scanStage && pd->scan.r <= 0 && !us;  // = encodeHere of recordFilmWindow
}

FilmImg recordFilmWindow(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pres, const FilmPre& pre, int fw, int fh,
                         const FilmWin& win) {
  const FilmImg none{};
  if (!pre.any) return pre.disp >= 0 ? FilmImg{pre.disp, 0, 0} : none;
  const auto pd = planOf(res);
  const fd::SpatialPlan& pl = pd->plan;
  const PointU u0 = pointUniforms(res, tb, win.w, win.h);
  FilmCtx c(r, res, tb);
  Mix mix{r};  // single plane mixtures (glare field)
  const int w = win.w, h = win.h, x0 = win.x0, y0 = win.y0;
  const bool hasQ = pre.q >= 0;
  // Without grain (and without the enlarger diffusion detour) nothing sits between the develop and the print step: they run as one pass
  // (film_point / film_pd MODE 8), which saves a full-frame RGBA16F write and read. The grain drag and the final tier keep the two passes.
  const bool fuseDev = !pd->plan.grain.active && !pd->split;
  H D = -1;
  if (!fuseDev) {
    D = c.point(1, hasQ, pre.e2, hasQ ? pre.q : c.rawH, w, h, 0, nullptr, 0, 0, x0, y0);
    if (D < 0) return none;
  }

  // ---- grain: sampled on the window grown by the blur margins (hash on absolute coordinates), blurred, minimum density removed -----------
  int off = 0;
  float dsub[4] = {0, 0, 0, 0};
  if (pl.grain.active) {
    const fd::GrainPlan& gp = pl.grain;
    if (!pres.grainTab) {
      r.err = "film GPU: grain tables not available";
      return none;
    }
    const int Hg = pd->grain.r, layers = gp.ch[0].layers;
    int dyeR = 0;
    unsigned mask = 0;  // channels whose sub-layers are blurred separately (a dye-cloud blur of any layer)
    for (int ch = 0; ch < 3; ++ch)
      for (int l = 0; l < gp.ch[ch].layers; ++l)
        if (pd->dye[ch][l].r > 0) {
          mask |= 1u << ch;
          dyeR = std::max(dyeR, pd->dye[ch][l].r);
        }
    if (std::max(dyeR, pd->micro.r) > kDyeTaps || Hg > kMaxTaps) {
      r.err = "film GPU: blur kernel too wide";
      return none;
    }
    GrainU gb{};
    gb.dim[2] = w;
    gb.dim[3] = h;
    gb.org[2] = layers;
    gb.org[3] = pres.grainTableN;
    for (int ch = 0; ch < 3; ++ch) {
      const fd::GrainChannelPlan& gc = gp.ch[ch];
      gb.stream[ch] = pd->streamGrain[ch];
      for (int l = 0; l < 3; ++l) {
        gb.pa[ch][l] = gc.dminL[l];
        gb.pb[ch][l] = gc.invDmaxL[l];
        gb.pc[ch][l] = gc.nL[l];
        gb.pd[ch][l] = gc.scaleL[l];
      }
      gb.pa[ch][3] = gc.invScale;
      gb.pb[ch][3] = gc.u;
      gb.pc[ch][3] = gc.lambdaSwitch;
      dsub[ch] = gc.dminCh;
    }
    const hal::PipelineH pg = r.pl("film_grain", {});
    if (!pg) return none;
    const H tabH = r.g.externalBuf(pres.grainTab);
    auto sample = [&](int ring, int layer) -> H {  // the grain of one sub-layer (or of their sum) over the window grown by `ring`
      GrainU gu = gb;
      const int gw = w + 2 * ring, gh = h + 2 * ring;
      gu.dim[0] = gw;
      gu.dim[1] = gh;
      gu.org[0] = x0 - ring;
      gu.org[1] = y0 - ring;
      packGrainWindow(gu, layer, mask, fw, fh, x0, y0);
      const H G = r.g.transient(hal::Format::RGBA16F, std::uint32_t(gw), std::uint32_t(gh));
      r.g.dispatch("film_grain", pg, {GpuGraph::R(D), GpuGraph::R(tabH), GpuGraph::W(G)}, r.gx(gw), r.gy(gh), &gu, sizeof gu);
      return G;
    };
    const int gw0 = w + 2 * Hg, gh0 = h + 2 * Hg;  // the extent the grain blur needs
    H S;                                           // grain of the extent, before the grain blur
    if (mask == 0) {
      S = sample(Hg, -1);
    } else {
      // dye-cloud blur: each sub-layer is blurred on its own (per channel and layer kernel), then they are summed
      Taps unit;
      unit.set(0);
      const int ringL = Hg + dyeR;
      S = -1;
      for (int l = 0; l < layers; ++l) {
        const H Gl = sample(ringL, l);
        const Taps* tp[3];
        for (int ch = 0; ch < 3; ++ch) tp[ch] = ((mask >> ch) & 1u) && l < gp.ch[ch].layers ? &pd->dye[ch][l] : &unit;
        H T;
        if (!dyePass(r, Gl, gw0, h + 2 * ringL, true, dyeR, tp, 0, -1, T) || !dyePass(r, T, gw0, gh0, false, dyeR, tp, l == 0 ? 0 : 1, S, S)) return none;
      }
    }
    if (gp.microActive) {
      // multiplicative clumping: a log-normal hash field (blurred) multiplies the density of the grain
      const float sg2 = float(std::log(1.0 + gp.microStd * gp.microStd));
      const int Hm = pd->micro.r, ringM = Hg + Hm;
      const hal::PipelineH pm = r.pl("film_mnoise", {});
      if (!pm) return none;
      DyeU nu{};
      const int nw = w + 2 * ringM, nh = h + 2 * ringM;
      nu.dim[0] = nw;
      nu.dim[1] = nh;
      nu.rad[0] = x0 - ringM;
      nu.rad[1] = y0 - ringM;
      nu.ln[0] = -0.5f * sg2;
      nu.ln[1] = std::sqrt(sg2);
      for (int ch = 0; ch < 3; ++ch) nu.stream[ch] = pd->streamMicro[ch];
      const H N = r.g.transient(hal::Format::RGBA16F, std::uint32_t(nw), std::uint32_t(nh));
      r.g.dispatch("film_mnoise", pm, {GpuGraph::W(N)}, r.gx(nw), r.gy(nh), &nu, sizeof nu);
      const Taps* tp[3] = {&pd->micro, &pd->micro, &pd->micro};
      H T;
      if (!dyePass(r, N, gw0, nh, true, Hm, tp, 0, -1, T) || !dyePass(r, T, gw0, gh0, false, Hm, tp, 2, S, S)) return none;
    }
    H G = S;
    if (Hg > 0) {
      G = fir4(r, G, gw0, gh0, pd->grain);
      if (G < 0) return none;
    }
    D = G;
    off = Hg;
  }
  // ---- enlarger diffusion filter: density -> linear paper exposure (FilmPrintExposure), filter mixtures (FilmEnlargerBlur); the print step
  // below then reads the filtered exposure (MODE 7) instead of the density (MODE 2). Whole frames only: the mixtures have no window form ----
  const bool split = pd->split;
  if (split) {
    if (!tb.split || w != fw || h != fh) {
      r.err = "film GPU: the enlarger diffusion filter runs on whole frames";
      return none;
    }
    Mix3 mix3{r};
    const H Ep = c.point(6, false, D, c.rawH, w, h, off, dsub);
    if (Ep < 0) return none;
    D = mix3.mixture(Ep, w, h, pl.enl, false);
    if (!mix3.ok || D < 0) return none;
    off = 0;
  }
  if (fuseDev && !pd->scanStage) {
    const H o = c.point(8, hasQ, pre.e2, hasQ ? pre.q : c.rawH, w, h, 0, nullptr, 0, 0, x0, y0);
    return o >= 0 ? FilmImg{o, x0, y0} : none;
  }
  if (!pd->scanStage) {
    const H o = split ? c.point(7, false, D, c.rawH, w, h) : c.point(2, false, D, c.rawH, w, h, off, dsub);
    return o >= 0 ? FilmImg{o, x0, y0} : none;
  }

  // ---- FilmGlare + FilmScan --------------------------------------------------------------------------------------------------------
  const std::uint32_t gxw = r.gx(w), gyh = r.gy(h);
  PostU pu{};
  pu.dim[0] = w;
  pu.dim[1] = h;
  // the glare field first (it does not depend on the picture): blurred log-normal hash noise over the window grown by the blur radius
  H field = -1;
  int rgl = 0;
  if (pl.glare) {
    rgl = pd->rgl;
    if (rgl > kMaxTaps) {
      r.err = "film GPU: glare blur too wide";
      return none;
    }
    PostU gu = pu;
    const int fwd = w + 2 * rgl, fht = h + 2 * rgl;
    gu.dim[0] = fwd;
    gu.dim[1] = fht;
    gu.org[0] = x0 - rgl;
    gu.org[1] = y0 - rgl;
    gu.glare[0] = float(pl.glareMu);
    gu.glare[1] = float(pl.glareSigma);
    gu.stream[0] = pd->streamGlare;
    const hal::PipelineH p = r.pl("film_gnoise", {});
    if (!p) return none;
    field = r.g.transient(hal::Format::R32F, std::uint32_t(fwd), std::uint32_t(fht));
    r.g.dispatch("film_gnoise", p, {GpuGraph::W(field)}, r.gx(fwd), r.gy(fht), &gu, sizeof gu);
    if (rgl > 0) {
      std::vector<LevelComp> comps(1);
      comps[0].r = rgl;
      comps[0].w = pd->glare.w;
      comps[0].weight = 1.f;
      field = mix.levelCombine(field, fwd, fht, 0.0, comps, -1);
    }
  }
  const bool us = !pd->unsharp.w.empty() && pl.unsharpAmount > 0 && pl.unsharpSigma > 0 && pd->unsharp.r > 0;
  // develop fused in, and no scan blur / unsharp after it: the same pass also encodes (film_enc would only re-read and re-write it)
  const bool encodeHere = fuseDev && pd->scan.r <= 0 && !us;
  // FilmPrintScan + the sRGB decode + the glare add in one pass: linear light
  H L;
  {
    const hal::PipelineH p = r.pl("film_pd", {{0, fuseDev ? 8 : split ? 7 : 2}, {1, fuseDev && hasQ ? 1 : 0}, {2, encodeHere ? 1 : 0}, {3, encodeHere && res.clampOut ? 1 : 0}});
    if (!p) return none;
    PointU u = split ? c.paperU(u0) : u0;
    u.dim[0] = w;
    u.dim[1] = h;
    u.lutN[1] = u.lutN[2] = off;
    u.org[0] = fuseDev ? x0 : 0;
    u.org[1] = fuseDev ? y0 : 0;
    for (int ch = 0; ch < 3; ++ch) u.dsub[ch] = dsub[ch];
    u.dmin[3] = tb.glareRgb[0];  // linear display white (film_point.comp FUSED_POST)
    u.dscale[3] = tb.glareRgb[1];
    u.dsub[3] = tb.glareRgb[2];
    u.misc[3] = tb.glareRgb[3];  // glare mean
    u.src[0] = u.src[1] = rgl;
    u.src[2] = field >= 0 ? 1 : 0;
    L = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
    r.g.dispatch("film_pd", p,
                 {GpuGraph::R(fuseDev ? pre.e2 : D), GpuGraph::R(fuseDev && hasQ ? pre.q : c.rawH), GpuGraph::R(split ? c.lut3H : c.lutH), GpuGraph::R(split ? c.paperH : c.curH), GpuGraph::W(L),
                  GpuGraph::R(field >= 0 ? field : r.dummy(hal::Format::R32F)), GpuGraph::R(r.g.externalBuf(tb.decode))},
                 r.gx(w), r.gy(h), &u, sizeof u);
  }
  if (encodeHere) return FilmImg{L, x0, y0};
  if (pd->scan.r > 0) {
    L = fir4(r, L, w, h, pd->scan);
    if (L < 0) return none;
  }
  const H out = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
  if (us) {  // unsharp: horizontal blur, then (vertical blur + unsharp + encode) in one pass
    const Taps& t = pd->unsharp;
    if (t.r > kMaxTaps) return r.err = "film GPU: blur kernel too wide", none;
    const hal::PipelineH ph = r.pl("film_f4h", {}), pe = r.pl("film_f4ve", {});
    if (!ph || !pe) return none;
    PostU eu = pu;
    eu.dim[2] = t.r;
    for (int i = 0; i <= t.r; ++i) eu.taps[i] = t.w[std::size_t(i)];
    eu.glare[3] = float(pl.unsharpAmount);
    const H tmp = r.g.transient(hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h));
    r.g.dispatch("film_f4h", ph, {GpuGraph::R(L), GpuGraph::W(tmp)}, gxw, gyh, &eu, sizeof eu);
    r.g.dispatch("film_f4ve", pe, {GpuGraph::R(tmp), GpuGraph::R(L), GpuGraph::W(out)}, gxw, gyh, &eu, sizeof eu);
  } else {
    const hal::PipelineH pe = r.pl("film_enc", {});
    if (!pe) return none;
    PostU eu = pu;
    r.g.dispatch("film_enc", pe, {GpuGraph::R(L), GpuGraph::R(L), GpuGraph::W(out)}, gxw, gyh, &eu, sizeof eu);
  }
  return mix.ok ? FilmImg{out, x0, y0} : none;
}

H recordFilmFrame(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pres, H src, int w, int h) {
  FilmPre pre;
  if (!recordFilmPre(r, res, tb, src, w, h, pre)) return -1;
  return recordFilmWindow(r, res, tb, pres, pre, w, h, FilmWin{0, 0, w, h}).img;
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
