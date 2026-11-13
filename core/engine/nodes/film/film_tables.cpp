// Film table nodes (FilmTableRaw / FilmTableCurves / FilmTablePrintScan), the shared resolve function and the plan cache.
// The model code (FilmModel) is the existing double-precision bake; the three nodes call its three bake parts, so the
// tables are bit-identical to the ones FilmSimulator::setParams bakes (tested), but cached under three narrower keys:
// dragging print warmth / tint re-bakes only the print LUT (about 10 ms), on a model whose neutral solve is reused.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "parallel.h"
#include "determinism.h"
#include "engine/nodes/film/film_async.h"

#include "dapple/film_match.h"
#include "engine/nodes/film/film_node_internal.h"

namespace dapple::engine::filmnode {

namespace {

constexpr float kWarmthFilterUnits = 12.f;  // renderer.cpp: enlarger filtration at warmth / tint = +-100
constexpr int kCoarseLutN = 17;           // nodes per axis of the print table baked while a print-table slider moves (33 otherwise)

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
  dapple::applyFilmPaperMapping(fp, r.black);  // as renderer.cpp
  fp.camera.filmFormatMm *= r.formatScale;
  fp.enlarger.yFilterShift = -r.warmth / 100.f * kWarmthFilterUnits;
  fp.enlarger.mFilterShift = -r.tint / 100.f * kWarmthFilterUnits;
  const double g = std::clamp(r.grainPct / 100.f, 0.f, 2.f);
  if (g <= 0) {
    fp.filmRender.grain.active = false;
  } else {
    fp.filmRender.grain.particleAreaUm2 *= g * g;  // grain amplitude ~ sqrt(particle area)
  }
  const double h = std::clamp(r.halationPct / 100.f, 0.f, 2.f);
  fp.filmRender.halation.halationAmount = h;  // as renderer.cpp
  fp.filmRender.halation.scatterAmount = std::min(h, 1.0);
  fp.filmRender.halation.boostEv = r.fx.boostEv;
  fp.filmRender.halation.boostRange = r.fx.boostRange;
  fp.filmRender.halation.protectEv = r.fx.protectEv;
  if (r.fx.enlargerFilter >= 1 && r.fx.enlargerFilter <= 4) {
    DiffusionFilterParams& d = fp.enlarger.diffusionFilter;
    d.active = true;
    d.filterFamily = kDiffusionFamilies[r.fx.enlargerFilter - 1];
    d.strength = r.fx.enlStrength;
    d.spatialScale = r.fx.enlScale;
    d.haloWarmth = r.fx.enlHaloWarmth;
    d.coreIntensity = r.fx.enlCoreIntensity;
    d.coreSize = r.fx.enlCoreSize;
    d.haloIntensity = r.fx.enlHaloIntensity;
    d.haloSize = r.fx.enlHaloSize;
    d.bloomIntensity = r.fx.enlBloomIntensity;
    d.bloomSize = r.fx.enlBloomSize;
  }
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
  bool split = false;           // enlarger diffusion filter active: the print tables carry the split-path tables
  bool bw = false;              // paper-to-screen white / black mapping (filmPaperMappingEnabled at creation)
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
  const bool wantSplit = paperKey && r.fx.enlargerFilter != 0 && r.quality != 0;
  std::shared_ptr<ModelEntry> e;
  {
    std::lock_guard<std::mutex> lock(C.m);
    for (auto it = C.models.begin(); it != C.models.end(); ++it)
      if ((*it)->lib == r.lib && (*it)->stock == r.stock && (*it)->paper == paperKey && (*it)->off == (r.quality == 0) &&
          (*it)->split == wantSplit && (*it)->bw == dapple::filmPaperMappingEnabled()) {
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
  ne->split = wantSplit;
  ne->bw = dapple::filmPaperMappingEnabled();
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
  const float evGain = std::exp2(e.filmExposure + e.filmKeyEv);
  for (int c = 0; c < 3; ++c) out.gain[c] = g[c] * evGain;
  out.printEv = e.filmPrintEv;
  out.black = e.filmBlack;
  {  // film white balance: the assumed scene illuminant and the degree of adaptation (D = 1: the film's own balance, no effect)
    double sx, sy, sd;
    dapple::filmInputAdaptation(e, &sx, &sy, &sd);
    out.sceneX = float(sx), out.sceneY = float(sy), out.adapt = float(sd);
  }
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
    // film white balance: only the 3x3 in front of the (shared, unchanged) raw LUT depends on the scene illuminant and D
    m->model->setInputAdaptation(r.adapt < 1.f ? r.sceneX : 0.0, r.adapt < 1.f ? r.sceneY : 0.0, r.adapt);
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

Status bakePrintScanN(const void* resolved, int lutN, void** payloadOut) try {
  const FilmResolved& r = *static_cast<const FilmResolved*>(resolved);
  if (!usable(r)) return Status::Unsupported;
  std::string err;
  auto m = modelFor(r, true, &err);
  if (!m) return Status::Internal;
  auto t = std::make_unique<PrintTable>();
  {
    const dapple::film::FilmParams fp = makeParams(r, r.quality == 0 ? 0 : 2);
    std::lock_guard<std::mutex> lock(m->m);
    m->model->setBlackMix(fp.scanner.blackMix);
    m->model->rebalance(fp.enlarger.mFilterShift, fp.enlarger.yFilterShift);
    m->model->setPrintExposure(std::exp2(-double(std::clamp(r.printEv, -9.f, 9.f))));
    m->model->bakePrintScan(t->t, lutN);
  }
  *payloadOut = static_cast<PayloadBase*>(t.release());
  return Status::Ok;
} catch (const std::bad_alloc&) {
  return Status::OutOfMemory;
} catch (...) {
  return Status::Internal;
}

Status bakePrintScan(const void* resolved, std::span<const void* const>, void** payloadOut) { return bakePrintScanN(resolved, 0, payloadOut); }
// Coarse print table for interactive frames while a print-table slider moves (docs/engine/pipeline-architecture.md section 26)
Status bakePrintScanCoarse(const void* resolved, std::span<const void* const>, void** payloadOut) { return bakePrintScanN(resolved, kCoarseLutN, payloadOut); }

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

// ---- film tables on disk (wave 9): "warm from disk" ---------------------------------------------------------------------------------------------
// The raw / curve / print tables of a stock are baked on the frame thread or the background pool the first time a process meets them (100-300 ms on a
// phone per stock). A baked table is also written to <cache dir>/film_tables/<key>.bin from a writer thread (never on the frame thread), and the next
// process finds it there. File = header (magic, format version, 128-bit key, payload length, FNV-1a checksum) + the fields of fd::FilmTables, floats as
// stored. The key hashes: the content of the loaded pack (libraryContentHash), the build salt (app version + Build.FINGERPRINT), the format version, the
// table kind and everything the table depends on (stock / paper ids, warmth, tint, print EV, black, mapping, split path, adaptation, LUT size). A file that
// fails any check (size, magic, version, key, checksum) is deleted and the table is baked as before. Writes go to a per-process temporary name and are
// renamed into place; the directory is kept under a size cap (default 64 MB, DAPPLE_FILM_DISK_CAP_MB) by deleting the least recently used files (a load
// touches the file's time). DAPPLE_FILM_TABLE_DIR names a directory when the app did not configure one (tests, benches).
namespace {
constexpr std::uint32_t kDiskFormat = 1;
constexpr std::uint32_t kDiskMagic = 0x42544644u;  // "DFTB"
struct DiskCfg {
  std::mutex m;
  std::string dir, salt;
  bool configured = false;
};
DiskCfg& diskCfg() {
  static DiskCfg* c = new DiskCfg;
  return *c;
}
std::atomic<unsigned long long> g_diskHits{0}, g_diskMisses{0}, g_diskStores{0}, g_diskCorrupt{0};

bool diskDirOf(std::string& dir, std::string& salt) {
  DiskCfg& c = diskCfg();
  std::lock_guard<std::mutex> lock(c.m);
  if (!c.configured) {
    if (const char* e = std::getenv("DAPPLE_FILM_TABLE_DIR")) {
      c.dir = e;
      c.salt = "env";
    }
    c.configured = true;
  }
  if (c.dir.empty() || std::getenv("DAPPLE_FILM_DISK_OFF")) return false;
  dir = c.dir, salt = c.salt;
  return true;
}

struct W {
  std::string b;
  template <class T>
  void p(const T& v) {
    b.append(reinterpret_cast<const char*>(&v), sizeof v);
  }
  void arr(const float* a, std::size_t n) { b.append(reinterpret_cast<const char*>(a), n * sizeof(float)); }
  void vec(const fd::AVec& v) {
    const std::uint64_t n = v.size();
    p(n);
    arr(v.data(), v.size());
  }
};
struct R {
  const char* q;
  std::size_t n;
  bool ok = true;
  template <class T>
  T g() {
    T v{};
    if (n < sizeof v) {
      ok = false;
      return v;
    }
    std::memcpy(&v, q, sizeof v);
    q += sizeof v, n -= sizeof v;
    return v;
  }
  void arr(float* a, std::size_t k) {
    if (n < k * sizeof(float)) {
      ok = false;
      return;
    }
    std::memcpy(a, q, k * sizeof(float));
    q += k * sizeof(float), n -= k * sizeof(float);
  }
  void vec(fd::AVec& v) {
    const std::uint64_t k = g<std::uint64_t>();
    if (!ok || k > (1ull << 26) || n < k * sizeof(float)) {
      ok = false;
      return;
    }
    v.resize(std::size_t(k));
    if (k) std::memcpy(v.data(), q, std::size_t(k) * sizeof(float));
    q += k * sizeof(float), n -= std::size_t(k) * sizeof(float);
  }
};
std::string serializeTables(const fd::FilmTables& t) {
  W w;
  w.arr(t.mIn, 12);
  w.p(std::int32_t(t.rawN));
  w.vec(t.raw);
  w.p(std::int32_t(t.curveN));
  w.p(t.curveX0);
  w.p(t.curveInvDx);
  w.arr(t.gamma, 4);
  w.vec(t.dn);
  w.vec(t.d0);
  w.arr(t.dirQ, 12);
  w.arr(t.dirQ0, 4);
  w.p(std::int32_t(t.lutN));
  w.arr(t.dmin, 4);
  w.arr(t.dscale, 4);
  w.vec(t.lut);
  w.vec(t.srgbDecode);
  w.arr(t.glareRgb, 4);
  w.p(std::int32_t(t.split ? 1 : 0));
  w.vec(t.lut1);
  w.p(std::int32_t(t.paperN));
  w.p(t.paperX0);
  w.p(t.paperInvDx);
  w.vec(t.paperCurve);
  w.arr(t.dminS, 4);
  w.arr(t.dscaleS, 4);
  w.vec(t.lut3);
  return std::move(w.b);
}
bool deserializeTables(const char* data, std::size_t size, fd::FilmTables& t) {
  R r{data, size};
  r.arr(t.mIn, 12);
  t.rawN = r.g<std::int32_t>();
  r.vec(t.raw);
  t.curveN = r.g<std::int32_t>();
  t.curveX0 = r.g<float>();
  t.curveInvDx = r.g<float>();
  r.arr(t.gamma, 4);
  r.vec(t.dn);
  r.vec(t.d0);
  r.arr(t.dirQ, 12);
  r.arr(t.dirQ0, 4);
  t.lutN = r.g<std::int32_t>();
  r.arr(t.dmin, 4);
  r.arr(t.dscale, 4);
  r.vec(t.lut);
  r.vec(t.srgbDecode);
  r.arr(t.glareRgb, 4);
  t.split = r.g<std::int32_t>() != 0;
  r.vec(t.lut1);
  t.paperN = r.g<std::int32_t>();
  t.paperX0 = r.g<float>();
  t.paperInvDx = r.g<float>();
  r.vec(t.paperCurve);
  r.arr(t.dminS, 4);
  r.arr(t.dscaleS, 4);
  r.vec(t.lut3);
  return r.ok && r.n == 0;
}
std::uint64_t fnv1a(const char* p, std::size_t n) {
  std::uint64_t h = 14695981039346656037ull;
  for (std::size_t i = 0; i < n; ++i) h = (h ^ std::uint8_t(p[i])) * 1099511628211ull;
  return h;
}

// kind: 0 raw, 1 curves, 2 print (exact), 3 print (coarse)
Hash128 diskKeyOf(const FilmResolved& r, int kind, const std::string& salt) {
  Hasher h;
  h.u64(dapple::film::libraryContentHash());
  h.str(salt);
  h.i32(int(kDiskFormat));
  h.i32(kind);
  h.str(r.stock->stock);
  h.i32(r.quality == 0);
  if (kind == 0 && r.adapt < 1.f && r.sceneX > 0.f) {
    h.f32(r.sceneX);
    h.f32(r.sceneY);
    h.f32(r.adapt);
  }
  if (kind >= 2) {
    h.str(r.paper ? r.paper->stock : std::string());
    h.i32(r.scan);
    h.f32(r.warmth);
    h.f32(r.tint);
    h.f32(r.printEv);
    h.f32(r.black);
    h.i32(dapple::filmPaperMappingEnabled());
    h.i32(r.fx.enlargerFilter != 0 && r.quality != 0 && !r.scan);
    h.i32(kind == 3 ? kCoarseLutN : 0);
  }
  return h.finish();
}
std::filesystem::path diskFile(const std::string& dir, const Hash128& k) { return std::filesystem::path(dir) / (k.hex() + ".bin"); }

bool diskLoad(const std::string& dir, const Hash128& key, fd::FilmTables& out) {
  namespace fs = std::filesystem;
  const fs::path p = diskFile(dir, key);
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    ++g_diskMisses;
    return false;
  }
  std::string buf;
  {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
      ++g_diskMisses;
      return false;
    }
    buf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  constexpr std::size_t kHead = 4 + 4 + 16 + 8 + 8;
  bool good = buf.size() >= kHead;
  std::uint32_t magic = 0, ver = 0;
  std::uint64_t ka = 0, kb = 0, len = 0, sum = 0;
  if (good) {
    std::memcpy(&magic, buf.data(), 4);
    std::memcpy(&ver, buf.data() + 4, 4);
    std::memcpy(&ka, buf.data() + 8, 8);
    std::memcpy(&kb, buf.data() + 16, 8);
    std::memcpy(&len, buf.data() + 24, 8);
    std::memcpy(&sum, buf.data() + 32, 8);
    good = magic == kDiskMagic && ver == kDiskFormat && ka == key.a && kb == key.b && len == buf.size() - kHead && fnv1a(buf.data() + kHead, std::size_t(len)) == sum;
  }
  fd::FilmTables t;
  if (!good || !deserializeTables(buf.data() + kHead, std::size_t(buf.size() - kHead), t)) {
    ++g_diskCorrupt;
    fs::remove(p, ec);  // silent rebuild
    return false;
  }
  out = std::move(t);
  fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // least recently used eviction
  ++g_diskHits;
  return true;
}

void diskEnforceCap(const std::string& dir) {
  namespace fs = std::filesystem;
  std::uint64_t cap = 64ull << 20;
  if (const char* e = std::getenv("DAPPLE_FILM_DISK_CAP_MB")) cap = std::uint64_t(std::max(0.0, std::atof(e)) * 1048576.0);
  std::error_code ec;
  struct F {
    fs::path p;
    fs::file_time_type t;
    std::uint64_t size;
  };
  std::vector<F> files;
  std::uint64_t total = 0;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const auto sz = std::uint64_t(e.file_size(ec));
    const auto tm = e.last_write_time(ec);
    if (e.path().extension() == ".tmp") {  // a writer that died: older than an hour
      if (now - tm > std::chrono::hours(1)) fs::remove(e.path(), ec);
      continue;
    }
    if (e.path().extension() != ".bin") continue;
    files.push_back({e.path(), tm, sz});
    total += sz;
  }
  if (total <= cap) return;
  std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
  for (const F& f : files) {
    if (total <= cap) break;
    fs::remove(f.p, ec);
    total -= f.size;
  }
}

void diskStoreNow(const std::string& dir, const Hash128& key, const fd::FilmTables& t) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string payload = serializeTables(t);
  std::string file;
  const std::uint32_t magic = kDiskMagic, ver = kDiskFormat;
  const std::uint64_t len = payload.size(), sum = fnv1a(payload.data(), payload.size());
  file.append(reinterpret_cast<const char*>(&magic), 4);
  file.append(reinterpret_cast<const char*>(&ver), 4);
  file.append(reinterpret_cast<const char*>(&key.a), 8);
  file.append(reinterpret_cast<const char*>(&key.b), 8);
  file.append(reinterpret_cast<const char*>(&len), 8);
  file.append(reinterpret_cast<const char*>(&sum), 8);
  file += payload;
  // per-process temporary name (two processes writing the same table must not share a file), then renamed into place
  static const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tag));
  const fs::path dst = diskFile(dir, key), tmp = dst.string() + "." + tag + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(file.data(), std::streamsize(file.size()));
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      return;
    }
  }
  fs::rename(tmp, dst, ec);
  if (ec) {  // Windows does not replace an existing file by rename
    fs::remove(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return;
    }
  }
  ++g_diskStores;
  diskEnforceCap(dir);
}

