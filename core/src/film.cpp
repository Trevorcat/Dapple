// FilmSimulator: parameter digest, bake cache, fused / staged run paths, auto exposure.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <shared_mutex>

#include "dapple/film.h"
#include "film_internal.h"
#include "film_kernels.h"
#include "film_spatial.h"
#include "parallel.h"

namespace dapple::film {

using namespace detail;
using Clock = std::chrono::steady_clock;

namespace detail {
const KernelSet& selectKernels() {
#if defined(DAPPLE_FILM_HAVE_AVX2)
  static const bool ok = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  }();
  if (ok) return kAvx2;
#endif
  return kBase;
}
}  // namespace detail

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Reference digest (spec 01 s3.5.5, s3.7, 03 s7).
void digest(FilmParams& p, const FilmStock& film) {
  auto& dc = p.filmRender.dirCouplers;
  if (p.settings.applyStocksSpecifics) {
    if (film.positive()) {
      dc.gammaSamelayerRgb = {0.12, 0.08, 0.06};
      dc.gammaInterlayerRToGb = {0.12, 0.06};
      dc.gammaInterlayerGToRb = {0.08, 0.06};
      dc.gammaInterlayerBToRg = {0.06, 0.06};
    } else {
      dc.gammaSamelayerRgb = {0.336, 0.319, 0.273};
      dc.gammaInterlayerRToGb = {0.353, 0.302};
      dc.gammaInterlayerGToRb = {0.154, 0.353};
      dc.gammaInterlayerBToRg = {0.168, 0.226};
    }
    if (film.stock == "fujifilm_velvia_100") {
      dc.gammaSamelayerRgb = {0.108, 0.072, 0.054};
      dc.gammaInterlayerRToGb = {0.108, 0.054};
      dc.gammaInterlayerGToRb = {0.072, 0.054};
      dc.gammaInterlayerBToRg = {0.054, 0.054};
    } else if (film.stock == "fujifilm_provia_100f") {
      dc.gammaSamelayerRgb = {0.156, 0.104, 0.078};
      dc.gammaInterlayerRToGb = {0.156, 0.078};
      dc.gammaInterlayerGToRb = {0.104, 0.078};
      dc.gammaInterlayerBToRg = {0.078, 0.078};
    }
  }
  if (film.support == "film") {
    const bool cine = film.use == "cine";
    const double sigma = cine ? 50.0 : 65.0;
    std::array<double, 3> strength{0.015, 0.005, 0.0};
    if (film.antihalation == "weak") strength = {0.08, 0.02, 0.0};
    else if (film.antihalation == "no") strength = {0.30, 0.10, 0.015};
    p.filmRender.halation.halationStrength = strength;
    p.filmRender.halation.halationFirstSigmaUm = {sigma, sigma, sigma};
  }
  auto killSpatial = [&] {
    auto& h = p.filmRender.halation;
    h.active = false;
    h.scatterCoreUm = h.scatterTailUm = h.halationFirstSigmaUm = {0, 0, 0};
    dc.diffusionSizeUm = 0;
    p.filmRender.grain.blur = 0;
    p.filmRender.grain.blurDyeCloudsUm = 0;
    p.printRender.glare.blur = 0;
    p.camera.lensBlurUm = 0;
    p.camera.diffusionFilter.active = false;
    p.enlarger.diffusionFilter.active = false;
    p.scanner.lensBlur = 0;
    p.scanner.unsharpMask = {0, 0};
  };
  if (p.settings.previewMode) {
    dc.diffusionSizeUm = 0;
    p.filmRender.grain.active = false;
    p.filmRender.grain.particleAreaUm2 = 0;
    p.filmRender.grain.blur = 0;
    p.printRender.glare.blur = 0;
    p.camera.lensBlurUm = 0;
    p.scanner.lensBlur = 0;
    p.scanner.unsharpMask = {0, 0};
  }
  if (p.debug.deactivateSpatialEffects) killSpatial();
  if (p.debug.deactivateStochasticEffects) {
    p.filmRender.grain.active = false;
    p.filmRender.glare.active = false;
    p.printRender.glare.active = false;
  }
}

