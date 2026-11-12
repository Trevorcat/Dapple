// Render driver of the spatial effects: planar exposure passes (boost, diffusion filter, lens blur, scatter, halation),
// the DIR inhibitor field, and one fused tile pass (density -> grain -> print/scan LUT -> glare -> scanner blur ->
// unsharp mask -> sRGB encode). See docs/film/architecture.md section 2.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "film_spatial.h"
#include "parallel.h"

#ifdef DAPPLE_SPATIAL_PROFILE  // per-section tile-pass timers (nanoseconds summed over threads), printed by renderSpatial
#include <atomic>
#include <cstdio>
static std::atomic<long long> gProf[8];
#define PROF_BEGIN auto prof_t = std::chrono::steady_clock::now();
#define PROF_LAP(i)                                                                                    \
  {                                                                                                    \
    const auto n_ = std::chrono::steady_clock::now();                                                  \
    gProf[i] += std::chrono::duration_cast<std::chrono::nanoseconds>(n_ - prof_t).count();             \
    prof_t = n_;                                                                                       \
  }
#else
#define PROF_BEGIN
#define PROF_LAP(i)
#endif

namespace dapple::film::detail {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

inline int reflectIdx(int i, int n) {
  if (n == 1) return 0;
  const int period = 2 * n;
  i %= period;
  if (i < 0) i += period;
  return i < n ? i : period - 1 - i;
}

struct Rect {
  int x0, y0, x1, y1;
};
inline Rect shrink(Rect r, int d) { return {r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d}; }

struct EncTable {
  float t[4099];
  EncTable() {
    for (int i = 0; i < 4099; ++i) {
      const double v = double(i) / 4096.0;
      t[i] = float(v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
    }
  }
};
const EncTable& encTable() {
  static const EncTable e;
  return e;
}
inline float encodeSrgb(const EncTable& e, float v) {
  if (v <= 0.f) return 12.92f * v;
  if (v >= 1.f) return 1.f + (v - 1.f) * 0.4396f;
  const float x = v * 4096.f;
  const int i = int(x);
  const float f = x - float(i);
  return e.t[i] + f * (e.t[i + 1] - e.t[i]);
}

std::uint32_t mix32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}
std::uint32_t streamOf(std::uint32_t seed, std::uint32_t id) { return mix32(seed * 0x9E3779B1u + id * 0x85EBCA6Bu + 0x165667B1u); }

// Interleave n pixels of three planes starting at (reflect-mapped) column gx0 of row gy.
inline void gatherInterleaved(const Plane pl[3], int gy, int gx0, int n, float* out) {
  const int W = pl[0].w;
  const float *r0 = pl[0].row(gy), *r1 = pl[1].row(gy), *r2 = pl[2].row(gy);
  auto one = [&](int i) {
    const int gx = reflectIdx(gx0 + i, W);
    out[3 * i] = r0[gx];
    out[3 * i + 1] = r1[gx];
    out[3 * i + 2] = r2[gx];
  };
  const int lo = std::clamp(-gx0, 0, n), hi = std::clamp(W - gx0, lo, n);
  for (int i = 0; i < lo; ++i) one(i);
  for (int i = lo; i < hi; ++i) {
    const int gx = gx0 + i;
    out[3 * i] = r0[gx];
    out[3 * i + 1] = r1[gx];
    out[3 * i + 2] = r2[gx];
  }
  for (int i = hi; i < n; ++i) one(i);
}

inline void deinterleave(const float* in, float* a, float* b, float* c, int n) {
  for (int i = 0; i < n; ++i) {
    a[i] = in[3 * i];
    b[i] = in[3 * i + 1];
    c[i] = in[3 * i + 2];
  }
}
inline void interleave(const float* a, const float* b, const float* c, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    out[3 * i] = a[i];
    out[3 * i + 1] = b[i];
    out[3 * i + 2] = c[i];
  }
}

struct Taps {
  std::vector<float> w;
  int r = 0;
  void set(double sigma) {
    if (sigma > 0) gaussTaps(sigma, w, r);
    else {
      w.assign(1, 1.f);
      r = 0;
    }
  }
};