// One writer thread: the frame thread and the table workers only enqueue a copy.
struct DiskWriter {
  std::mutex m;
  std::condition_variable cv;
  std::list<std::function<void()>> q;
  bool started = false;
  unsigned pending = 0;
  void post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(m);
      if (q.size() > 64) return;  // never an unbounded backlog
      q.push_back(std::move(fn));
      ++pending;
      if (!started) {
        started = true;
        std::thread([this] { loop(); }).detach();
      }
    }
    cv.notify_all();
  }
  void loop() {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return !q.empty(); });
        fn = std::move(q.front());
        q.pop_front();
      }
      try {
        fn();
      } catch (...) {
      }
      std::lock_guard<std::mutex> lock(m);
      --pending;
      cv.notify_all();
    }
  }
  void drain() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return pending == 0; });
  }
};
DiskWriter& diskWriter() {
  static DiskWriter* w = new DiskWriter;
  return *w;
}

template <int KIND, class T>
Status bakeWithDisk(const void* resolved, void** payloadOut, Status (*bake)(const void*, std::span<const void* const>, void**)) {
  const FilmResolved& r = *static_cast<const FilmResolved*>(resolved);
  std::string dir, salt;
  if (!diskDirOf(dir, salt) || !r.stock) return bake(resolved, {}, payloadOut);
  const Hash128 key = diskKeyOf(r, KIND, salt);
  {
    auto t = std::make_unique<T>();
    if (diskLoad(dir, key, t->t)) {
      *payloadOut = static_cast<PayloadBase*>(t.release());
      return Status::Ok;
    }
  }
  const Status s = bake(resolved, {}, payloadOut);
  if (s == Status::Ok) {
    const fd::FilmTables copy = payloadAs<T>(*payloadOut)->t;
    diskWriter().post([dir, key, copy] { diskStoreNow(dir, key, copy); });
  }
  return s;
}
Status bakeRawDisk(const void* r, std::span<const void* const>, void** out) { return bakeWithDisk<0, RawTable>(r, out, bakeRaw); }
Status bakeCurvesDisk(const void* r, std::span<const void* const>, void** out) { return bakeWithDisk<1, CurvesTable>(r, out, bakeCurves); }
Status bakePrintScanDisk(const void* r, std::span<const void* const>, void** out) { return bakeWithDisk<2, PrintTable>(r, out, bakePrintScan); }
Status bakePrintScanCoarseDisk(const void* r, std::span<const void* const>, void** out) { return bakeWithDisk<3, PrintTable>(r, out, bakePrintScanCoarse); }
}  // namespace