// 128-bit key (two independent 64-bit FNV-1a style lanes with a final avalanche) of the parameters a cache
// depends on. Doubles enter by bit pattern (-0 folded into +0), so equal parameters give equal keys with no
// string formatting or allocation; a collision needs both lanes to collide.
struct Key {
  std::uint64_t a = 0xcbf29ce484222325ull, b = 0x9e3779b97f4a7c15ull;
  void word(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      const std::uint64_t byte = (v >> (8 * i)) & 0xFF;
      a = (a ^ byte) * 0x100000001b3ull;
      b = (b + byte + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
      b ^= b >> 29;
    }
  }
  bool operator==(const Key& o) const { return a == o.a && b == o.b; }
  bool operator!=(const Key& o) const { return !(*this == o); }
  bool operator<(const Key& o) const { return a != o.a ? a < o.a : b < o.b; }
};
void keyNum(Key& k, double v) {
  if (v == 0) v = 0;  // -0 -> +0
  std::uint64_t u;
  std::memcpy(&u, &v, 8);
  k.word(u);
}
void keyStr(Key& k, const std::string& s) {
  k.word(s.size());
  for (unsigned char c : s) k.word(c);
}
void keyDiffusion(Key& k, const DiffusionFilterParams& d) {
  keyNum(k, d.active);
  keyStr(k, d.filterFamily);
  for (double v : {d.strength, d.spatialScale, d.haloWarmth, d.coreIntensity, d.coreSize, d.haloIntensity, d.haloSize,
                   d.bloomIntensity, d.bloomSize})
    keyNum(k, v);
}

// Everything that changes the baked tables (not the spatial effect parameters).
Key bakeKey(const FilmParams& p) {
  Key k;
  keyStr(k, p.film);
  keyStr(k, p.paper);
  keyStr(k, p.enlarger.illuminant);
  keyNum(k, p.camera.exposureCompensationEv);
  for (double v : p.camera.filterUv) keyNum(k, v);
  for (double v : p.camera.filterIr) keyNum(k, v);
  keyNum(k, p.filmRender.densityCurveGamma);
  for (double v : p.filmRender.densityCurveGammaRgb) keyNum(k, v);
  for (double v : p.filmRender.grain.densityMin) keyNum(k, v);
  const auto& d = p.filmRender.dirCouplers;
  keyNum(k, d.active);
  keyNum(k, d.amount);
  keyNum(k, d.inhibitionSamelayer);
  keyNum(k, d.inhibitionInterlayer);
  for (double v : d.gammaSamelayerRgb) keyNum(k, v);
  for (double v : d.gammaInterlayerRToGb) keyNum(k, v);
  for (double v : d.gammaInterlayerGToRb) keyNum(k, v);
  for (double v : d.gammaInterlayerBToRg) keyNum(k, v);
  const auto& e = p.enlarger;
  for (double v : {e.printExposure, double(e.printExposureCompensation), double(e.normalizePrintExposure), e.yFilterShift,
                   e.mFilterShift, e.yFilterNeutral, e.mFilterNeutral, e.cFilterNeutral, e.preflashExposure,
                   e.preflashYFilterShift, e.preflashMFilterShift})
    keyNum(k, v);
  const auto& g = p.printRender.glare;
  keyNum(k, g.active && !p.debug.deactivateStochasticEffects);
  keyNum(k, g.percent);
  const auto& m = p.printRender.densityCurvesMorph;
  for (double v : {double(m.active), m.gammaFactor, m.gammaFactorFast, m.gammaFactorSlow, m.gammaFactorRed,
                   m.gammaFactorGreen, m.gammaFactorBlue})
    keyNum(k, v);
  keyNum(k, p.io.scanFilm);
  for (double v : {double(p.scanner.blackCorrection), double(p.scanner.whiteCorrection), p.scanner.blackLevel, p.scanner.whiteLevel})
    keyNum(k, v);
  keyNum(k, p.io.inputGamutCompress);
  keyNum(k, p.io.outputGamutCompress);
  for (double v : {p.io.inputKnee.threshold, p.io.inputKnee.limit, p.io.inputKnee.power}) keyNum(k, v);
  keyNum(k, p.settings.applyWindow);
  keyNum(k, p.settings.applySurface);
  keyNum(k, p.settings.neutralPrintFilters);
  keyNum(k, p.settings.lutResolution);
  keyNum(k, !p.io.scanFilm && p.enlarger.diffusionFilter.active);  // split print tables for the enlarger diffusion filter
  return k;
}

