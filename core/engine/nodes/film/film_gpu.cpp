// Film nodes as GPU passes. See film_gpu.h.
#include "engine/nodes/film/film_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/nodes/film/film_node_internal.h"

namespace dapple::engine::filmnode {

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
    r.g.dispatch(shader, p, uses, std::uint32_t(gpu::cdiv(w, 16)), std::uint32_t(gpu::cdiv(h, 16)), &u, sizeof u);
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
  const H tmp = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
  const H out = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
  r.g.dispatch("film_f4h", ph, {GpuGraph::R(in), GpuGraph::W(tmp)}, std::uint32_t(gpu::cdiv(w, 16)), std::uint32_t(gpu::cdiv(h, 16)), &u, sizeof u);
  r.g.dispatch("film_f4v", pv, {GpuGraph::R(tmp), GpuGraph::W(out)}, std::uint32_t(gpu::cdiv(w, 16)), std::uint32_t(gpu::cdiv(h, 16)), &u, sizeof u);
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
    const H out = r.g.transient(hal::Format::RGBA32F, std::uint32_t(ow), std::uint32_t(oh));
    r.g.dispatch("film_point", p, {GpuGraph::R(in), GpuGraph::R(aux), GpuGraph::R(lutH), GpuGraph::R(curH), GpuGraph::W(out)},
                 std::uint32_t(gpu::cdiv(ow, 16)), std::uint32_t(gpu::cdiv(oh, 16)), &u, sizeof u);
    return out;
  };
  if (!pl.any()) return point(3, false, src, rawH, w, h);  // the fused point chain

  H E = point(0, false, src, rawH, w, h);
  if (E < 0) return -1;
  Mix mix{r};
  auto perChannel = [&](H rgba, int sw, int sh, int mw, int mh, auto&& fn) -> H {
    H c3[3];
    for (int c = 0; c < 3; ++c) c3[c] = fn(mix.split(rgba, sw, sh, c), c);
    return mix.merge(c3[0], c3[1], c3[2], mw, mh);
  };
  H E2 = E;
  if (pl.preMix || pl.haloMix) {
    E2 = perChannel(E, w, h, w, h, [&](H ch, int c) {
      if (pl.preMix) ch = mix.mixture(ch, w, h, pl.pre[c], false);
      if (pl.haloMix) ch = mix.mixture(ch, w, h, pl.halo[c], false);
      return ch;
    });
  }
  H Q = -1;
  if (pl.dirDiffuse) {
    if (pd->dirHalf) {
      const int hw = (w + 1) / 2, hh = (h + 1) / 2;
      const H qh = point(4, false, E2, rawH, hw, hh, 0, nullptr, w, h);
      if (qh < 0) return -1;
      Q = perChannel(qh, hw, hh, w, h, [&](H ch, int c) { return mix.mixture(ch, w, h, pl.dir[c], true); });
    } else {
      const H q = point(5, false, E2, rawH, w, h);
      if (q < 0) return -1;
      Q = perChannel(q, w, h, w, h, [&](H ch, int c) { return mix.mixture(ch, w, h, pl.dir[c], false); });
    }
  }
  if (!mix.ok) return -1;
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
    H G = r.g.transient(hal::Format::RGBA32F, std::uint32_t(gw), std::uint32_t(gh));
    r.g.dispatch("film_grain", pg, {GpuGraph::R(D), GpuGraph::R(r.g.externalBuf(pres.grainTab)), GpuGraph::W(G)},
                 std::uint32_t(gpu::cdiv(gw, 16)), std::uint32_t(gpu::cdiv(gh, 16)), &gu, sizeof gu);
    if (Hg > 0) {
      G = fir4(r, G, gw, gh, pd->grain);
      if (G < 0) return -1;
    }
    D = G;
    off = Hg;
  }
  const H disp = point(2, false, D, rawH, w, h, off, dsub);
  if (disp < 0) return -1;
  if (!pd->scanStage) return disp;

  // ---- FilmGlare + FilmScan --------------------------------------------------------------------------------------------------------
  const std::uint32_t gxw = std::uint32_t(gpu::cdiv(w, 16)), gyh = std::uint32_t(gpu::cdiv(h, 16));
  PostU pu{};
  pu.dim[0] = w;
  pu.dim[1] = h;
  H L = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
  {
    const hal::PipelineH p = r.pl("film_dec", {});
    if (!p) return -1;
    r.g.dispatch("film_dec", p, {GpuGraph::R(disp), GpuGraph::R(r.g.externalBuf(tb.decode)), GpuGraph::W(L)}, gxw, gyh, &pu, sizeof pu);
  }
  if (pl.glare) {
    const int rgl = pd->rgl;
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
    H field;
    {
      const hal::PipelineH p = r.pl("film_gnoise", {});
      if (!p) return -1;
      field = r.g.transient(hal::Format::R32F, std::uint32_t(fw), std::uint32_t(fh));
      r.g.dispatch("film_gnoise", p, {GpuGraph::W(field)}, std::uint32_t(gpu::cdiv(fw, 16)), std::uint32_t(gpu::cdiv(fh, 16)), &gu, sizeof gu);
    }
    if (rgl > 0) {
      std::vector<LevelComp> comps(1);
      comps[0].r = rgl;
      comps[0].w = pd->glare.w;
      comps[0].weight = 1.f;
      field = mix.levelCombine(field, fw, fh, 0.0, comps, -1);
    }
    PostU au = pu;
    au.org[2] = rgl;
    au.org[3] = rgl;
    au.glare[2] = tb.glareRgb[3];
    for (int c = 0; c < 3; ++c) au.white[c] = tb.glareRgb[c];
    const hal::PipelineH p = r.pl("film_gadd", {});
    if (!p) return -1;
    const H L2 = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
    r.g.dispatch("film_gadd", p, {GpuGraph::R(L), GpuGraph::R(field), GpuGraph::W(L2)}, gxw, gyh, &au, sizeof au);
    L = L2;
  }
  if (pd->scan.r > 0) {
    L = fir4(r, L, w, h, pd->scan);
    if (L < 0) return -1;
  }
  const bool us = !pd->unsharp.w.empty() && pl.unsharpAmount > 0 && pl.unsharpSigma > 0 && pd->unsharp.r > 0;
  H B = L;
  PostU eu = pu;
  if (us) {
    B = fir4(r, L, w, h, pd->unsharp);
    if (B < 0) return -1;
    eu.dim[3] = 1;
    eu.glare[3] = float(pl.unsharpAmount);
  }
  const hal::PipelineH pe = r.pl("film_enc", {});
  if (!pe) return -1;
  const H out = r.g.transient(hal::Format::RGBA32F, std::uint32_t(w), std::uint32_t(h));
  r.g.dispatch("film_enc", pe, {GpuGraph::R(L), GpuGraph::R(B), GpuGraph::W(out)}, gxw, gyh, &eu, sizeof eu);
  return mix.ok ? out : -1;
}

