// CPU implementations of the film image nodes. They are the pieces of film_spatial_render.cpp renderSpatial() (exposure,
// mixtures, DIR field, and the fused tile pass: develop, grain, print/scan, glare, scanner blur + unsharp, encode), each on a
// window of a planner-owned buffer, calling the very same SIMD kernels in the same order: composing them reproduces
// FilmSimulator::render byte for byte (tests/engine/film_nodes_tests.cpp).
//
// Rect conventions: every function derives the pixels it computes from its OUTPUT view; input windows must cover the output
// rect grown by the node's halo. Boundary policy: scipy 'reflect' against CpuCtx::frame.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "engine/nodes/film/film_node_internal.h"
#include "parallel.h"

namespace dapple::engine::filmnode {

thread_local fd::Arena* g_frameArena = nullptr;

using dapple::parallelFor;

namespace {

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

// n pixels of the three planes of `v` starting at absolute column gx0 of absolute row gy, columns reflected into the frame.
inline void gatherView(const ImageView& v, const Rect& frame, int gy, int gx0, int n, float* out) {
  const int W = frame.x1 - frame.x0;
  const float *r0 = planeRow(v, 0, gy, frame.x0), *r1 = planeRow(v, 1, gy, frame.x0), *r2 = planeRow(v, 2, gy, frame.x0);
  auto one = [&](int i) {
    const int gx = reflectIdx(gx0 + i - frame.x0, W);
    out[3 * i] = r0[gx];
    out[3 * i + 1] = r1[gx];
    out[3 * i + 2] = r2[gx];
  };
  const int lo = std::clamp(frame.x0 - gx0, 0, n), hi = std::clamp(frame.x1 - gx0, lo, n);
  for (int i = 0; i < lo; ++i) one(i);
  for (int i = lo; i < hi; ++i) {
    const int gx = gx0 + i - frame.x0;
    out[3 * i] = r0[gx];
    out[3 * i + 1] = r1[gx];
    out[3 * i + 2] = r2[gx];
  }
  for (int i = hi; i < n; ++i) one(i);
}

// Separable blur on a common local coordinate frame: src, tmp and dst are indexed [y * stride + x] with their own strides;
// `o` is the output rect, src must be valid on o grown by t.r.
void blurLocal(const fd::SpatialKernels& K, const float* src, std::ptrdiff_t ss, float* dst, std::ptrdiff_t ds, float* tmp,
               std::ptrdiff_t ts, Rect o, const Taps& t) {
  const int n = o.x1 - o.x0;
  if (n <= 0 || o.y1 <= o.y0) return;
  for (int y = o.y0 - t.r; y < o.y1 + t.r; ++y) K.firRow(src + y * ss + o.x0, tmp + y * ts + o.x0, n, t.w.data(), t.r);
  for (int y = o.y0; y < o.y1; ++y) K.firCol(tmp + y * ts + o.x0, ts, dst + y * ds + o.x0, n, t.w.data(), t.r, 0);
}

// Whole-frame workspace of the Composite nodes (planes persist between frames: first touch is the only page-fault cost).
struct Workspace {
  std::mutex m;
  fd::Arena arena;
};
Workspace& workspace() {
  static Workspace w;
  return w;
}

// The arena a whole-frame node works in: the calling frame driver's own, or the process-wide one under its mutex.
struct ArenaLease {
  fd::Arena* arena = nullptr;
  std::unique_lock<std::mutex> lock;
};
ArenaLease leaseArena() {
  ArenaLease l;
  if (g_frameArena) {
    l.arena = g_frameArena;
  } else {
    Workspace& ws = workspace();
    l.lock = std::unique_lock<std::mutex>(ws.m);
    l.arena = &ws.arena;
  }
  l.arena->reset();
  return l;
}

const FilmResolved& R(const void* p) { return *static_cast<const FilmResolved*>(p); }

// One per-thread workspace shared by the tile nodes (they run one after the other on a thread): keeps the cache footprint of
// the tile pass at that of the old fused function instead of the sum of all nodes.
float* nodeScratch(std::size_t floats) {
  thread_local fd::Scratch s;
  return s.get(floats);
}

}  // namespace

// ---- FilmExposure -----------------------------------------------------------------------------------------------------------------
Status runExposure(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;  // (highlight boost needs the global maximum: it runs in FilmBlurExposure)
  const RawTable* raw = payloadAs<RawTable>(c.tables[0]);
  float mi[12];
  const fd::KView view = viewOf(raw, nullptr, nullptr, r.gain, mi);
  const fd::KernelSet& ks = fd::selectKernels();
  const ImageView &src = in[0], &E = out[0];
  const Rect o = rectOf(E);
  const int n = rw(o);
  if (n <= 0) return Status::Ok;
  thread_local std::vector<float> row;
  if (int(row.size()) < 3 * n) row.resize(std::size_t(3) * n);
  const float scale = 1.f;  // 2^(camera exposure compensation (0) + auto exposure (off)): the user's gain is in r.gain
  for (int y = o.y0; y < o.y1; ++y) {
    ks.exposure(view, packedRow(src, y, o.x0), row.data(), n, scale);
    deinterleave(row.data(), planeRow(E, 0, y, o.x0), planeRow(E, 1, y, o.x0), planeRow(E, 2, y, o.x0), n);
  }
  return Status::Ok;
}

// ---- FilmBlurExposure ---------------------------------------------------------------------------------------------------------------
Status runBlurExposure(const CpuCtx&, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialPlan& pl = pd->plan;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  fd::Plane src[3], dst[3];
  for (int c = 0; c < 3; ++c) {
    src[c] = planeOf(in[0], c);
    dst[c] = planeOf(out[0], c);
  }
  const int W = out[0].width, H = out[0].height;
  ArenaLease lease = leaseArena();
  fd::Arena& arena = *lease.arena;
  // highlight boost (spec 03 s4.1): v > x0 -> v + X k (exp(a d) - a d - 1), X = the maximum of the whole exposure frame. The boosted
  // copy replaces the input of the mixtures (the input planes stay untouched).
  if (pl.boost) {
    std::vector<float> rowMax(std::size_t(H), 0.f);
    parallelFor(
        H,
        [&](int y) {
          float m = 0.f;
          for (int c = 0; c < 3; ++c) {
            const float* r = src[c].row(y);
            for (int x = 0; x < W; ++x) m = std::max(m, r[x]);
          }
          rowMax[std::size_t(y)] = m;
        },
        4);
    double X = 0;
    for (float v : rowMax) X = std::max(X, double(v));
    double x0, aa, k;
    if (fd::boostConstants(pl.boostM, pl.boostA, pl.boostP, X, x0, aa, k) && x0 < X) {
      const float fx0 = float(x0), fX = float(X), fa = float(aa), fk = float(k);
      const bool mixes = pl.preMix || pl.haloMix;
      fd::Plane bs[3];
      for (int c = 0; c < 3; ++c) bs[c] = mixes ? arena.plane(W, H) : dst[c];
      parallelFor(
          H,
          [&](int y) {
            for (int c = 0; c < 3; ++c) {
              std::memcpy(bs[c].row(y), src[c].row(y), sizeof(float) * W);
              K.boostRow(bs[c].row(y), W, fx0, fX, fa, fk);
            }
          },
          4);
      for (int c = 0; c < 3; ++c) src[c] = bs[c];
      if (!mixes) return Status::Ok;
    }
  }
  if (pl.preMix && pl.haloMix) {
    fd::Plane mid[3] = {arena.plane(W, H), arena.plane(W, H), arena.plane(W, H)};
    for (int c = 0; c < 3; ++c) fd::applyMixture(K, arena, src[c], mid[c], pl.pre[c]);
    for (int c = 0; c < 3; ++c) fd::applyMixture(K, arena, mid[c], dst[c], pl.halo[c]);
  } else if (pl.preMix || pl.haloMix) {
    const fd::Mixture* m = pl.preMix ? pl.pre : pl.halo;
    for (int c = 0; c < 3; ++c) fd::applyMixture(K, arena, src[c], dst[c], m[c]);
  } else {
    for (int c = 0; c < 3; ++c)
      for (int y = 0; y < H; ++y) std::memcpy(dst[c].row(y), src[c].row(y), sizeof(float) * W);
  }
  return Status::Ok;
}

// ---- FilmDirField -----------------------------------------------------------------------------------------------------------------------
Status runDirField(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialPlan& pl = pd->plan;
  if (!pl.dirDiffuse) return Status::Ok;  // identity: FilmDevelop computes the field in its tile
  const CurvesTable* cur = payloadAs<CurvesTable>(c.tables[0]);
  float mi[12];
  const fd::KView view = viewOf(nullptr, cur, nullptr, nullptr, mi);
  const fd::KernelSet& ks = fd::selectKernels();
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const ImageView& E2 = in[0];
  const Rect frame = rectOf(E2);
  const int W = E2.width, H = E2.height;
  fd::Plane Qp[3];
  for (int ch = 0; ch < 3; ++ch) Qp[ch] = planeOf(out[0], ch);
  ArenaLease lease = leaseArena();
  fd::Arena& ar = *lease.arena;
  if (pd->dirHalf) {
    // the inhibitor field is smooth (sigma >= 3.2 px everywhere): build it box-averaged at half resolution directly
    const int hw = (W + 1) / 2, hh = (H + 1) / 2;
    fd::Plane Qh[3] = {ar.plane(hw, hh), ar.plane(hw, hh), ar.plane(hw, hh)};
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
          gatherView(E2, frame, frame.y0 + ya, frame.x0, W, e.data());
          gatherView(E2, frame, frame.y0 + yb, frame.x0, W, e.data() + 3 * std::size_t(W));
          ks.logE(view, e.data(), x.data(), 2 * W);
          ks.develop1(view, x.data(), q.data(), 2 * W);
          const float* qa = q.data();
          const float* qb = q.data() + 3 * std::size_t(W);
          for (int ch = 0; ch < 3; ++ch) {
            float* o = Qh[ch].row(j);
            const int full = W / 2;
            for (int i = 0; i < full; ++i)
              o[i] = 0.25f * (qa[6 * i + ch] + qa[6 * i + 3 + ch] + qb[6 * i + ch] + qb[6 * i + 3 + ch]);
            if (hw > full) o[full] = 0.5f * (qa[6 * full + ch] + qb[6 * full + ch]);
          }
        },
        2);
    for (int ch = 0; ch < 3; ++ch) fd::applyMixtureHalf(K, ar, Qh[ch], Qp[ch], pl.dir[ch]);
  } else {
    fd::Plane Q[3] = {ar.plane(W, H), ar.plane(W, H), ar.plane(W, H)};
    parallelFor(
        H,
        [&](int y) {
          thread_local std::vector<float> e, x, q;
          if (int(e.size()) < 3 * W) {
            e.resize(std::size_t(3) * W);
            x.resize(std::size_t(3) * W);
            q.resize(std::size_t(3) * W);
          }
          gatherView(E2, frame, frame.y0 + y, frame.x0, W, e.data());
          ks.logE(view, e.data(), x.data(), W);
          ks.develop1(view, x.data(), q.data(), W);
          deinterleave(q.data(), Q[0].row(y), Q[1].row(y), Q[2].row(y), W);
        },
        4);
    for (int ch = 0; ch < 3; ++ch) fd::applyMixture(K, ar, Q[ch], Qp[ch], pl.dir[ch]);
  }
  return Status::Ok;
}