Key rawKey(const FilmParams& p) {
  Key k;
  keyStr(k, p.film);
  keyNum(k, p.settings.applyWindow);
  keyNum(k, p.settings.applySurface);
  keyNum(k, p.io.inputGamutCompress);
  for (double v : {p.io.inputKnee.threshold, p.io.inputKnee.limit, p.io.inputKnee.power}) keyNum(k, v);
  for (double v : p.camera.filterUv) keyNum(k, v);
  for (double v : p.camera.filterIr) keyNum(k, v);
  return k;
}

// Everything makeSpatialPlan() reads (the plan is rebuilt only when this changes, or the image size does).
Key spatialKey(const FilmParams& p, const Key& bake, bool printPath) {
  Key k = bake;
  keyNum(k, printPath);
  keyNum(k, p.settings.seed);
  keyNum(k, p.settings.previewMode);
  keyNum(k, p.debug.deactivateSpatialEffects);
  keyNum(k, p.debug.deactivateStochasticEffects);
  keyNum(k, p.camera.lensBlurUm);
  keyNum(k, p.camera.filmFormatMm);
  keyDiffusion(k, p.camera.diffusionFilter);
  keyDiffusion(k, p.enlarger.diffusionFilter);
  const auto& g = p.filmRender.grain;
  for (double v : {double(g.active), double(g.sublayersActive), g.particleAreaUm2, g.blur, g.blurDyeCloudsUm,
                   g.microStructure[0], g.microStructure[1], double(g.nSubLayers)})
    keyNum(k, v);
  for (const auto* a : {&g.particleScale, &g.particleScaleLayers, &g.densityMin, &g.uniformity})
    for (double v : *a) keyNum(k, v);
  const auto& h = p.filmRender.halation;
  for (double v : {double(h.active), h.scatterAmount, h.scatterSpatialScale, h.halationAmount, h.halationSpatialScale,
                   h.boostEv, h.boostRange, h.protectEv, double(h.halationNBounces), h.halationBounceDecay,
                   double(h.halationRenormalize)})
    keyNum(k, v);
  for (const auto* a : {&h.scatterCoreUm, &h.scatterTailUm, &h.scatterTailWeight, &h.halationStrength, &h.halationFirstSigmaUm})
    for (double v : *a) keyNum(k, v);
  const auto& d = p.filmRender.dirCouplers;
  for (double v : {double(d.active), d.diffusionSizeUm, d.diffusionTailUm, d.diffusionTailWeight}) keyNum(k, v);
  for (double v : {double(p.printRender.glare.active), p.printRender.glare.percent, p.printRender.glare.roughness,
                   p.printRender.glare.blur, p.scanner.lensBlur, p.scanner.unsharpMask[0], p.scanner.unsharpMask[1]})
    keyNum(k, v);
  return k;
}

}  // namespace

struct FilmSimulator::Impl {
  std::shared_ptr<const FilmLibrary> lib;
  FilmParams params, digested, effective;
  Key key;
  std::unique_ptr<FilmModel> model;
  FilmTables tables;
  KView view{};
  std::map<Key, std::shared_ptr<RawLut>> rawCache;
  FilmSpatialHooks hooks;
  const FilmStock* film = nullptr;
  bool printPath = true;
  unsigned gen = 0;
  Key planKey;
  mutable Arena arena;
  mutable std::mutex runMutex;           // serialises renders that share the plan / arena
  mutable std::shared_mutex stateMutex;  // setParams / setHooks (exclusive) vs render* (shared)
  mutable SpatialPlan plan;
  mutable int planW = 0, planH = 0;
  mutable unsigned planGen = ~0u;
  const KernelSet* ks = &selectKernels();
  bool ready = false;