void setFilmTableDisk(const std::string& dir, const std::string& salt) {
  DiskCfg& c = diskCfg();
  std::lock_guard<std::mutex> lock(c.m);
  c.dir = dir, c.salt = salt, c.configured = true;
  dapple::setFilmCalibrationDisk(dir, salt);
}
void filmTableDiskDrain() { diskWriter().drain(); }
void filmTableDiskStats(unsigned long long* hits, unsigned long long* misses, unsigned long long* stores, unsigned long long* corrupt) {
  if (hits) *hits = g_diskHits.load();
  if (misses) *misses = g_diskMisses.load();
  if (stores) *stores = g_diskStores.load();
  if (corrupt) *corrupt = g_diskCorrupt.load();
}
bool filmTableDiskRoundTrip(const fd::FilmTables& in, fd::FilmTables& out) {
  const std::string s = serializeTables(in);
  return deserializeTables(s.data(), s.size(), out);
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
  while (c.l.size() > 24) c.l.pop_back();  // wave 8: 24 (was 6): the film map slides across ~10 stocks in a second and each needs its raw / curves / print tables (about 1.5 MB each)
  return sp;
}
template <class T>
bool cachedPeek(TableLru<T>& c, const Hash128& key) {
  std::lock_guard<std::mutex> lock(c.m);
  for (const auto& e : c.l)
    if (e.first == key) return true;
  return false;
}
template <class T>
std::shared_ptr<const T> cachedFind(TableLru<T>& c, const Hash128& key) {
  std::lock_guard<std::mutex> lock(c.m);
  for (auto it = c.l.begin(); it != c.l.end(); ++it)
    if (it->first == key) {
      c.l.splice(c.l.begin(), c.l, it);
      return c.l.front().second;
    }
  return nullptr;
}
}  // namespace