// ---- FilmDevelop ----------------------------------------------------------------------------------------------------------------------------
// in[0] = E2 (planar, covers the frame), in[1] (optional) = q field (planar, covers the frame), out[0] = density D (planar window).
Status runDevelop(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const bool haveQ = in.size() > 1 && in[1].data != nullptr && pd->plan.dirDiffuse;
  const CurvesTable* cur = payloadAs<CurvesTable>(c.tables[0]);
  float mi[12];
  const fd::KView view = viewOf(nullptr, cur, nullptr, nullptr, mi);
  const fd::KernelSet& ks = fd::selectKernels();
  const ImageView& D = out[0];
  const Rect o = rectOf(D);
  const Rect frame = c.frame;
  const int H = frame.y1 - frame.y0;
  const int n = rw(o);
  if (n <= 0) return Status::Ok;
  thread_local std::vector<float> rowE, rowX, rowQ, rowD;
  const std::size_t need = std::size_t(3) * (n + 8);
  if (rowE.size() < need) rowE.resize(need), rowX.resize(need), rowQ.resize(need), rowD.resize(need);
  for (int y = o.y0; y < o.y1; ++y) {
    const int gy = frame.y0 + reflectIdx(y - frame.y0, H);
    gatherView(in[0], frame, gy, o.x0, n, rowE.data());
    ks.logE(view, rowE.data(), rowX.data(), n);
    if (haveQ) gatherView(in[1], frame, gy, o.x0, n, rowQ.data());
    else ks.develop1(view, rowX.data(), rowQ.data(), n);
    ks.develop2(view, rowX.data(), rowQ.data(), rowD.data(), n);
    deinterleave(rowD.data(), planeRow(D, 0, y, o.x0), planeRow(D, 1, y, o.x0), planeRow(D, 2, y, o.x0), n);
  }
  return Status::Ok;
}