  // Rebuilds the spatial plan when the size or the spatial parameters changed. Caller holds runMutex.
  bool ensurePlan(int W, int H) const {
    if (planW == W && planH == H && planGen == gen) return true;
    std::string perr;
    SpatialPlan np = makeSpatialPlan(*film, digested, W, H, printPath, tables, &perr);
    if (!perr.empty()) return false;
    np.seed = digested.settings.seed;
    plan = std::move(np);
    planW = W;
    planH = H;
    planGen = gen;
    return true;
  }

  void makeView() {
    view = KView{};
    view.mIn = tables.mIn;
    view.rawN = tables.rawN;
    view.rawStride = tables.rawN + 2;
    view.raw = tables.raw.data();
    view.curveN = tables.curveN;
    view.curveX0 = tables.curveX0;
    view.curveInvDx = tables.curveInvDx;
    std::copy(tables.gamma, tables.gamma + 4, view.gamma);
    view.dn = tables.dn.data();
    view.d0 = tables.d0.data();
    std::copy(tables.dirQ, tables.dirQ + 12, view.dirQ);
    std::copy(tables.dirQ0, tables.dirQ0 + 4, view.dirQ0);
    view.lutN = tables.lutN;
    std::copy(tables.dmin, tables.dmin + 4, view.dmin);
    std::copy(tables.dscale, tables.dscale + 4, view.dscale);
    view.lut = tables.lut.data();
    view.lut1 = tables.lut1.data();
    view.paperCurve = tables.paperCurve.data();
    view.paperN = tables.paperN;
    view.paperX0 = tables.paperX0;
    view.paperInvDx = tables.paperInvDx;
    std::copy(tables.dminS, tables.dminS + 4, view.dminS);
    std::copy(tables.dscaleS, tables.dscaleS + 4, view.dscaleS);
    view.lut3 = tables.lut3.data();
  }
};

FilmSimulator::FilmSimulator(std::shared_ptr<const FilmLibrary> lib) : impl_(new Impl) { impl_->lib = std::move(lib); }
FilmSimulator::~FilmSimulator() = default;

const FilmParams& FilmSimulator::params() const { return impl_->params; }
const FilmParams& FilmSimulator::effectiveParams() const { return impl_->effective; }
void trimMemory() { detail::scratchGeneration().fetch_add(1, std::memory_order_relaxed); }

void FilmSimulator::setHooks(const FilmSpatialHooks& h) {
  std::unique_lock<std::shared_mutex> wr(impl_->stateMutex);
  impl_->hooks = h;
}
const char* FilmSimulator::kernelName() const { return impl_->ks->name; }