namespace {
// ---- background print tables while a print-table slider moves (wave 8, Redmi / Pixel 5: 17-124 ms of CPU bake + upload per drag frame) --------
// An interactive drag frame never bakes on the frame thread: it uses the coarse table of its own parameter values when one is cached, else the
// most recent table of the same stock / paper / mapping (the previous step of the drag), and asks the worker to bake the coarse table for the
// values it wanted. The worker runs on the background pool (own threads, same capability-chosen cores) and keeps only the latest request.
// The settled frame is not coarse and bakes / fetches the exact table on the frame thread exactly as before (DAPPLE_FILM_ASYNC_TABLES=0
// restores the synchronous path, the engine tests use it).
TableLru<RawTable>& rawsLru() {
  static TableLru<RawTable> t;
  return t;
}
TableLru<CurvesTable>& curvesLru() {
  static TableLru<CurvesTable> t;
  return t;
}
TableLru<PrintTable>& printsExact() {
  static TableLru<PrintTable> t;
  return t;
}
TableLru<PrintTable>& printsCoarseLru() {
  static TableLru<PrintTable> t;
  return t;
}
struct LastPrint {
  std::mutex m;
  Hash128 base;
  std::shared_ptr<const PrintTable> t;
};
LastPrint& lastPrint() {
  static LastPrint l;
  return l;
}
void noteLastPrint(const Hash128& base, const std::shared_ptr<const PrintTable>& t) {
  LastPrint& l = lastPrint();
  std::lock_guard<std::mutex> lock(l.m);
  l.base = base;
  l.t = t;
}
std::shared_ptr<const PrintTable> staleFor(const Hash128& base) {
  LastPrint& l = lastPrint();
  std::lock_guard<std::mutex> lock(l.m);
  return l.t && l.base == base ? l.t : nullptr;
}
std::atomic<unsigned long long> g_staleUses{0};
std::atomic<bool> g_staleOutstanding{false};  // a frame used a stale table and no table has landed since
struct Listeners {
  std::mutex m;
  std::vector<std::pair<TableLandedFn, void*>> v;
};
Listeners& listeners() {
  static Listeners* l = new Listeners;
  return *l;
}
void notifyLanded() {
  std::vector<std::pair<TableLandedFn, void*>> copy;
  {
    std::lock_guard<std::mutex> lock(listeners().m);
    copy = listeners().v;
  }
  for (auto& e : copy) e.first(e.second);
}
struct PrintWorker {
  std::mutex m;
  std::condition_variable cv;
  bool have = false, started = false;
  FilmResolved r{};
  Hash128 key{}, base{};
  std::atomic<unsigned> baked{0}, requested{0};
  void request(const FilmResolved& rr, const Hash128& k, const Hash128& b) {
    {
      std::lock_guard<std::mutex> lock(m);
      r = rr;
      key = k;
      base = b;
      have = true;
      requested.fetch_add(1);
      if (!started) {
        started = true;
        std::thread([this] { loop(); }).detach();
      }
    }
    cv.notify_one();
  }
  void loop() {
    ThreadPool::tlUse() = &ThreadPool::background();
    for (;;) {
      FilmResolved rr;
      Hash128 k, b;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return have; });
        rr = r, k = key, b = base;
        have = false;
      }
      try {
        auto t = cachedTable(printsCoarseLru(), k, bakePrintScanCoarseDisk, rr);
        if (t) noteLastPrint(b, t);
      } catch (...) {
      }
      baked.fetch_add(1);
      bool newer;
      {
        std::lock_guard<std::mutex> lock(m);
        newer = have;
      }
      if (!newer && g_staleOutstanding.exchange(false)) notifyLanded();
    }
  }
};
PrintWorker& printWorker() {
  static PrintWorker* w = new PrintWorker;  // leaked: the thread outlives static destruction
  return *w;
}
bool asyncTables() {
  static const bool on = [] {
    const char* e = std::getenv("DAPPLE_FILM_ASYNC_TABLES");
    return !(e && e[0] == '0') && !dapple::deterministicMode();
  }();
  return on;
}
}  // namespace