bool gpuMixtureForTest(gpu::Core& core, const float* plane, int w, int h, const void* mixture, bool half, std::vector<float>& out,
                       std::string& err) {
  hal::Device& dev = core.dev();
  const int iw = half ? (w + 1) / 2 : w, ih = half ? (h + 1) / 2 : h;
  hal::ImageH in;
  if (dev.createImage({hal::Format::R32F, std::uint32_t(iw), std::uint32_t(ih), 1}, &in) != hal::Status::Ok) return err = dev.lastError(), false;
  {
    auto cl = dev.begin();
    if (!cl || !core.ensureStaging(cl->slot(), std::size_t(iw) * ih * 4, err)) return false;
    std::memcpy(core.slot(cl->slot()).staging.mapped, plane, std::size_t(iw) * ih * 4);
    GpuGraph g(dev, core.pool());
    g.copyBufferToImage(core.slot(cl->slot()).staging, g.external(in), std::uint32_t(iw), std::uint32_t(ih), 1, 0);
    if (gpu::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok || dev.waitAll() != hal::Status::Ok) return false;
  }
  auto cl = dev.begin();
  if (!cl || !core.ensureReadback(cl->slot(), std::size_t(w) * h * 4, err)) return false;
  GpuGraph g(dev, core.pool());
  gpu::GpuRecorder rec(dev, g, err);
  Mix mix{rec};
  const H o = mix.mixture(g.external(in), w, h, *static_cast<const fd::Mixture*>(mixture), half);
  if (!mix.ok || o < 0) return false;
  g.copyImageToBuffer(o, core.slot(cl->slot()).readback, 0, 0, std::uint32_t(w), std::uint32_t(h));
  hal::FenceH f;
  if (gpu::runGraph(dev, g, *cl, &f, err) != hal::Status::Ok || dev.wait(f, 30ull * 1000 * 1000 * 1000) != hal::Status::Ok) return err = dev.lastError(), false;
  dev.invalidate(core.slot(cl->slot()).readback, 0, std::size_t(w) * h * 4);
  out.assign(static_cast<const float*>(core.slot(cl->slot()).readback.mapped), static_cast<const float*>(core.slot(cl->slot()).readback.mapped) + std::size_t(w) * h);
  dev.waitAll();
  dev.destroy(in);
  return true;
}

}  // namespace dapple::engine::filmnode