bool FilmSimulator::setParams(const FilmParams& p, std::string* err, FilmTimings* t) try {
  Impl& s = *impl_;
  std::unique_lock<std::shared_mutex> wr(s.stateMutex);
  const auto t0 = Clock::now();
  // Strong guarantee: everything is computed into locals and committed at the very end, so a failure (unknown
  // stock, invalid value, bake error, allocation failure) leaves the simulator exactly as it was.
  if (!p.validate(err)) return false;
  const FilmStock* film = s.lib->find(p.film);
  if (!film) {
    if (err) *err = "unknown film stock '" + p.film + "'";
    return false;
  }
  if (film->isPrintMedium()) {
    if (err) *err = "'" + p.film + "' is a print stock, not a camera film";
    return false;
  }
  const bool scan = p.io.scanFilm || p.paper == "none";
  const FilmStock* paper = nullptr;
  if (!scan) {
    paper = s.lib->find(p.paper);
    if (!paper || !paper->isPrintMedium()) {
      if (err) *err = "unknown print stock '" + p.paper + "'";
      return false;
    }
  }
  FilmParams eff = p;
  if (scan) eff.io.scanFilm = true;
  digest(eff, *film);
  const Key key = bakeKey(eff);
  const Key plKey = spatialKey(eff, key, !scan);
  const bool replan = plKey != s.planKey;
  if (s.ready && key == s.key) {
    FilmParams effective = eff;
    effective.enlarger = s.model->effective().enlarger;
    s.params = p;
    s.digested = std::move(eff);
    s.effective = std::move(effective);
    s.film = film;
    s.printPath = !scan;
    if (replan) {
      s.planKey = plKey;
      ++s.gen;
    }
    if (t) *t = FilmTimings{};
    return true;
  }
  // raw LUT (cached per film)
  const UpsampleTable& up = s.lib->upsample();
  if (up.n < 2) {
    if (err) *err = "film library has no spectral upsampling table";
    return false;
  }
  const auto tr = Clock::now();
  Spec iRef;
  if (!illuminant(film->referenceIlluminant, iRef)) {
    if (err) *err = "unknown reference illuminant " + film->referenceIlluminant;
    return false;
  }
  double wx, wy;
  xyOfXyz(illuminantXyz(iRef), wx, wy);
  const Mat3 mIn = mul(adaptationMatrix(cat16(), kD50x, kD50y, wx, wy), prophotoToXyz());
  const Key rk = rawKey(eff);
  auto it = s.rawCache.find(rk);
  if (it == s.rawCache.end()) {
    RawLutParams rp;
    rp.window = eff.settings.applyWindow;
    rp.surface = eff.settings.applySurface;
    rp.gamut = eff.io.inputGamutCompress;
    rp.knee[0] = eff.io.inputKnee.threshold;
    rp.knee[1] = eff.io.inputKnee.limit;
    rp.knee[2] = eff.io.inputKnee.power;
    for (int i = 0; i < 3; ++i) {
      rp.uv[i] = eff.camera.filterUv[i];
      rp.ir[i] = eff.camera.filterIr[i];
    }
    if (s.rawCache.size() > 6) s.rawCache.clear();
    it = s.rawCache.emplace(rk, bakeRawLut(*film, up, rp, iRef, wx, wy, mIn)).first;
  }
  const double rawMs = msSince(tr);
  auto model = std::make_unique<FilmModel>(*film, paper, eff, up, s.lib->cmax(), it->second);
  if (!model->ok()) {
    if (err) *err = model->error();
    return false;
  }
  BakeTimings bt;
  FilmTables tables = model->bake(&bt);
  // ---- commit (nothing below can fail) ----
  s.params = p;
  FilmParams effective = eff;
  effective.enlarger = model->effective().enlarger;
  s.effective = std::move(effective);
  s.digested = std::move(eff);
  s.film = film;
  s.printPath = !scan;
  s.planKey = plKey;
  ++s.gen;
  s.model = std::move(model);
  s.tables = std::move(tables);
  s.makeView();
  s.key = key;
  s.ready = true;
  if (t) {
    *t = FilmTimings{};
    t->bakeMs = msSince(t0);
    t->rawLutMs = rawMs;
    t->curvesMs = bt.curvesMs;
    t->lutMs = bt.lutMs;
    t->neutralMs = std::max(0.0, t->bakeMs - rawMs - bt.totalMs);
  }
  return true;
} catch (const std::exception& e) {
  if (err) *err = std::string("film bake failed: ") + e.what();
  return false;
} catch (...) {
  if (err) *err = "film bake failed";
  return false;
}

// ---- auto exposure --------------------------------------------------------------------------------------