// ---- FilmGrain ------------------------------------------------------------------------------------------------------------------------------
// in[0] = density planes covering out rect grown by (dye + blur halo), out[0] = density planes (may alias in[0]).
Status runGrain(const CpuCtx&, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialPlan& pl = pd->plan;
  const fd::GrainPlan& gp = pl.grain;
  const ImageView &Din = in[0], &Dout = out[0];
  if (!gp.active) {  // identity (normally elided): copy
    const Rect o = rectOf(Dout);
    for (int ch = 0; ch < 3; ++ch)
      for (int y = o.y0; y < o.y1; ++y)
        if (planeRow(Dout, ch, y, o.x0) != planeRow(Din, ch, y, o.x0))
          std::memcpy(planeRow(Dout, ch, y, o.x0), planeRow(Din, ch, y, o.x0), sizeof(float) * rw(o));
    return Status::Ok;
  }
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const Rect ri = rectOf(Din), ro = rectOf(Dout);
  const int Hdg = pd->Hd + pd->Hg;
  if (ro.x0 - Hdg < ri.x0 || ro.y0 - Hdg < ri.y0 || ro.x1 + Hdg > ri.x1 || ro.y1 + Hdg > ri.y1) return Status::Internal;
  const int lw = rw(ri), lh = rh(ri);
  const std::ptrdiff_t ls = (lw + 15) & ~15;
  const std::size_t ps = std::size_t(ls) * lh;
  float* base = nodeScratch(ps * 7 + 64);
  float *A0 = base, *A1 = base + ps, *A2 = base + 2 * ps, *Sp = base + 3 * ps, *Tp = base + 4 * ps, *Bp = base + 5 * ps,
        *Mp = base + 6 * ps;
  float* A[3] = {A0, A1, A2};
  // local coordinates: (0, 0) = window origin
  const Rect lrg{ro.x0 - ri.x0, ro.y0 - ri.y0, ro.x1 - ri.x0, ro.y1 - ri.y0};
  const Rect lrd = grow(lrg, pd->Hg);
  const int x0 = ri.x0, y0 = ri.y0;
  for (int ch = 0; ch < 3; ++ch) {
    const fd::GrainChannelPlan& gc = gp.ch[ch];
    bool dyeBlur = false;
    for (int l = 0; l < gc.layers; ++l) dyeBlur |= pd->dye[ch][l].r > 0;
    for (int ly = 0; ly < lh; ++ly)
      K.grainRow(gc, planeRow(Din, ch, y0 + ly, x0), A0 + ly * ls, A1 + ly * ls, A2 + ly * ls, lw, x0, y0 + ly, pd->streamGrain[ch],
                 dyeBlur ? 0 : 1);
    float* G = A0;
    if (dyeBlur) {
      G = Sp;
      for (int l = 0; l < gc.layers; ++l) {
        const float* src = A[l];
        if (pd->dye[ch][l].r > 0) {
          blurLocal(K, A[l], ls, Bp, ls, Tp, ls, lrd, pd->dye[ch][l]);
          src = Bp;
        }
        for (int y = lrd.y0; y < lrd.y1; ++y) {
          float* g = G + y * ls;
          const float* s = src + y * ls;
          if (l == 0)
            for (int x = lrd.x0; x < lrd.x1; ++x) g[x] = s[x];
          else
            for (int x = lrd.x0; x < lrd.x1; ++x) g[x] += s[x];
        }
      }
    }
    if (gp.microActive) {
      const float sg2 = float(std::log(1.0 + gp.microStd * gp.microStd));
      for (int ly = 0; ly < lh; ++ly)
        K.logNormalRow(Mp + ly * ls, lw, x0, y0 + ly, pd->streamMicro[ch], -0.5f * sg2, std::sqrt(sg2));
      const float* M = Mp;
      if (pd->micro.r > 0) {
        blurLocal(K, Mp, ls, Bp, ls, Tp, ls, lrd, pd->micro);
        M = Bp;
      }
      for (int y = lrd.y0; y < lrd.y1; ++y)
        for (int x = lrd.x0; x < lrd.x1; ++x) G[y * ls + x] *= M[y * ls + x];
    }
    const float dmin = gc.dminCh;
    if (pd->grain.r > 0) {
      // blur straight into the output plane (G is already complete: the input plane of this channel is not read again)
      float* dstBase = planeRow(Dout, ch, ro.y0, ro.x0) - std::ptrdiff_t(lrg.y0) * std::ptrdiff_t(Dout.strideFloats) - lrg.x0;
      blurLocal(K, G, ls, dstBase, std::ptrdiff_t(Dout.strideFloats), Tp, ls, lrg, pd->grain);
      for (int y = 0; y < rh(lrg); ++y) {
        float* dst = planeRow(Dout, ch, ro.y0 + y, ro.x0);
        for (int x = 0; x < rw(lrg); ++x) dst[x] -= dmin;
      }
    } else {
      for (int y = lrg.y0; y < lrg.y1; ++y) {
        float* dst = planeRow(Dout, ch, ro.y0 + (y - lrg.y0), ro.x0);
        const float* sp = G + y * ls + lrg.x0;
        for (int x = 0; x < rw(lrg); ++x) dst[x] = sp[x] - dmin;
      }
    }
  }
  return Status::Ok;
}