// out rect of dst = separable blur of src; src must be valid on out grown by r. tmp has the same geometry.
void blurLocal(const SpatialKernels& K, const float* src, float* dst, float* tmp, std::ptrdiff_t ls, Rect o, const Taps& t) {
  const int n = o.x1 - o.x0;
  if (n <= 0 || o.y1 <= o.y0) return;
  for (int y = o.y0 - t.r; y < o.y1 + t.r; ++y) K.firRow(src + y * ls + o.x0, tmp + y * ls + o.x0, n, t.w.data(), t.r);
  for (int y = o.y0; y < o.y1; ++y) K.firCol(tmp + y * ls + o.x0, ls, dst + y * ls + o.x0, n, t.w.data(), t.r, 0);
}

struct FinalCfg {
  Taps scan, unsharp, grain, glare, micro;
  Taps dye[3][3];
  int Hs = 0, Hg = 0, Hd = 0, H = 0, rgl = 0;
  std::uint32_t streamGrain[3], streamMicro[3], streamGlare;
};

}  // namespace

void renderSpatial(const SpatialRenderArgs& a, const ImageF& in, ImageF& out, SpatialTimings* tm) {
  const SpatialPlan& pl = *a.plan;
  const SpatialKernels& K = *a.sk;
  const KernelSet& ks = *a.ks;
  const KView& view = *a.view;
  Arena& ar = *a.arena;
  const int W = in.width, H = in.height;
  ar.reset();
  const auto tStart = Clock::now();
  SpatialTimings st;

  Plane sets[4][3];
  bool has[4] = {false, false, false, false};
  auto get = [&](int i) -> Plane* {
    if (!has[i]) {
      for (int c = 0; c < 3; ++c) sets[i][c] = ar.plane(W, H);
      has[i] = true;
    }
    return sets[i];
  };

  // --- P1: exposure -> planar --------------------------------------------------------------------------------------
  auto t0 = Clock::now();
  Plane* cur = get(0);
  int curIdx = 0;
  std::vector<float> rowMax(pl.boost ? H : 0);
  parallelFor(
      H,
      [&](int y) {
        thread_local std::vector<float> row;
        if (int(row.size()) < 3 * W) row.resize(std::size_t(3) * W);
        ks.exposure(view, in.row(y), row.data(), W, a.scale);
        deinterleave(row.data(), cur[0].row(y), cur[1].row(y), cur[2].row(y), W);
        if (pl.boost) {
          float m = 0.f;
          for (int i = 0; i < 3 * W; ++i) m = std::max(m, row[i]);
          rowMax[y] = m;
        }
      },
      4);
  if (pl.boost) {
    double X = 0;
    for (float v : rowMax) X = std::max(X, double(v));
    double x0, aa, k;
    if (boostConstants(pl.boostM, pl.boostA, pl.boostP, X, x0, aa, k) && x0 < X) {
      const float fx0 = float(x0), fX = float(X), fa = float(aa), fk = float(k);
      parallelFor(
          H,
          [&](int y) {
            for (int c = 0; c < 3; ++c) {
              float* r = cur[c].row(y);
              K.boostRow(r, W, fx0, fX, fa, fk);
            }
          },
          4);
    }
  }
  st.exposureMs = msSince(t0);

  // --- camera diffusion + lens blur, scatter + halation (linear, per channel Gaussian mixtures) -----------------------
  auto runMix = [&](const Mixture m[3]) {
    const int oi = (curIdx + 1) % 3;
    Plane* other = get(oi);
    for (int c = 0; c < 3; ++c) applyMixture(K, ar, cur[c], other[c], m[c]);
    cur = other;
    curIdx = oi;
  };
  t0 = Clock::now();
  if (pl.preMix) runMix(pl.pre);
  st.preMs = msSince(t0);
  t0 = Clock::now();
  if (pl.haloMix) runMix(pl.halo);
  st.haloMs = msSince(t0);
  Plane* E2 = cur;

  // --- DIR inhibitor field and its diffusion ------------------------------------------------------------------------------
  t0 = Clock::now();
  Plane* Qp = nullptr;
  if (pl.dirDiffuse) {
    Qp = get((curIdx + 1) % 3);
    const bool half = mixtureFitsHalf(pl.dir[0]) && mixtureFitsHalf(pl.dir[1]) && mixtureFitsHalf(pl.dir[2]);
    if (half) {
      // the inhibitor field is smooth (sigma >= 3.2 px everywhere): build it box-averaged at half resolution directly
      const Arena::Mark am = ar.mark();
      const int hw = (W + 1) / 2, hh = (H + 1) / 2;
      Plane Qh[3] = {ar.plane(hw, hh), ar.plane(hw, hh), ar.plane(hw, hh)};
      parallelFor(
          hh,
          [&](int j) {
            thread_local std::vector<float> e, x, q;
            if (int(e.size()) < 6 * W) {
              e.resize(std::size_t(6) * W);
              x.resize(std::size_t(6) * W);
              q.resize(std::size_t(6) * W);
            }
            const int ya = std::min(2 * j, H - 1), yb = std::min(2 * j + 1, H - 1);
            gatherInterleaved(E2, ya, 0, W, e.data());
            gatherInterleaved(E2, yb, 0, W, e.data() + 3 * std::size_t(W));
            ks.logE(view, e.data(), x.data(), 2 * W);
            ks.develop1(view, x.data(), q.data(), 2 * W);
            const float* qa = q.data();
            const float* qb = q.data() + 3 * std::size_t(W);
            for (int c = 0; c < 3; ++c) {
              float* o = Qh[c].row(j);
              const int full = W / 2;
              for (int i = 0; i < full; ++i)
                o[i] = 0.25f * (qa[6 * i + c] + qa[6 * i + 3 + c] + qb[6 * i + c] + qb[6 * i + 3 + c]);
              if (hw > full) o[full] = 0.5f * (qa[6 * full + c] + qb[6 * full + c]);
            }
          },
          2);
      for (int c = 0; c < 3; ++c) applyMixtureHalf(K, ar, Qh[c], Qp[c], pl.dir[c]);
      ar.release(am);
    } else {
    Plane* Q = get((curIdx + 2) % 3);
    parallelFor(
        H,
        [&](int y) {
          thread_local std::vector<float> e, x, q;
          if (int(e.size()) < 3 * W) {
            e.resize(std::size_t(3) * W);
            x.resize(std::size_t(3) * W);
            q.resize(std::size_t(3) * W);
          }
          gatherInterleaved(E2, y, 0, W, e.data());
          ks.logE(view, e.data(), x.data(), W);
          ks.develop1(view, x.data(), q.data(), W);
          deinterleave(q.data(), Q[0].row(y), Q[1].row(y), Q[2].row(y), W);
        },
        4);
    for (int c = 0; c < 3; ++c) applyMixture(K, ar, Q[c], Qp[c], pl.dir[c]);
    }
  }
  st.dirMs = msSince(t0);

  // --- fused tile pass ------------------------------------------------------------------------------------------------------
  t0 = Clock::now();
  FinalCfg cfg;
  const auto& gp = pl.grain;
  cfg.scan.set(pl.scanBlur);
  if (pl.unsharpSigma > 0 && pl.unsharpAmount > 0) cfg.unsharp.set(pl.unsharpSigma);
  cfg.Hs = cfg.scan.r + cfg.unsharp.r;
  if (gp.active) {
    cfg.grain.set(gp.blurSigma);
    cfg.Hg = cfg.grain.r;
    for (int ch = 0; ch < 3; ++ch)
      for (int l = 0; l < gp.ch[ch].layers; ++l) {
        cfg.dye[ch][l].set(gp.ch[ch].dyeSigma[l]);
        cfg.Hd = std::max(cfg.Hd, cfg.dye[ch][l].r);
      }
    if (gp.microActive) {
      cfg.micro.set(gp.microBlur > 0.4 ? gp.microBlur : 0.0);
      cfg.Hd = std::max(cfg.Hd, cfg.micro.r);
    }
  }
  if (pl.glare) {
    cfg.glare.set(pl.glareBlur);
    cfg.rgl = cfg.glare.r;
  }
  cfg.H = cfg.Hs + cfg.Hg + cfg.Hd;
  for (int c = 0; c < 3; ++c) {
    cfg.streamGrain[c] = streamOf(pl.seed, 1 + c);
    cfg.streamMicro[c] = streamOf(pl.seed, 11 + c);
  }
  cfg.streamGlare = streamOf(pl.seed, 21);
  const EncTable& enc = encTable();
  const float* decode = a.tables->srgbDecode.data();
  const bool split = pl.enlargerDiffusion && a.tables->split;
  Plane* Ep = nullptr;  // print exposure planes (enlarger diffusion path)
  Plane* Eb = nullptr;  // ... after the diffusion filter
  const int TW = std::min(W, a.tileW > 0 ? a.tileW : 256), TH = std::min(H, a.tileH > 0 ? a.tileH : 64);
  const int ntx = (W + TW - 1) / TW, nty = (H + TH - 1) / TH;
  const bool scanStage = pl.scanStage();

  // mode 0: whole tile chain; 1: density + grain -> print exposure planes (enlarger diffusion path); 2: paper + scan stage
  auto tile = [&](int ti, int mode) {
        PROF_BEGIN
        const int HALO = mode == 0 ? cfg.H : (mode == 1 ? cfg.Hg + cfg.Hd : cfg.Hs);
        const int tx = ti % ntx, ty = ti / ntx;
        const int x0 = tx * TW, x1 = std::min(W, x0 + TW), y0 = ty * TH, y1 = std::min(H, y0 + TH);
        const int nx = x1 - x0, ny = y1 - y0;
        const int lw = nx + 2 * HALO, lh = ny + 2 * HALO;
        const std::ptrdiff_t ls = (lw + 15) & ~15;
        const std::size_t ps = std::size_t(ls) * lh;
        const int rgl = cfg.rgl;
        const int gw = lw + 2 * rgl, gh = lh + 2 * rgl;
        const std::ptrdiff_t gls = (gw + 15) & ~15;
        const std::size_t total = ps * 13 + (pl.glare ? std::size_t(gls) * gh * 3 : 0) + std::size_t(lw + 8) * 15 + 64;
        thread_local Scratch tileScratch;
        float* base = tileScratch.get(total);
        auto plane = [&](int i) { return base + std::size_t(i) * ps; };
        float* D[3] = {plane(0), plane(1), plane(2)};
        float *A0 = plane(3), *A1 = plane(4), *A2 = plane(5), *Sp = plane(6), *Tp = plane(7), *Bp = plane(8), *Mp = plane(9);
        float* LIN[3] = {plane(10), plane(11), plane(12)};
        float* gbase = base + ps * 13;
        float* GN = gbase;
        float* GB = gbase + std::size_t(gls) * gh;
        float* GT = gbase + std::size_t(gls) * gh * 2;
        float* rows = gbase + (pl.glare ? std::size_t(gls) * gh * 3 : 0);
        float *rowE = rows, *rowX = rows + 3 * (lw + 8), *rowQ = rows + 6 * (lw + 8), *rowD = rows + 9 * (lw + 8),
              *rowO = rows + 12 * (lw + 8);

        // 1. developed density on the tile + halo
        for (int ly = 0; mode != 2 && ly < lh; ++ly) {
          const int gy = reflectIdx(y0 - HALO + ly, H);
          gatherInterleaved(E2, gy, x0 - HALO, lw, rowE);
          ks.logE(view, rowE, rowX, lw);
          if (Qp) gatherInterleaved(Qp, gy, x0 - HALO, lw, rowQ);
          else ks.develop1(view, rowX, rowQ, lw);
          ks.develop2(view, rowX, rowQ, rowD, lw);
          deinterleave(rowD, D[0] + ly * ls, D[1] + ly * ls, D[2] + ly * ls, lw);
        }
        Rect cur{0, 0, lw, lh};
        PROF_LAP(0)

        // 2. grain
        if (gp.active && mode != 2) {
          const Rect rd = shrink(cur, cfg.Hd), rg = shrink(rd, cfg.Hg);
          float* A[3] = {A0, A1, A2};
          for (int ch = 0; ch < 3; ++ch) {
            const GrainChannelPlan& gc = gp.ch[ch];
            bool dyeBlur = false;
            for (int l = 0; l < gc.layers; ++l) dyeBlur |= cfg.dye[ch][l].r > 0;
            for (int ly = 0; ly < lh; ++ly)
              K.grainRow(gc, D[ch] + ly * ls, A0 + ly * ls, A1 + ly * ls, A2 + ly * ls, lw, x0 - HALO, y0 - HALO + ly,
                         cfg.streamGrain[ch], dyeBlur ? 0 : 1);
            PROF_LAP(6)
            float* G = A0;
            if (dyeBlur) {
              G = Sp;
              for (int l = 0; l < gc.layers; ++l) {
                const float* src = A[l];
                if (cfg.dye[ch][l].r > 0) {
                  blurLocal(K, A[l], Bp, Tp, ls, rd, cfg.dye[ch][l]);
                  src = Bp;
                }
                for (int y = rd.y0; y < rd.y1; ++y) {
                  float* g = G + y * ls;
                  const float* s = src + y * ls;
                  if (l == 0)
                    for (int x = rd.x0; x < rd.x1; ++x) g[x] = s[x];
                  else
                    for (int x = rd.x0; x < rd.x1; ++x) g[x] += s[x];
                }
              }
            }
            if (gp.microActive) {
              const float sg2 = float(std::log(1.0 + gp.microStd * gp.microStd));
              for (int ly = 0; ly < lh; ++ly)
                K.logNormalRow(Mp + ly * ls, lw, x0 - HALO, y0 - HALO + ly, cfg.streamMicro[ch], -0.5f * sg2, std::sqrt(sg2));
              const float* M = Mp;
              if (cfg.micro.r > 0) {
                blurLocal(K, Mp, Bp, Tp, ls, rd, cfg.micro);
                M = Bp;
              }
              for (int y = rd.y0; y < rd.y1; ++y)
                for (int x = rd.x0; x < rd.x1; ++x) G[y * ls + x] *= M[y * ls + x];
            }
            const float dmin = gc.dminCh;
            if (cfg.grain.r > 0) {
              blurLocal(K, G, D[ch], Tp, ls, rg, cfg.grain);
              for (int y = rg.y0; y < rg.y1; ++y)
                for (int x = rg.x0; x < rg.x1; ++x) D[ch][y * ls + x] -= dmin;
            } else {
              for (int y = rg.y0; y < rg.y1; ++y)
                for (int x = rg.x0; x < rg.x1; ++x) D[ch][y * ls + x] = G[y * ls + x] - dmin;
            }
          }
          cur = rg;
        }
        PROF_LAP(1)

        // 3. print + scan LUT
        if (mode == 1) {  // linear paper exposure for the enlarger diffusion filter
          for (int y = cur.y0; y < cur.y1; ++y) {
            const int n = cur.x1 - cur.x0;
            interleave(D[0] + y * ls + cur.x0, D[1] + y * ls + cur.x0, D[2] + y * ls + cur.x0, rowD, n);
            ks.printRaw(view, rowD, rowO, n);
            K.exp2Scale(rowO, n * 3, 3.32192809f);
            const int gy = y0 + (y - cur.y0);
            deinterleave(rowO, Ep[0].row(gy) + x0, Ep[1].row(gy) + x0, Ep[2].row(gy) + x0, n);
          }
          return;
        }
        auto lutRow = [&](int y, float* dstRow) {
          const int n = cur.x1 - cur.x0;
          if (mode == 2) {
            gatherInterleaved(Eb, reflectIdx(y0 - HALO + y, H), x0 - HALO + cur.x0, n, rowE);
            ks.logE(view, rowE, rowX, n);
            ks.paperScan(view, rowX, dstRow, n);
          } else {
            interleave(D[0] + y * ls + cur.x0, D[1] + y * ls + cur.x0, D[2] + y * ls + cur.x0, rowD, n);
            ks.printScan(view, rowD, dstRow, n);
          }
        };
        if (!scanStage) {
          for (int y = cur.y0; y < cur.y1; ++y) lutRow(y, out.row(y0 + (y - cur.y0)) + 3 * std::size_t(x0));
          PROF_LAP(2)
          return;
        }
        for (int y = cur.y0; y < cur.y1; ++y) {
          const int n = cur.x1 - cur.x0;
          lutRow(y, rowO);
          for (int c = 0; c < 3; ++c) {
            float* l = LIN[c] + y * ls + cur.x0;
            for (int i = 0; i < n; ++i) {
              const float e = rowO[3 * i + c];
              if (e <= 0.f) {
                l[i] = e * (1.f / 12.92f);
              } else if (e >= 1.f) {
                l[i] = 1.f + (e - 1.f) * 2.2749f;
              } else {
                const float xx = e * 4096.f;
                const int idx = std::min(int(xx), 4095);
                const float f = xx - float(idx);
                l[i] = decode[idx] + f * (decode[idx + 1] - decode[idx]);
              }
            }
          }
        }

        PROF_LAP(2)
        // 4. stochastic glare: (blurred log-normal field - baked mean) times the white of the viewing illuminant
        if (pl.glare) {
          for (int gy = 0; gy < gh; ++gy)
            K.logNormalRow(GN + gy * gls, gw, x0 - HALO - rgl, y0 - HALO - rgl + gy, cfg.streamGlare, float(pl.glareMu),
                           float(pl.glareSigma));
          const Rect go{cur.x0 + rgl, cur.y0 + rgl, cur.x1 + rgl, cur.y1 + rgl};
          const float* field = GN;
          if (rgl > 0) {
            blurLocal(K, GN, GB, GT, gls, go, cfg.glare);
            field = GB;
          }
          const float bm = a.tables->glareRgb[3];
          for (int c = 0; c < 3; ++c) {
            const float wc = a.tables->glareRgb[c];
            for (int y = cur.y0; y < cur.y1; ++y) {
              const float* g = field + (y + rgl) * gls + rgl;
              float* l = LIN[c] + y * ls;
              for (int x = cur.x0; x < cur.x1; ++x) l[x] += (g[x] * 0.01f - bm) * wc;
            }
          }
        }

        PROF_LAP(3)
        // 5. scanner blur, unsharp mask
        float** lin = LIN;
        // A0..A2 (grain scratch) are free now: blur targets
        float* LBt[3] = {A0, A1, A2};
        if (cfg.scan.r > 0) {
          const Rect o = shrink(cur, cfg.scan.r);
          for (int c = 0; c < 3; ++c) {
            blurLocal(K, lin[c], LBt[c], Tp, ls, o, cfg.scan);
            std::swap(lin[c], LBt[c]);
          }
          cur = o;
        }
        const bool us = cfg.unsharp.w.size() > 0 && pl.unsharpAmount > 0 && pl.unsharpSigma > 0;
        const float amount = float(pl.unsharpAmount);
        if (us && cfg.unsharp.r > 0) {
          const Rect o = shrink(cur, cfg.unsharp.r);
          for (int c = 0; c < 3; ++c) blurLocal(K, lin[c], LBt[c], Tp, ls, o, cfg.unsharp);
          for (int c = 0; c < 3; ++c)
            for (int y = o.y0; y < o.y1; ++y) {
              float* l = lin[c] + y * ls;
              const float* b = LBt[c] + y * ls;
              for (int x = o.x0; x < o.x1; ++x) l[x] += amount * (l[x] - b[x]);
            }
          cur = o;
        }

        PROF_LAP(4)
        // 6. sRGB encode into the output image
        for (int y = cur.y0; y < cur.y1; ++y) {
          float* dst = out.row(y0 + (y - cur.y0)) + 3 * std::size_t(x0);
          const float *l0 = lin[0] + y * ls, *l1 = lin[1] + y * ls, *l2 = lin[2] + y * ls;
          for (int x = cur.x0; x < cur.x1; ++x) {
            float* o = dst + 3 * (x - cur.x0);
            o[0] = encodeSrgb(enc, l0[x]);
            o[1] = encodeSrgb(enc, l1[x]);
            o[2] = encodeSrgb(enc, l2[x]);
          }
        }
        PROF_LAP(5)
  };
  auto runTiles = [&](int mode) {
    parallelFor(ntx * nty, [&](int ti) { tile(ti, mode); }, 1);
  };
  if (!split) {
    runTiles(0);
  } else {
    Ep = get(3);
    runTiles(1);
    Eb = sets[curIdx];  // E2 and the inhibitor planes are free now
    for (int c = 0; c < 3; ++c) applyMixture(K, ar, Ep[c], Eb[c], pl.enl[c]);
    runTiles(2);
  }
  st.finalMs = msSince(t0);
#ifdef DAPPLE_SPATIAL_PROFILE
  std::printf("tile pass thread-ms: density %.1f grain %.1f lut+decode %.1f glare %.1f blur+unsharp %.1f encode %.1f sampler+ %.1f\n", gProf[0] * 1e-6,
              gProf[1] * 1e-6, gProf[2] * 1e-6, gProf[3] * 1e-6, gProf[4] * 1e-6, gProf[5] * 1e-6, gProf[6] * 1e-6);
  for (auto& g : gProf) g = 0;
#endif
  st.totalMs = msSince(tStart);
  if (tm) *tm = st;
}

}  // namespace dapple::film::detail