float FilmSimulator::meterEv(const ImageF& img, const std::string& method) {
  const ImageF th = resizeToLongEdge(img, 256);
  const int W = th.width, H = th.height;
  if (W <= 0 || H <= 0) return 0.f;
  std::vector<double> Y(std::size_t(W) * H);
  for (std::size_t i = 0; i < Y.size(); ++i) {
    const float* p = &th.data[i * 3];
    Y[i] = 0.288 * p[0] + 0.7119 * p[1] + 0.0001 * p[2];
  }
  const double n = std::max(W, H);
  auto cx = [&](int j) { return (double(j) / W - 0.5) * W / n; };
  auto cy = [&](int i) { return (double(i) / H - 0.5) * H / n; };
  double meter = 0.184;
  if (method == "average") {
    double s = 0;
    for (double v : Y) s += v;
    meter = s / Y.size();
  } else if (method == "median") {
    std::vector<double> c = Y;
    std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end());
    meter = c[c.size() / 2];
    if (c.size() % 2 == 0) {
      const double hi = meter;
      std::nth_element(c.begin(), c.begin() + c.size() / 2 - 1, c.end());
      meter = 0.5 * (hi + c[c.size() / 2 - 1]);
    }
  } else if (method == "center_weighted") {
    double sw = 0, s = 0;
    for (int i = 0; i < H; ++i)
      for (int j = 0; j < W; ++j) {
        const double w = std::exp(-(cx(j) * cx(j) + cy(i) * cy(i)) / (2 * 0.2 * 0.2));
        sw += w;
        s += w * Y[std::size_t(i) * W + j];
      }
    meter = s / sw;
  } else if (method == "partial") {
    double s = 0;
    long c = 0;
    for (int i = 0; i < H; ++i)
      for (int j = 0; j < W; ++j)
        if (std::hypot(cx(j), cy(i)) < 0.15) {
          s += Y[std::size_t(i) * W + j];
          ++c;
        }
    if (c == 0) {
      for (double v : Y) s += v;
      c = long(Y.size());
    }
    meter = s / c;
  } else if (method == "matrix") {
    const int ch = H / 5, cw = W / 5;
    if (ch > 0 && cw > 0) {
      double sw = 0, s = 0;
      for (int gi = 0; gi < 5; ++gi)
        for (int gj = 0; gj < 5; ++gj) {
          double m = 0;
          for (int i = 0; i < ch; ++i)
            for (int j = 0; j < cw; ++j) m += Y[std::size_t(gi * ch + i) * W + gj * cw + j];
          m /= double(ch) * cw;
          const double dx = (gj - 2) / 2.0, dy = (gi - 2) / 2.0;
          const double w = 0.5 * (1 + std::cos(kPi * std::sqrt(dx * dx + dy * dy) / std::sqrt(2.0)));
          sw += w;
          s += w * m;
        }
      meter = s / sw;
    }
  } else if (method == "multi_zone") {
    const double lo[3] = {0, 0.05, 0.25}, hi[3] = {0.05, 0.25, 0.5}, wt[3] = {0.5, 0.3, 0.2};
    double s[3] = {0, 0, 0};
    long c[3] = {0, 0, 0};
    for (int i = 0; i < H; ++i)
      for (int j = 0; j < W; ++j) {
        const double r = std::hypot(cx(j), cy(i));
        for (int z = 0; z < 3; ++z)
          if (r >= lo[z] && r < hi[z]) {
            s[z] += Y[std::size_t(i) * W + j];
            ++c[z];
          }
      }
    double sw = 0, acc = 0;
    for (int z = 0; z < 3; ++z)
      if (c[z]) {
        acc += wt[z] * s[z] / c[z];
        sw += wt[z];
      }
    if (sw > 0) meter = acc / sw;
  } else if (method == "highlight_weighted") {
    double s2 = 0, s3 = 0, s1 = 0;
    for (double v : Y) {
      s1 += v;
      s2 += v * v;
      s3 += v * v * v;
    }
    meter = s2 < 1e-12 ? s1 / Y.size() : s3 / s2;
  }
  if (!(meter > 0) || !std::isfinite(meter)) return 0.f;
  return float(-std::log2(meter / 0.184));
}

// ---- run paths ------------------------------------------------------------------------------------------