// ---- FilmPrintExposure (enlarger diffusion path) ----------------------------------------------------------------------------------------------
// in[0] = density planes (covers out rect), out[0] = linear paper exposure planes (a window of the frame-sized planes).
Status runPrintExposure(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  if (!pd->split) {  // identity (normally elided): copy
    const Rect o = rectOf(out[0]);
    for (int ch = 0; ch < 3; ++ch)
      for (int y = o.y0; y < o.y1; ++y)
        if (planeRow(out[0], ch, y, o.x0) != planeRow(in[0], ch, y, o.x0))
          std::memcpy(planeRow(out[0], ch, y, o.x0), planeRow(in[0], ch, y, o.x0), sizeof(float) * rw(o));
    return Status::Ok;
  }
  const PrintTable* pt = payloadAs<PrintTable>(c.tables[0]);
  if (!pt->t.split) return Status::Internal;
  float mi[12];
  const fd::KView view = viewOf(nullptr, nullptr, pt, nullptr, mi);
  const fd::KernelSet& ks = fd::selectKernels();
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const Rect o = rectOf(out[0]);
  const int n = rw(o);
  if (n <= 0) return Status::Ok;
  thread_local std::vector<float> rowD, rowO;
  if (int(rowD.size()) < 3 * std::size_t(n)) rowD.resize(std::size_t(3) * n), rowO.resize(std::size_t(3) * n);
  for (int y = o.y0; y < o.y1; ++y) {
    interleave(planeRow(in[0], 0, y, o.x0), planeRow(in[0], 1, y, o.x0), planeRow(in[0], 2, y, o.x0), rowD.data(), n);
    ks.printRaw(view, rowD.data(), rowO.data(), n);
    K.exp2Scale(rowO.data(), n * 3, 3.32192809f);  // 10^x
    deinterleave(rowO.data(), planeRow(out[0], 0, y, o.x0), planeRow(out[0], 1, y, o.x0), planeRow(out[0], 2, y, o.x0), n);
  }
  return Status::Ok;
}