bool tablesFor(const FilmResolved& r, FilmTablesHandle& out) {
  TableLru<RawTable>& raws = rawsLru();
  TableLru<CurvesTable>& curves = curvesLru();
  TableLru<PrintTable>& prints = printsExact();
  Hasher hs;
  hs.u64(std::uintptr_t(r.lib));
  hs.u64(std::uintptr_t(r.stock));
  hs.i32(r.quality == 0);
  const Hash128 ks = hs.finish();
  Hasher hb;  // what a stale print table must share with the wanted one: everything but warmth / tint / print exposure / black
  hb.hash(ks);
  hb.u64(std::uintptr_t(r.paper));
  hb.i32(r.scan);
  hb.i32(r.quality == 0);
  hb.i32(dapple::filmPaperMappingEnabled());
  hb.i32(r.fx.enlargerFilter != 0 && r.quality != 0 && !r.scan);  // split-path tables
  const Hash128 base = hb.finish();
  Hasher hp;
  hp.hash(base);
  hp.f32(r.warmth);
  hp.f32(r.tint);
  hp.f32(r.printEv);
  hp.f32(r.black);
  Hasher hr;  // the raw table also carries the input matrix: scene illuminant and degree of adaptation (only when the film's own balance is not in use)
  hr.hash(ks);
  if (r.adapt < 1.f && r.sceneX > 0.f) {
    hr.f32(r.sceneX);
    hr.f32(r.sceneY);
    hr.f32(r.adapt);
  }
  out.raw = cachedTable(raws, hr.finish(), bakeRawDisk, r);
  out.curves = cachedTable(curves, ks, bakeCurvesDisk, r);
  const Hash128 kp = hp.finish();
  bool usedStale = false;
  // Interactive frame while a print-table slider moves (r.coarse): the exact table when it is cached, else a coarse one (cached on its own, so
  // a drag does not evict the exact tables); the frame after the drag is not coarse and bakes the exact table.
  if (r.coarse && !cachedPeek(prints, kp)) {
    Hasher hc;
    hc.hash(kp);
    hc.i32(kCoarseLutN);
    const Hash128 kc = hc.finish();
    if (asyncTables()) {
      out.print = cachedFind(printsCoarseLru(), kc);
      if (!out.print) {
        g_staleOutstanding.store(true);  // before the request: the worker may land the table before this frame has used the stale one
        printWorker().request(r, kc, base);
        out.print = staleFor(base), usedStale = out.print != nullptr;
        if (usedStale) g_staleUses.fetch_add(1, std::memory_order_relaxed);
        else g_staleOutstanding.store(false);  // nothing stale was shown (the table baked synchronously below)  // the previous step of the drag (or the settled table it started from)
        if (!out.print) out.print = cachedTable(printsCoarseLru(), kc, bakePrintScanCoarseDisk, r);  // the first table of this stock: bake here
      }
    } else {
      out.print = cachedTable(printsCoarseLru(), kc, bakePrintScanCoarseDisk, r);
    }
  } else {
    out.print = cachedTable(prints, kp, bakePrintScanDisk, r);
  }
  if (out.print && !usedStale) noteLastPrint(base, out.print);
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
  h.f32(r.fx.boostEv);
  h.f32(r.fx.boostRange);
  h.f32(r.fx.protectEv);
  h.i32(r.fx.enlargerFilter);
  for (float v : {r.fx.enlStrength, r.fx.enlScale, r.fx.enlHaloWarmth, r.fx.enlCoreIntensity, r.fx.enlCoreSize, r.fx.enlHaloIntensity,
                  r.fx.enlHaloSize, r.fx.enlBloomIntensity, r.fx.enlBloomSize})
    h.f32(v);
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
  // The print flare's noise belongs to the print / enlarger, not to a negative frame: it is held across a clip (frameIndex 0's stream), so only the grain and the
  // emulsion micro-structure animate from frame to frame (docs/engine/pipeline-architecture.md section 30.4). A still (seed 0) is unchanged.
  pd->streamGlare = mix32(0u * 0x9E3779B1u + 21u * 0x85EBCA6Bu + 0x165667B1u);
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

namespace dapple::engine::filmnode {
void filmAsyncTableStats(unsigned long long* staleUses, unsigned long long* baked, unsigned long long* requested) {
  if (staleUses) *staleUses = g_staleUses.load();
  if (baked) *baked = printWorker().baked.load();
  if (requested) *requested = printWorker().requested.load();
}
}  // namespace dapple::engine::filmnode

namespace dapple::engine::filmnode {
void addTableLandedListener(TableLandedFn fn, void* user) {
  std::lock_guard<std::mutex> lock(listeners().m);
  listeners().v.push_back({fn, user});
}
void removeTableLandedListener(TableLandedFn fn, void* user) {
  std::lock_guard<std::mutex> lock(listeners().m);
  auto& v = listeners().v;
  v.erase(std::remove(v.begin(), v.end(), std::make_pair(fn, user)), v.end());
}
bool filmTablesStale() { return g_staleOutstanding.load(); }
}  // namespace dapple::engine::filmnode

namespace dapple::engine::filmnode {
namespace {
std::atomic<bool> g_matchStale{false};
}
void markMatchStale() { g_matchStale.store(true); }
void notifyRepaintIfMatchStale() {
  if (g_matchStale.exchange(false)) notifyLanded();
}
}  // namespace dapple::engine::filmnode

// ---- stock selection (wave 8): the film map selects stocks live while the finger slides ----------------------------------------------------------
// An interactive frame of a stock whose tables are not cached renders with the previous stock (GpuExecutor::filmResolve substitutes the previous
// FilmResolved) while one latest-wins worker on the background pool bakes the wanted stock's raw / curve / print tables and its plan; the repaint
// listeners fire when it landed. Cancellation is "latest wins": a worker that is baking a stock the finger already left finishes it (the tables are
// cached for a later selection) and then takes the newest request.
namespace dapple::engine::filmnode {
namespace {
struct StockKeys {
  Hash128 ks, hr, kp;
};
StockKeys stockKeys(const FilmResolved& r) {
  Hasher hs;
  hs.u64(std::uintptr_t(r.lib));
  hs.u64(std::uintptr_t(r.stock));
  hs.i32(r.quality == 0);
  StockKeys k;
  k.ks = hs.finish();
  Hasher hb;
  hb.hash(k.ks);
  hb.u64(std::uintptr_t(r.paper));
  hb.i32(r.scan);
  hb.i32(r.quality == 0);
  hb.i32(dapple::filmPaperMappingEnabled());
  hb.i32(r.fx.enlargerFilter != 0 && r.quality != 0 && !r.scan);
  Hasher hp;
  hp.hash(hb.finish());
  hp.f32(r.warmth);
  hp.f32(r.tint);
  hp.f32(r.printEv);
  hp.f32(r.black);
  k.kp = hp.finish();
  Hasher hr;
  hr.hash(k.ks);
  if (r.adapt < 1.f && r.sceneX > 0.f) {
    hr.f32(r.sceneX);
    hr.f32(r.sceneY);
    hr.f32(r.adapt);
  }
  k.hr = hr.finish();
  return k;
}
std::atomic<bool> g_stockStale{false};
struct StockWorker {
  std::mutex m;
  std::condition_variable cv;
  bool have = false, started = false;
  FilmResolved r{};
  void request(const FilmResolved& rr) {
    {
      std::lock_guard<std::mutex> lock(m);
      r = rr;
      have = true;
      if (!started) {
        started = true;
        std::thread([this] { loop(); }).detach();
      }
    }
    cv.notify_one();
  }
  void loop() {
    ThreadPool::tlUse() = &ThreadPool::background();
    for (;;) {
      FilmResolved rr;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return have; });
        rr = r;
        have = false;
      }
      try {
        rr.coarse = 0;
        FilmTablesHandle tb;
        tablesFor(rr, tb);
        planOf(rr);
      } catch (...) {
      }
      bool newer;
      {
        std::lock_guard<std::mutex> lock(m);
        newer = have;
      }
      if (!newer && g_stockStale.exchange(false)) notifyLanded();
    }
  }
};
StockWorker& stockWorker() {
  static StockWorker* w = new StockWorker;
  return *w;
}
}  // namespace

bool tablesCachedFor(const FilmResolved& r) {
  const StockKeys k = stockKeys(r);
  return cachedPeek(rawsLru(), k.hr) && cachedPeek(curvesLru(), k.ks) && cachedPeek(printsExact(), k.kp);
}
void requestStockPrep(const FilmResolved& r) {
  g_stockStale.store(true);
  stockWorker().request(r);
}
}  // namespace dapple::engine::filmnode