bool FilmSimulator::render(const ImageF& in, ImageF& out, FilmTimings* t, const float* channelGain) const try {
  const Impl& s = *impl_;
  std::shared_lock<std::shared_mutex> rd(s.stateMutex);
  if (!s.ready || in.empty()) return false;
  KView view = s.view;
  float mInLocal[12];
  if (channelGain) {
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 4; ++i) mInLocal[c * 4 + i] = s.view.mIn[c * 4 + i] * std::max(0.f, channelGain[c]);
    view.mIn = mInLocal;
  }
  const auto t0 = Clock::now();
  double autoEv = 0;
  if (s.digested.camera.autoExposure) autoEv = meterEv(in, s.digested.camera.autoExposureMethod);
  const double autoMs = msSince(t0);
  const float scale = float(std::exp2(s.digested.camera.exposureCompensationEv + autoEv));
  if (out.width != in.width || out.height != in.height) out = ImageF(in.width, in.height);
  const int W = in.width, H = in.height;
  const KernelSet& ks = *s.ks;
  const auto t1 = Clock::now();
  bool spatial = false;
  if (!s.hooks.any()) {
    std::lock_guard<std::mutex> lock(s.runMutex);
    if (!s.ensurePlan(W, H)) return false;
    if (s.plan.any()) {
      spatial = true;
      SpatialRenderArgs ra{&view, &s.tables, &ks, &selectSpatialKernels(), &s.plan, &s.arena, scale,
                           s.digested.settings.tileWidth, s.digested.settings.tileHeight};
      SpatialTimings stm;
      renderSpatial(ra, in, out, &stm);
      if (t) {
        t->spExposureMs = stm.exposureMs;
        t->spCameraMs = stm.preMs;
        t->spHalationMs = stm.haloMs;
        t->spDirMs = stm.dirMs;
        t->spTileMs = stm.finalMs;
      }
    }
  }
  if (spatial) {
  } else if (!s.hooks.any()) {
    parallelFor(
        H, [&](int y) { ks.block(view, in.row(y), out.row(y), W, scale); }, 4);
  } else {
    FilmSpatialContext ctx;
    ctx.width = W;
    ctx.height = H;
    ctx.pixelSizeUm = s.digested.camera.filmFormatMm * 1000.0 / std::max(W, H);
    ctx.params = &s.digested;
    ImageF e(W, H), q(W, H);
    parallelFor(H, [&](int y) { ks.exposure(view, in.row(y), e.row(y), W, scale); }, 4);
    if (s.hooks.exposure) s.hooks.exposure(s.hooks.user, e.data.data(), ctx);
    ImageF x(W, H);
    parallelFor(H, [&](int y) { ks.logE(view, e.row(y), x.row(y), W); }, 4);
    parallelFor(H, [&](int y) { ks.develop1(view, x.row(y), q.row(y), W); }, 4);
    if (s.hooks.inhibitor) s.hooks.inhibitor(s.hooks.user, q.data.data(), ctx);
    parallelFor(H, [&](int y) { ks.develop2(view, x.row(y), q.row(y), e.row(y), W); }, 4);  // e reused for d
    if (s.hooks.density) s.hooks.density(s.hooks.user, e.data.data(), ctx);
    parallelFor(H, [&](int y) { ks.printScan(view, e.row(y), out.row(y), W); }, 4);
    if (s.hooks.scan) s.hooks.scan(s.hooks.user, out.data.data(), ctx);
  }
  if (t) {
    t->autoExposureMs = autoMs;
    t->autoExposureEv = autoEv;
    t->runMs = msSince(t1);
  }
  return true;
} catch (...) {
  return false;  // allocation failure or an exception from a worker: nothing is left half-owned
}

bool FilmSimulator::renderRows(const ImageF& in, int w, int h, const float* channelGain, RowSource src, void* srcUser,
                               RowSink sink, void* user) const try {
  const Impl& s = *impl_;
  std::shared_lock<std::shared_mutex> rd(s.stateMutex);
  if (!s.ready || in.empty() || !sink || s.hooks.any()) return false;
  const int W = src ? w : in.width, H = src ? h : in.height;
  if (W <= 0 || H <= 0) return false;
  KView view = s.view;
  float mInLocal[12];
  if (channelGain) {
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 4; ++i) mInLocal[c * 4 + i] = s.view.mIn[c * 4 + i] * std::max(0.f, channelGain[c]);
    view.mIn = mInLocal;
  }
  double autoEv = 0;
  if (s.digested.camera.autoExposure) autoEv = meterEv(in, s.digested.camera.autoExposureMethod);
  const float scale = float(std::exp2(s.digested.camera.exposureCompensationEv + autoEv));
  {
    std::lock_guard<std::mutex> lock(s.runMutex);
    if (!s.ensurePlan(W, H)) return false;
    if (s.plan.any()) return false;  // spatial effects need the planar passes: use render()
  }
  const KernelSet& ks = *s.ks;
  // Row bands through a small thread-local float tile (stays in L2): no full-frame float output, the consumer
  // (display stages + 8-bit encode) runs on the hot tile.
  constexpr int kBandRows = 4;
  const int bands = (H + kBandRows - 1) / kBandRows;
  parallelFor(
      bands,
      [&](int b) {
        thread_local Scratch tileS, fetchedS;
        const int y0 = b * kBandRows, y1 = std::min(H, y0 + kBandRows);
        const std::size_t n = std::size_t(y1 - y0) * W * 3;
        float* const tile = tileS.get(n);
        const float* rows = nullptr;
        if (src) {
          float* const fetched = fetchedS.get(n);
          for (int y = y0; y < y1; ++y) src(srcUser, y, fetched + std::size_t(y - y0) * W * 3);
          rows = fetched;
        } else {
          rows = in.row(y0);
        }
        ks.block(view, rows, tile, int(std::size_t(y1 - y0) * W), scale);
        for (int y = y0; y < y1; ++y) sink(user, y, tile + std::size_t(y - y0) * W * 3, W);
      },
      1);
  return true;
} catch (...) {
  return false;
}