// ---- FilmEnlargerBlur (enlarger diffusion path) -------------------------------------------------------------------------------------------------
// Whole-frame node: in[0] and out[0] are frame-sized planar paper exposure; out[0] = the exposure through the filter mixtures.
Status runEnlargerBlur(const CpuCtx&, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const int W = out[0].width, H = out[0].height;
  if (!pd->split) {
    for (int c = 0; c < 3; ++c)
      for (int y = 0; y < H; ++y) {
        float* d = planeRow(out[0], c, y + out[0].origin.y0, out[0].origin.x0);
        const float* s = planeRow(in[0], c, y + in[0].origin.y0, in[0].origin.x0);
        if (d != s) std::memcpy(d, s, sizeof(float) * W);
      }
    return Status::Ok;
  }
  ArenaLease lease = leaseArena();
  for (int c = 0; c < 3; ++c) fd::applyMixture(K, *lease.arena, planeOf(in[0], c), planeOf(out[0], c), pd->plan.enl[c]);
  return Status::Ok;
}

// ---- FilmPrintScan --------------------------------------------------------------------------------------------------------------------------
// in[0] = density planes (covers out rect), out[0] = interleaved display sRGB (encoded, unclamped).
// Enlarger diffusion path: in[0] = the filtered linear paper exposure of the whole frame (read with the reflect boundary against
// CpuCtx::frame); the node applies log10, the paper curves and the scan LUT instead of the density LUT.
Status runPrintScan(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const PrintTable* pt = payloadAs<PrintTable>(c.tables[0]);
  float mi[12];
  const fd::KView view = viewOf(nullptr, nullptr, pt, nullptr, mi);
  const fd::KernelSet& ks = fd::selectKernels();
  const Rect o = rectOf(out[0]);
  const int n = rw(o);
  if (n <= 0) return Status::Ok;
  if (pd->split) {
    if (!pt->t.split) return Status::Internal;
    const Rect frame = c.frame;
    const int H = frame.y1 - frame.y0;
    thread_local std::vector<float> rowE, rowX;
    if (rowE.size() < std::size_t(3) * n) rowE.resize(std::size_t(3) * n), rowX.resize(std::size_t(3) * n);
    for (int y = o.y0; y < o.y1; ++y) {
      const int gy = frame.y0 + reflectIdx(y - frame.y0, H);
      gatherView(in[0], frame, gy, o.x0, n, rowE.data());
      ks.logE(view, rowE.data(), rowX.data(), n);
      ks.paperScan(view, rowX.data(), packedRow(out[0], y, o.x0), n);
    }
    return Status::Ok;
  }
  thread_local std::vector<float> rowD;
  if (int(rowD.size()) < 3 * n) rowD.resize(std::size_t(3) * n);
  for (int y = o.y0; y < o.y1; ++y) {
    interleave(planeRow(in[0], 0, y, o.x0), planeRow(in[0], 1, y, o.x0), planeRow(in[0], 2, y, o.x0), rowD.data(), n);
    ks.printScan(view, rowD.data(), packedRow(out[0], y, o.x0), n);
  }
  return Status::Ok;
}

