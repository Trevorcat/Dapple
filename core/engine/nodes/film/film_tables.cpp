// Film table nodes (FilmTableRaw / FilmTableCurves / FilmTablePrintScan), the shared resolve function and the plan cache.
// The model code (FilmModel) is the existing double-precision bake; the three nodes call its three bake parts, so the
// tables are bit-identical to the ones FilmSimulator::setParams bakes (tested), but cached under three narrower keys:
// dragging print warmth / tint re-bakes only the print LUT (about 10 ms), on a model whose neutral solve is reused.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <list>
#include <map>
#include <mutex>

#include "engine/nodes/film/film_node_internal.h"

namespace dapple::engine::filmnode {

namespace {

constexpr float kWarmthFilterUnits = 12.f;  // renderer.cpp: enlarger filtration at warmth / tint = +-100

std::uint32_t mix32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

// Same construction as renderer.cpp makeFilmParams(); `quality`: 0 off, 1 interactive (preview mode), 2 full.
dapple::film::FilmParams makeParams(const FilmResolved& r, int quality) {
  using namespace dapple::film;
  FilmParams fp;
  fp.film = r.stock->stock;
  fp.paper = r.scan ? std::string("none") : r.paper->stock;
  fp.camera.autoExposure = false;
  fp.camera.filmFormatMm *= r.formatScale;
  fp.enlarger.yFilterShift = -r.warmth / 100.f * kWarmthFilterUnits;
  fp.enlarger.mFilterShift = -r.tint / 100.f * kWarmthFilterUnits;
  const double g = std::clamp(r.grainPct / 100.f, 0.f, 1.f);
  if (g <= 0) {
    fp.filmRender.grain.active = false;
  } else {
    fp.filmRender.grain.particleAreaUm2 *= g * g;  // grain amplitude ~ sqrt(particle area)
  }
  const double h = std::clamp(r.halationPct / 100.f, 0.f, 1.f);
  fp.filmRender.halation.halationAmount = h;
  fp.filmRender.halation.scatterAmount = h;
  if (quality == 0) fp.disableSpatialAndStochastic();
  else if (quality == 1) fp.settings.previewMode = true;
  return fp;
}

// ---- model + raw LUT caches --------------------------------------------------------------------------------------------------------
struct ModelEntry {
  const void* lib = nullptr;
  const void* stock = nullptr;
  const void* paper = nullptr;  // null = scan model
  bool off = false;             // spatial + stochastic effects off (the print LUT then has no mean glare baked in)
  std::unique_ptr<fd::FilmModel> model;
  std::mutex m;
  std::string error;
};
struct RawEntry {
  const void* lib;
  const void* stock;
  std::shared_ptr<fd::RawLut> lut;
};
struct Caches {
  std::mutex m;
  std::list<std::shared_ptr<ModelEntry>> models;  // most recent first
  std::list<RawEntry> raws;
};
Caches& caches() {
  static Caches c;
  return c;
}

std::shared_ptr<fd::RawLut> rawLutFor(const dapple::film::FilmLibrary& lib, const dapple::film::FilmStock& film,
                                      const dapple::film::FilmParams& eff, std::string* err) {
  Caches& C = caches();
  {
    std::lock_guard<std::mutex> lock(C.m);
    for (auto& e : C.raws)
      if (e.lib == &lib && e.stock == &film) return e.lut;
  }
  const dapple::film::UpsampleTable& up = lib.upsample();
  if (up.n < 2) {
    if (err) *err = "film library has no spectral upsampling table";
    return nullptr;
  }
  fd::Spec iRef;
  if (!fd::illuminant(film.referenceIlluminant, iRef)) {
    if (err) *err = "unknown reference illuminant " + film.referenceIlluminant;
    return nullptr;
  }
  double wx, wy;
  fd::xyOfXyz(fd::illuminantXyz(iRef), wx, wy);
  const fd::Mat3 mIn = fd::mul(fd::adaptationMatrix(fd::cat16(), fd::kD50x, fd::kD50y, wx, wy), fd::prophotoToXyz());
  fd::RawLutParams rp;
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
  std::shared_ptr<fd::RawLut> lut = fd::bakeRawLut(film, up, rp, iRef, wx, wy, mIn);
  std::lock_guard<std::mutex> lock(C.m);
  C.raws.push_front({&lib, &film, lut});
  while (C.raws.size() > 6) C.raws.pop_back();
  return lut;
}

// The model of (stock, paper) with neutral filtration solved and zero filtration shifts. paper == null: scan model.
std::shared_ptr<ModelEntry> modelFor(const FilmResolved& r, bool withPaper, std::string* err) {
  Caches& C = caches();
  const void* paperKey = withPaper && !r.scan ? static_cast<const void*>(r.paper) : nullptr;
  std::shared_ptr<ModelEntry> e;
  {
    std::lock_guard<std::mutex> lock(C.m);
    for (auto it = C.models.begin(); it != C.models.end(); ++it)
      if ((*it)->lib == r.lib && (*it)->stock == r.stock && (*it)->paper == paperKey && (*it)->off == (r.quality == 0)) {
        e = *it;
        C.models.splice(C.models.begin(), C.models, it);
        break;
      }
  }
  if (e) return e;
  auto ne = std::make_shared<ModelEntry>();
  ne->lib = r.lib;
  ne->stock = r.stock;
  ne->paper = paperKey;
  ne->off = r.quality == 0;
  dapple::film::FilmParams eff = makeParams(r, r.quality == 0 ? 0 : 2);
  eff.enlarger.yFilterShift = eff.enlarger.mFilterShift = 0;
  const bool scan = !paperKey;
  if (scan) {
    eff.paper = "none";
    eff.io.scanFilm = true;
  }
  fd::digestParams(eff, *r.stock);
  auto raw = rawLutFor(*r.lib, *r.stock, eff, err);
  if (!raw) return nullptr;
  ne->model = std::make_unique<fd::FilmModel>(*r.stock, scan ? nullptr : r.paper, eff, r.lib->upsample(), r.lib->cmax(), raw);
  if (!ne->model->ok()) {
    if (err) *err = ne->model->error();
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(C.m);
  C.models.push_front(ne);
  while (C.models.size() > 4) C.models.pop_back();
  return ne;
}

bool usable(const FilmResolved& r) { return r.lib && r.stock && (r.scan || r.paper); }

}  // namespace

// ---- resolve -------------------------------------------------------------------------------------------------------------------------
void resolveFilm(const ParamSet& ps, const FrameGeom& geom, FilmResolved& out) {
  out = FilmResolved{};
  const EditParams& e = ps.edit();
  const SessionCtx& ctx = ps.ctx();
  out.lib = ctx.filmLib;
  out.w = geom.w;
  out.h = geom.h;
  if (!ctx.filmLib || e.filmStock.empty()) return;
  const dapple::film::FilmStock* s = ctx.filmLib->find(e.filmStock);
  if (!s || s->isPrintMedium()) return;
  out.stock = s;
  std::string paper = e.filmPaper;
  if (paper.empty() || (paper != "none" && !(ctx.filmLib->find(paper) && ctx.filmLib->find(paper)->isPrintMedium())))
    paper = dapple::film::defaultPaperFor(*ctx.filmLib, e.filmStock);
  if (paper == "none") {
    out.scan = 1;
  } else {
    out.paper = ctx.filmLib->find(paper);
    if (!out.paper) out.scan = 1;
  }
  float g[3] = {1.f, 1.f, 1.f};
  if (ctx.gainFn) ctx.gainFn(ctx.gainUser, e, g);
  const float evGain = std::exp2(e.filmExposure);
  for (int c = 0; c < 3; ++c) out.gain[c] = g[c] * evGain;
  out.warmth = e.filmWarmth;
  out.tint = e.filmTint;
  out.grainPct = e.filmGrain;
  out.halationPct = e.filmHalation;
  out.pitch = geom.scale > 0.f ? geom.scale : 1.f;
  const int sw = ctx.srcW > 0 ? ctx.srcW : geom.w, sh = ctx.srcH > 0 ? ctx.srcH : geom.h;
  out.formatScale = out.pitch * float(std::max(geom.w, geom.h)) / float(std::max(sw, sh));
  out.fullLongEdge = ctx.fullLongEdge;
  out.frameIndex = ctx.frameIndex;
  out.seed = ctx.frameIndex ? filmSeed(ctx.frameIndex) : 0u;
  out.quality = std::uint8_t(ps.inputs().quality);
  out.keepGrain = ps.inputs().keepGrain ? 1 : 0;
}

// ---- tables -------------------------------------------------------------------------------------------------------------------------
Status bakeRaw(const void* resolved, std::span<const void* const>, void** payloadOut) try {
  const FilmResolved& r = *static_cast<const FilmResolved*>(resolved);
  if (!usable(r)) return Status::Unsupported;
  std::string err;
  auto m = modelFor(r, false, &err);
  if (!m) return Status::Internal;
  auto t = std::make_unique<RawTable>();
  {
    std::lock_guard<std::mutex> lock(m->m);
    m->model->bakeInput(t->t);
  }
  *payloadOut = static_cast<PayloadBase*>(t.release());
  return Status::Ok;
} catch (const std::bad_alloc&) {
  return Status::OutOfMemory;
} catch (...) {
  return Status::Internal;
}

Status bakeCurves(const void* resolved, std::span<const void* const>, void** payloadOut) try {
  const FilmResolved& r = *static_cast<const FilmResolved*>(resolved);
  if (!usable(r)) return Status::Unsupported;
  std::string err;
  auto m = modelFor(r, false, &err);  // the curves do not depend on the paper: the scan model is enough
  if (!m) return Status::Internal;
  auto t = std::make_unique<CurvesTable>();
  {
    std::lock_guard<std::mutex> lock(m->m);
    m->model->bakeCurves(t->t);
  }
  *payloadOut = static_cast<PayloadBase*>(t.release());
  return Status::Ok;
} catch (const std::bad_alloc&) {
  return Status::OutOfMemory;
} catch (...) {
  return Status::Internal;
}

Status bakePrintScan(const void* resolved, std::span<const void* const>, void** payloadOut) try {
  const FilmResolved& r = *static_cast<const FilmResolved*>(resolved);
  if (!usable(r)) return Status::Unsupported;
  std::string err;
  auto m = modelFor(r, true, &err);
  if (!m) return Status::Internal;
  auto t = std::make_unique<PrintTable>();
  {
    const dapple::film::FilmParams fp = makeParams(r, r.quality == 0 ? 0 : 2);
    std::lock_guard<std::mutex> lock(m->m);
    m->model->rebalance(fp.enlarger.mFilterShift, fp.enlarger.yFilterShift);
    m->model->bakePrintScan(t->t);
  }
  *payloadOut = static_cast<PayloadBase*>(t.release());
  return Status::Ok;
} catch (const std::bad_alloc&) {
  return Status::OutOfMemory;
} catch (...) {
  return Status::Internal;
}

void freePayload(void* p) { delete static_cast<PayloadBase*>(p); }

bool bakeFilmTables(const FilmResolved& r, FilmTablesHandle& out, std::string* err, FilmTimingsNodes* t) {
  const auto t0 = std::chrono::steady_clock::now();
  void *a = nullptr, *b = nullptr, *c = nullptr;
  if (bakeRaw(&r, {}, &a) != Status::Ok || bakeCurves(&r, {}, &b) != Status::Ok || bakePrintScan(&r, {}, &c) != Status::Ok) {
    if (err) *err = "film table bake failed";
    if (a) freePayload(a);
    if (b) freePayload(b);
    if (c) freePayload(c);
    return false;
  }
  out.raw = std::shared_ptr<const RawTable>(payloadAs<RawTable>(a), [](const RawTable* p) { freePayload(const_cast<RawTable*>(p)); });
  out.curves = std::shared_ptr<const CurvesTable>(payloadAs<CurvesTable>(b), [](const CurvesTable* p) { freePayload(const_cast<CurvesTable*>(p)); });
  out.print = std::shared_ptr<const PrintTable>(payloadAs<PrintTable>(c), [](const PrintTable* p) { freePayload(const_cast<PrintTable*>(p)); });
  if (t) t->tablesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

// ---- process-wide table cache (Renderer-facing entry) ------------------------------------------------------------------------------------
namespace {
template <class T>
struct TableLru {
  std::mutex m;
  std::list<std::pair<Hash128, std::shared_ptr<const T>>> l;
};
template <class T>
std::shared_ptr<const T> cachedTable(TableLru<T>& c, const Hash128& key, Status (*bake)(const void*, std::span<const void* const>, void**),
                                     const FilmResolved& r) {
  {
    std::lock_guard<std::mutex> lock(c.m);
    for (auto it = c.l.begin(); it != c.l.end(); ++it)
      if (it->first == key) {
        c.l.splice(c.l.begin(), c.l, it);
        return c.l.front().second;
      }
  }
  void* p = nullptr;
  if (bake(&r, {}, &p) != Status::Ok) return nullptr;
  std::shared_ptr<const T> sp(payloadAs<T>(p), [](const T* q) { freePayload(const_cast<T*>(q)); });
  std::lock_guard<std::mutex> lock(c.m);
  c.l.emplace_front(key, sp);
  while (c.l.size() > 6) c.l.pop_back();
  return sp;
}
}  // namespace

bool tablesFor(const FilmResolved& r, FilmTablesHandle& out) {
  static TableLru<RawTable> raws;
  static TableLru<CurvesTable> curves;
  static TableLru<PrintTable> prints;
  Hasher hs;
  hs.u64(std::uintptr_t(r.lib));
  hs.u64(std::uintptr_t(r.stock));
  hs.i32(r.quality == 0);
  const Hash128 ks = hs.finish();
  Hasher hp;
  hp.hash(ks);
  hp.u64(std::uintptr_t(r.paper));
  hp.i32(r.scan);
  hp.i32(r.quality == 0);
  hp.f32(r.warmth);
  hp.f32(r.tint);
  out.raw = cachedTable(raws, ks, bakeRaw, r);
  out.curves = cachedTable(curves, ks, bakeCurves, r);
  out.print = cachedTable(prints, hp.finish(), bakePrintScan, r);
  return out.raw && out.curves && out.print;
}

// ---- KView --------------------------------------------------------------------------------------------------------------------------
fd::KView viewOf(const RawTable* raw, const CurvesTable* cur, const PrintTable* print, const float* gain, float mInStorage[12]) {
  fd::KView v{};
  if (raw) {
    const fd::FilmTables& t = raw->t;
    v.mIn = t.mIn;
    v.rawN = t.rawN;
    v.rawStride = t.rawN + 2;
    v.raw = t.raw.data();
    if (gain) {
      for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 4; ++i) mInStorage[c * 4 + i] = t.mIn[c * 4 + i] * std::max(0.f, gain[c]);
      v.mIn = mInStorage;
    }
  }
  if (cur) {
    const fd::FilmTables& t = cur->t;
    v.curveN = t.curveN;
    v.curveX0 = t.curveX0;
    v.curveInvDx = t.curveInvDx;
    std::copy(t.gamma, t.gamma + 4, v.gamma);
    v.dn = t.dn.data();
    v.d0 = t.d0.data();
    std::copy(t.dirQ, t.dirQ + 12, v.dirQ);
    std::copy(t.dirQ0, t.dirQ0 + 4, v.dirQ0);
  }
  if (print) {
    const fd::FilmTables& t = print->t;
    v.lutN = t.lutN;
    std::copy(t.dmin, t.dmin + 4, v.dmin);
    std::copy(t.dscale, t.dscale + 4, v.dscale);
    v.lut = t.lut.data();
    v.lut1 = t.lut1.data();
    v.paperCurve = t.paperCurve.data();
    v.paperN = t.paperN;
    v.paperX0 = t.paperX0;
    v.paperInvDx = t.paperInvDx;
    std::copy(t.dminS, t.dminS + 4, v.dminS);
    std::copy(t.dscaleS, t.dscaleS + 4, v.dscaleS);
    v.lut3 = t.lut3.data();
  }
  return v;
}

// ---- plan cache ---------------------------------------------------------------------------------------------------------------------
namespace {
Hash128 planKey(const FilmResolved& r) {
  Hasher h;
  h.u64(std::uintptr_t(r.lib));
  h.u64(std::uintptr_t(r.stock));
  h.u64(std::uintptr_t(r.paper));
  h.f32(r.grainPct);
  h.f32(r.halationPct);
  h.f32(r.formatScale);
  h.i32(r.w);
  h.i32(r.h);
  h.i32(r.fullLongEdge);
  h.u64(r.seed);
  h.i32(r.quality);
  h.i32(r.keepGrain);
  h.i32(r.scan);
  return h.finish();
}
struct PlanCache {
  std::mutex m;
  std::list<std::pair<Hash128, std::shared_ptr<const FilmPlanData>>> l;
};
PlanCache& planCache() {
  static PlanCache c;
  return c;
}
}  // namespace

std::shared_ptr<const FilmPlanData> planOf(const FilmResolved& r) {
  PlanCache& C = planCache();
  const Hash128 key = planKey(r);
  {
    std::lock_guard<std::mutex> lock(C.m);
    for (auto it = C.l.begin(); it != C.l.end(); ++it)
      if (it->first == key) {
        C.l.splice(C.l.begin(), C.l, it);
        return C.l.front().second;
      }
  }
  auto pd = std::make_shared<FilmPlanData>();
  if (!usable(r) || r.w <= 0 || r.h <= 0) {
    pd->error = "film not available";
    return pd;
  }
  using namespace dapple::film;
  FilmParams base = makeParams(r, 2);
  FilmParams eff = makeParams(r, r.quality);
  if (r.scan) {
    base.io.scanFilm = true;
    eff.io.scanFilm = true;
  }
  fd::digestParams(eff, *r.stock);
  // Q8: at the interactive tier the grain stays on while a grain-family slider moves (everything else keeps its preview
  // simplification); the grain is then planned scale-aware (see makeSpatialPlan)
  if (r.quality == 1 && r.keepGrain) eff.filmRender.grain = base.filmRender.grain;
  eff.settings.seed = r.seed;
  pd->params = eff;
  std::string perr;
  static const fd::FilmTables noTables;
  fd::GrainFullRes full;
  if (r.fullLongEdge > 0) {
    // the final render this frame previews: unscaled film frame, full quality (its scanner stage shapes the grain)
    FilmParams fullEff = base;
    fd::digestParams(fullEff, *r.stock);
    full.pixelUm = FilmParams().camera.filmFormatMm * 1000.0 / double(r.fullLongEdge);
    full.scanBlur = fullEff.scanner.lensBlur;
    full.glareBlur = fullEff.printRender.glare.blur;
    if (fullEff.scanner.unsharpMask[0] > 0 && fullEff.scanner.unsharpMask[1] > 0) {
      full.unsharpSigma = fullEff.scanner.unsharpMask[0];
      full.unsharpAmount = fullEff.scanner.unsharpMask[1];
    }
  }
  pd->plan = fd::makeSpatialPlan(*r.stock, eff, r.w, r.h, !r.scan, noTables, &perr, full.pixelUm > 0 ? &full : nullptr);
  if (!perr.empty()) pd->error = perr;
  pd->plan.seed = eff.settings.seed;
  const fd::SpatialPlan& pl = pd->plan;
  pd->split = pl.enlargerDiffusion;
  pd->scanStage = pl.scanStage();
  pd->scan.set(pl.scanBlur);
  if (pl.unsharpSigma > 0 && pl.unsharpAmount > 0) pd->unsharp.set(pl.unsharpSigma);
  pd->Hs = pd->scan.r + pd->unsharp.r;
  const auto& gp = pl.grain;
  if (gp.active) {
    pd->grain.set(gp.blurSigma);
    pd->Hg = pd->grain.r;
    for (int ch = 0; ch < 3; ++ch)
      for (int l = 0; l < gp.ch[ch].layers; ++l) {
        pd->dye[ch][l].set(gp.ch[ch].dyeSigma[l]);
        pd->Hd = std::max(pd->Hd, pd->dye[ch][l].r);
      }
    if (gp.microActive) {
      pd->micro.set(gp.microBlur > 0.4 ? gp.microBlur : 0.0);
      pd->Hd = std::max(pd->Hd, pd->micro.r);
    }
  }
  if (pl.glare) {
    pd->glare.set(pl.glareBlur);
    pd->rgl = pd->glare.r;
  }
  auto stream = [&](std::uint32_t id) { return mix32(pl.seed * 0x9E3779B1u + id * 0x85EBCA6Bu + 0x165667B1u); };
  for (int c = 0; c < 3; ++c) {
    pd->streamGrain[c] = stream(1 + c);
    pd->streamMicro[c] = stream(11 + c);
  }
  pd->streamGlare = stream(21);
  pd->dirHalf = pl.dirDiffuse && fd::mixtureFitsHalf(pl.dir[0]) && fd::mixtureFitsHalf(pl.dir[1]) && fd::mixtureFitsHalf(pl.dir[2]);
  std::lock_guard<std::mutex> lock(C.m);
  C.l.emplace_front(key, pd);
  while (C.l.size() > 12) C.l.pop_back();
  return pd;
}

FilmHalos halosOf(const FilmResolved& r) {
  FilmHalos h;
  const auto pd = planOf(r);
  const fd::SpatialPlan& p = pd->plan;
  h.grainDye = pd->Hd;
  h.grainBlur = pd->Hg;
  h.scan = pd->Hs;
  h.boost = p.boost;
  h.enlargerSplit = pd->split;
  h.preMix = p.preMix;
  h.haloMix = p.haloMix;
  h.dirDiffuse = p.dirDiffuse;
  h.dirHalf = pd->dirHalf;
  h.grain = p.grain.active;
  h.glare = p.glare;
  h.scanStage = pd->scanStage;
  return h;
}

}  // namespace dapple::engine::filmnode