bool FilmSimulator::renderStaged(const ImageF& in, ImageF& out, FilmTimings* t) const try {
  const Impl& s = *impl_;
  std::shared_lock<std::shared_mutex> rd(s.stateMutex);
  if (!s.ready || in.empty()) return false;
  const float scale = float(std::exp2(s.digested.camera.exposureCompensationEv));
  if (out.width != in.width || out.height != in.height) out = ImageF(in.width, in.height);
  const int W = in.width, H = in.height;
  const KernelSet& ks = *s.ks;
  std::atomic<long long> nsE{0}, nsD{0}, nsP{0};
  const auto t0 = Clock::now();
  constexpr int kBlockRows = 4;
  const int blocks = (H + kBlockRows - 1) / kBlockRows;
  parallelFor(
      blocks,
      [&](int b) {
        thread_local Scratch eS, xS, qS, dS;
        const int y0 = b * kBlockRows, y1 = std::min(H, y0 + kBlockRows);
        const std::size_t n = std::size_t(y1 - y0) * W;
        float* const e = eS.get(n * 3);
        float* const x = xS.get(n * 3);
        float* const q = qS.get(n * 3);
        float* const d = dS.get(n * 3);
        auto tick = [] { return Clock::now(); };
        auto ns = [](Clock::time_point a, Clock::time_point c) {
          return std::chrono::duration_cast<std::chrono::nanoseconds>(c - a).count();
        };
        const float* src = in.row(y0);
        auto a = tick();
        ks.exposure(s.view, src, e, int(n), scale);
        tick();
        ks.logE(s.view, e, x, int(n));
        auto c = tick();
        ks.develop1(s.view, x, q, int(n));
        ks.develop2(s.view, x, q, d, int(n));
        auto d2 = tick();
        ks.printScan(s.view, d, out.row(y0), int(n));
        auto e2 = tick();
        nsE += ns(a, c);
        nsD += ns(c, d2);
        nsP += ns(d2, e2);
      },
      1);
  if (t) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    t->exposureMs = double(nsE.load()) * 1e-6 / hw;  // aggregate CPU time spread over the hardware threads
    t->developMs = double(nsD.load()) * 1e-6 / hw;
    t->printScanMs = double(nsP.load()) * 1e-6 / hw;
    t->runMs = msSince(t0);
  }
  return true;
} catch (...) {
  return false;
}

void FilmSimulator::renderPixelDirect(const double rgb[3], double out[3]) const {
  const Vec3 o = impl_->model->render({rgb[0], rgb[1], rgb[2]});
  for (int i = 0; i < 3; ++i) out[i] = o[i];
}

void FilmSimulator::renderPixelBaked(const float rgb[3], float out[3]) const {
  impl_->ks->single(impl_->view, rgb, out, float(std::exp2(impl_->digested.camera.exposureCompensationEv)));
}

void FilmSimulator::directIntermediates(const double rgb[3], double logE[3], double density[3]) const {
  const FilmModel& m = *impl_->model;
  const Vec3 x = m.logExposure(m.exposure({rgb[0], rgb[1], rgb[2]}));
  const Vec3 d = m.develop(x);
  for (int i = 0; i < 3; ++i) {
    logE[i] = x[i];
    density[i] = d[i];
  }
}

}  // namespace dapple::film