// ---- FilmGlare ------------------------------------------------------------------------------------------------------------------------------
// in[0] = interleaved display sRGB (covers out rect), out[0] = linear-light planes: sRGB decode, plus the stochastic glare
// (blurred log-normal hash field minus its baked mean) when the paper print has glare.
Status runGlare(const CpuCtx& c, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty() || c.tables.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialPlan& pl = pd->plan;
  const PrintTable* pt = payloadAs<PrintTable>(c.tables[0]);
  const float* decode = pt->t.srgbDecode.data();
  const ImageView &src = in[0], &L = out[0];
  const Rect o = rectOf(L);
  const int n = rw(o), m = rh(o);
  if (n <= 0 || m <= 0) return Status::Ok;
  for (int y = o.y0; y < o.y1; ++y) {
    const float* row = packedRow(src, y, o.x0);
    for (int ch = 0; ch < 3; ++ch) {
      float* l = planeRow(L, ch, y, o.x0);
      for (int i = 0; i < n; ++i) {
        const float e = row[3 * i + ch];
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
  if (!pl.glare) return Status::Ok;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const int rgl = pd->rgl;
  const int gw = n + 2 * rgl, gh = m + 2 * rgl;
  const std::ptrdiff_t gls = (gw + 15) & ~15;
  float* gbase = nodeScratch(std::size_t(gls) * gh * 3 + 64);
  float *GN = gbase, *GB = gbase + std::size_t(gls) * gh, *GT = gbase + std::size_t(gls) * gh * 2;
  for (int gy = 0; gy < gh; ++gy)
    K.logNormalRow(GN + gy * gls, gw, o.x0 - rgl, o.y0 - rgl + gy, pd->streamGlare, float(pl.glareMu), float(pl.glareSigma));
  const Rect go{rgl, rgl, rgl + n, rgl + m};
  const float* field = GN;
  if (rgl > 0) {
    blurLocal(K, GN, gls, GB, gls, GT, gls, go, pd->glare);
    field = GB;
  }
  const float bm = pt->t.glareRgb[3];
  for (int ch = 0; ch < 3; ++ch) {
    const float wc = pt->t.glareRgb[ch];
    for (int y = 0; y < m; ++y) {
      const float* g = field + (y + rgl) * gls + rgl;
      float* l = planeRow(L, ch, o.y0 + y, o.x0);
      for (int x = 0; x < n; ++x) l[x] += (g[x] * 0.01f - bm) * wc;
    }
  }
  return Status::Ok;
}

// ---- FilmScan ---------------------------------------------------------------------------------------------------------------------------------
// in[0] = linear-light planes covering out rect grown by (scanner blur + unsharp radius), out[0] = interleaved display sRGB.
Status runScan(const CpuCtx&, const void* res, std::span<const ImageView> in, std::span<const ImageView> out) {
  if (in.empty() || out.empty()) return Status::Internal;
  const FilmResolved& r = R(res);
  const auto pd = planOf(r);
  if (!pd->error.empty()) return Status::Unsupported;
  const fd::SpatialPlan& pl = pd->plan;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  const ImageView &Lin = in[0], &Out = out[0];
  const Rect ri = rectOf(Lin), ro = rectOf(Out);
  const int Hs = pd->Hs;
  if (ro.x0 - Hs < ri.x0 || ro.y0 - Hs < ri.y0 || ro.x1 + Hs > ri.x1 || ro.y1 + Hs > ri.y1) return Status::Internal;
  const int lw = rw(ri), lh = rh(ri);
  const std::ptrdiff_t ls = (lw + 15) & ~15;
  const std::size_t ps = std::size_t(ls) * lh;
  float* base = nodeScratch(ps * 7 + 64);
  float* A[3] = {base, base + ps, base + 2 * ps};          // scanner blur output
  float* B[3] = {base + 3 * ps, base + 4 * ps, base + 5 * ps};  // unsharp blur output
  float* Tp = base + 6 * ps;
  // local coordinates: (0, 0) = window origin; the input planes are read in place (no copy)
  const float* lin[3];
  std::ptrdiff_t lstr = std::ptrdiff_t(Lin.strideFloats);
  for (int ch = 0; ch < 3; ++ch) lin[ch] = planeRow(Lin, ch, ri.y0, ri.x0);
  const Rect lo{ro.x0 - ri.x0, ro.y0 - ri.y0, ro.x1 - ri.x0, ro.y1 - ri.y0};
  const bool us = !pd->unsharp.w.empty() && pl.unsharpAmount > 0 && pl.unsharpSigma > 0 && pd->unsharp.r > 0;
  if (pd->scan.r > 0) {
    const Rect o = grow(lo, us ? pd->unsharp.r : 0);
    for (int ch = 0; ch < 3; ++ch) {
      blurLocal(K, lin[ch], lstr, A[ch], ls, Tp, ls, o, pd->scan);
      lin[ch] = A[ch];
    }
    lstr = ls;
  }
  const float amount = float(pl.unsharpAmount);
  if (us)
    for (int ch = 0; ch < 3; ++ch) blurLocal(K, lin[ch], lstr, B[ch], ls, Tp, ls, lo, pd->unsharp);
  const EncTable& enc = encTable();
  for (int y = lo.y0; y < lo.y1; ++y) {
    float* dst = packedRow(Out, ro.y0 + (y - lo.y0), ro.x0);
    const float *l0 = lin[0] + y * lstr, *l1 = lin[1] + y * lstr, *l2 = lin[2] + y * lstr;
    if (us) {
      const float *b0 = B[0] + y * ls, *b1 = B[1] + y * ls, *b2 = B[2] + y * ls;
      for (int x = lo.x0; x < lo.x1; ++x) {
        float* o = dst + 3 * (x - lo.x0);
        float v0 = l0[x], v1 = l1[x], v2 = l2[x];
        v0 += amount * (v0 - b0[x]);
        v1 += amount * (v1 - b1[x]);
        v2 += amount * (v2 - b2[x]);
        o[0] = encodeSrgb(enc, v0);
        o[1] = encodeSrgb(enc, v1);
        o[2] = encodeSrgb(enc, v2);
      }
    } else {
      for (int x = lo.x0; x < lo.x1; ++x) {
        float* o = dst + 3 * (x - lo.x0);
        o[0] = encodeSrgb(enc, l0[x]);
        o[1] = encodeSrgb(enc, l1[x]);
        o[2] = encodeSrgb(enc, l2[x]);
      }
    }
  }
  return Status::Ok;
}

// ---- registration -----------------------------------------------------------------------------------------------------------------------------
namespace {

void resolveAny(const ParamSet& ps, const FrameGeom& g, void* out) { resolveFilm(ps, g, *static_cast<FilmResolved*>(out)); }

bool identityOf(NodeType t, const void* res) {
  const FilmResolved& r = R(res);
  if (!r.stock) return false;
  const auto pd = planOf(r);
  if (!pd->error.empty()) return false;
  const fd::SpatialPlan& p = pd->plan;
  switch (t) {
    case NodeType::FilmBlurExposure: return !(p.preMix || p.haloMix || p.boost);
    case NodeType::FilmPrintExposure:
    case NodeType::FilmEnlargerBlur: return !pd->split;
    case NodeType::FilmDirField: return !p.dirDiffuse;
    case NodeType::FilmGrain: return !p.grain.active;
    case NodeType::FilmGlare:
    case NodeType::FilmScan: return !pd->scanStage;
    default: return false;
  }
}

template <NodeType T>
bool identityFn(const void* res) { return identityOf(T, res); }

Halo haloOf(NodeType t, const void* res) {
  const FilmResolved& r = R(res);
  if (!r.stock) return {};
  const auto pd = planOf(r);
  if (t == NodeType::FilmGrain) return {pd->Hd + pd->Hg, pd->Hd + pd->Hg};
  if (t == NodeType::FilmScan) return {pd->Hs, pd->Hs};
  return {};
}
template <NodeType T>
Halo haloFn(const void* res) { return haloOf(T, res); }

NodeBehaviour behaviourFor(NodeType t) {
  NodeBehaviour b;
  b.resolvedBytes = sizeof(FilmResolved);
  b.resolve = resolveAny;
  switch (t) {
    case NodeType::FilmBlurExposure: b.isIdentity = identityFn<NodeType::FilmBlurExposure>; break;
    case NodeType::FilmPrintExposure: b.isIdentity = identityFn<NodeType::FilmPrintExposure>; break;
    case NodeType::FilmEnlargerBlur: b.isIdentity = identityFn<NodeType::FilmEnlargerBlur>; break;
    case NodeType::FilmDirField: b.isIdentity = identityFn<NodeType::FilmDirField>; break;
    case NodeType::FilmGrain: b.isIdentity = identityFn<NodeType::FilmGrain>; b.halo = haloFn<NodeType::FilmGrain>; break;
    case NodeType::FilmGlare: b.isIdentity = identityFn<NodeType::FilmGlare>; break;
    case NodeType::FilmScan: b.isIdentity = identityFn<NodeType::FilmScan>; b.halo = haloFn<NodeType::FilmScan>; break;
    default: break;
  }
  return b;
}

}  // namespace

bool isFilmNode(NodeType t) {
  return (t >= NodeType::FilmTableRaw && t <= NodeType::FilmScan) || t == NodeType::FilmPrintExposure || t == NodeType::FilmEnlargerBlur;
}

const CpuImpl& cpuImpl(NodeType t) {
  static const CpuImpl none{};
  static const auto img = [](Status (*run)(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>)) {
    CpuImpl i;
    i.run = run;
    return i;
  };
  static const auto tab = [](Status (*bake)(const void*, std::span<const void* const>, void**)) {
    CpuImpl i;
    i.bake = bake;
    i.freePayload = freePayload;
    return i;
  };
  static const CpuImpl exposure = img(runExposure), blur = img(runBlurExposure), dir = img(runDirField), develop = img(runDevelop),
                       grain = img(runGrain), print = img(runPrintScan), glare = img(runGlare), scan = img(runScan),
                       printExp = img(runPrintExposure), enlBlur = img(runEnlargerBlur);
  static const CpuImpl raw = tab(bakeRaw), curves = tab(bakeCurves), printTab = tab(bakePrintScan);
  switch (t) {
    case NodeType::FilmTableRaw: return raw;
    case NodeType::FilmTableCurves: return curves;
    case NodeType::FilmTablePrintScan: return printTab;
    case NodeType::FilmExposure: return exposure;
    case NodeType::FilmBlurExposure: return blur;
    case NodeType::FilmDirField: return dir;
    case NodeType::FilmDevelop: return develop;
    case NodeType::FilmGrain: return grain;
    case NodeType::FilmPrintScan: return print;
    case NodeType::FilmGlare: return glare;
    case NodeType::FilmScan: return scan;
    case NodeType::FilmPrintExposure: return printExp;
    case NodeType::FilmEnlargerBlur: return enlBlur;
    default: return none;
  }
}

const NodeBehaviour& behaviour(NodeType t) {
  static const NodeBehaviour table[] = {
      behaviourFor(NodeType::FilmTableRaw),     behaviourFor(NodeType::FilmTableCurves), behaviourFor(NodeType::FilmTablePrintScan),
      behaviourFor(NodeType::FilmExposure),     behaviourFor(NodeType::FilmBlurExposure), behaviourFor(NodeType::FilmDirField),
      behaviourFor(NodeType::FilmDevelop),      behaviourFor(NodeType::FilmGrain),        behaviourFor(NodeType::FilmPrintScan),
      behaviourFor(NodeType::FilmGlare),        behaviourFor(NodeType::FilmScan),         behaviourFor(NodeType::FilmPrintExposure),
      behaviourFor(NodeType::FilmEnlargerBlur)};
  static const NodeBehaviour empty{};
  if (!isFilmNode(t)) return empty;
  if (t == NodeType::FilmPrintExposure) return table[11];
  if (t == NodeType::FilmEnlargerBlur) return table[12];
  return table[int(t) - int(NodeType::FilmTableRaw)];
}

}  // namespace dapple::engine::filmnode
