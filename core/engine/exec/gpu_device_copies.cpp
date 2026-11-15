// The GPU executor's device copies (see gpu_executor_internal.h): the resident source, the colour tables, the HDR / reshaping / local buffers,
// the display stages and the film tables of a frame. Cached copies live in the session's ResultCache under device slots with their device
// bytes (one budget with the session's host entries' cache and the pool, ResultCache::Limits); released copies go to Core::retire.
#include <algorithm>
#include <cstring>

#include "dapple/film.h"
#include "determinism.h"
#include "engine/exec/gpu_executor_internal.h"
#include "engine/nodes/film/film_async.h"
#include "engine/nodes/film/frame_trace.h"
#include "hdr_field.h"
#include "warp_field.h"
#include "local_eval.h"

namespace dapple::engine::gpu {

DisplayBind DeviceDisplay::bind(GpuGraph& g) const {
  DisplayBind b;
  b.curves = g.externalBuf(curves);
  b.lut = g.external(lut);
  b.lutN = lutN;
  b.curveMask = curveMask;
  b.curveSamples = curveSamples;
  b.lutMix = lutMix;
  for (int c = 0; c < 3; ++c) b.domMin[c] = domMin[c], b.domMax[c] = domMax[c];
  return b;
}

void Executor::bindSource(SourceStore& s) {
  if (store_ == &s && rs_ && srcId_ == s.identity() && W_ == s.width() && H_ == s.height()) return;
  store_ = &s;
  srcId_ = s.identity();
  W_ = s.width();
  H_ = s.height();
  std::shared_ptr<Context> c = ctx_;
  const std::uint64_t ep = epoch_;
  rs_ = std::static_pointer_cast<ResidentSource>(ctx_->residentFor(srcId_, [c, ep]() -> std::shared_ptr<void> {
    auto r = std::make_shared<ResidentSource>();
    r->ctx = c;
    r->epoch = ep;
    return r;
  }));
}

// The upload (24 MB submissions) goes into an image of this frame and hands the device to a waiting interactive frame between two
// submissions; it is published at the end, inside the turn. Another executor of the same store that published meanwhile wins (ours is retired).
bool Executor::ensureResident() {
  if (!rs_ || !store_) return fail(GpuReason::SourceTooLarge, "no source");
  if (rs_->have) return true;
  if (!canResident(W_, H_)) return fail(GpuReason::SourceTooLarge, "source too large for a resident GPU copy");
  hal::ImageH img;
  hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(W_), std::uint32_t(H_), 1}, &img);
  if (s != hal::Status::Ok) return failStatus(s, "");
  std::size_t pitch = 0;
  const float* px = store_->window(0, 0, W_, H_, pitch);
  std::string err;
  bool rebuilt = false;
  const std::function<bool()> between = [&] { return yieldTurn() || (rebuilt = true, false); };
  if (!px || !core().uploadRgbF32(px, pitch, W_, H_, img, 0, 0, err, between)) {
    if (rebuilt) return false;  // the device was rebuilt meanwhile: the image belongs to the lost one (turnOk set the failure)
    core().retire(img);         // chunks of it may still be in flight
    if (!px) return fail(GpuReason::DeviceError, "source store cannot be read");
    return dev().state() == hal::DeviceState::Lost ? fail(GpuReason::DeviceLost, err) : fail(GpuReason::DeviceError, err);
  }
  if (rs_->have) {
    core().retire(img);
    return true;
  }
  rs_->img = img;
  rs_->have = true;
  return true;
}

// The resident source resized (Lanczos-3) to w x h; cached (two sizes). The result holds the image (aliasing pointer) for the frame.
std::shared_ptr<const hal::ImageH> Executor::resizedSource(int w, int h) {
  if (w == W_ && h == H_) return std::shared_ptr<const hal::ImageH>(rs_, &rs_->img);
  for (auto& r : rs_->resized)
    if (r->w == w && r->h == h) {
      r->used = ++rs_->tick;
      return std::shared_ptr<const hal::ImageH>(r, &r->img);
    }
  if (!canResident(w, h)) return fail(GpuReason::SourceTooLarge, "resized source too large"), nullptr;
  auto r = std::make_shared<ResidentSource::Resized>();
  r->ctx = ctx_;
  r->epoch = epoch_;
  r->w = w;
  r->h = h;
  hal::Status s = dev().createImage({hal::Format::RGBA16F, std::uint32_t(w), std::uint32_t(h), 1}, &r->img);
  if (s != hal::Status::Ok) return r->img = {}, failStatus(s, ""), nullptr;
  auto cl = dev().begin();
  if (!cl) {
    dev().destroy(r->img);
    return failStatus(hal::Status::Timeout, ""), nullptr;
  }
  GpuGraph g(dev(), core().pool());
  std::string err;
  GpuRecorder rec(dev(), g, err);
  recorderFor(rec, cl->slot());
  const H src = g.external(rs_->img);
  const SrcWin full{0, 0, W_, H_};
  const H out = recordResize(rec, src, full, resize::resolve(W_, H_, w, h), WRect{0, 0, w, h});
  if (out < 0) {
    dev().destroy(r->img);
    return failRecord(rec, err), nullptr;
  }
  g.copyImage(out, g.external(r->img), std::uint32_t(w), std::uint32_t(h));
  hal::FenceH f;
  s = submit(g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) return failStatus(s, err), nullptr;  // r retires its image
  r->used = ++rs_->tick;
  if (rs_->resized.size() >= 2) {  // retired when the last frame holding it (another executor's export between two tiles) lets go
    auto victim = std::min_element(rs_->resized.begin(), rs_->resized.end(), [](auto& a, auto& b) { return a->used < b->used; });
    rs_->resized.erase(victim);
  }
  rs_->resized.push_back(r);
  return std::shared_ptr<const hal::ImageH>(r, &r->img);
}

// Uploads `bytes` of `data` into `dst` through the slot's staging buffer and waits for it.
bool Executor::upload(hal::BufferH dst, const void* data, std::size_t bytes) {
  auto cl = dev().begin();
  if (!cl) return failStatus(hal::Status::Timeout, "");
  std::string err;
  if (!core().ensureStaging(cl->slot(), (bytes + 255) / 256 * 256, err)) return fail(GpuReason::OutOfMemory, err);
  auto& sr = core().slot(cl->slot());
  std::memset(sr.staging.mapped, 0, std::size_t(dst.bytes));
  std::memcpy(sr.staging.mapped, data, bytes);
  GpuGraph g(dev(), core().pool());
  g.copyBufferToBuffer(sr.staging, g.externalBuf(dst), dst.bytes, 0);
  hal::FenceH f;
  hal::Status s = submit(g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  return s == hal::Status::Ok || failStatus(s, err);
}

// ---- tables (HostTable payloads on the device) ------------------------------------------------------------------------------------
std::shared_ptr<DeviceTables> Executor::ensureTables(const GpuRun& in) {
  const BakedLut& lut = in.bake;
  Hasher hk;
  hk.str(lut.key);
  const Hash128 key = hk.finish();
  const CacheSlot slot = slotOf(NodeType::ColourLattice, true);
  if (const ResultCache::Hit h = in.cache.find(slot, key)) return h.as<DeviceTables>();

  if (lut.n < 2 || lut.rgb.size() != std::size_t(lut.n) * lut.n * lut.n * 3) return fail(GpuReason::LatticeUnusable, "lattice not usable (film only bake?)"), nullptr;
  if (lut.tone.size() != kToneN) return fail(GpuReason::LatticeUnusable, "tone table size differs from the GPU backend's"), nullptr;
  std::vector<float> overOwned;
  const std::vector<float>* over = in.overflow;
  if (!over) {
    const SourceImage* img = in.store.image();
    if (!img || !in.params) return fail(GpuReason::LatticeUnusable, "overflow lattice unavailable (no CPU source)"), nullptr;
    overOwned = point::bakeOverflow(*in.params, *img, lut, kLat2N, kLat2Max);
    over = &overOwned;
  }
  if (over->size() != std::size_t(kLat2N) * kLat2N * kLat2N * 3) return fail(GpuReason::LatticeUnusable, "overflow lattice unavailable"), nullptr;

  const auto t0 = Clock::now();
  auto t = std::make_shared<DeviceTables>();
  t->ctx = ctx_;
  t->epoch = epoch_;
  t->n = lut.n;
  t->n2 = kLat2N;
  hal::Status s = dev().createImage({hal::Format::RGBA32F, std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n)}, &t->lat);
  if (s == hal::Status::Ok) s = dev().createImage({hal::Format::RGBA32F, kLat2N, kLat2N, kLat2N}, &t->lat2);
  if (s == hal::Status::Ok) s = dev().createBuffer({kToneN * 4, hal::MemKind::DeviceLocal, true}, &t->tone);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;

  const std::size_t latBytes = std::size_t(lut.n) * lut.n * lut.n * 16, lat2Bytes = std::size_t(kLat2N) * kLat2N * kLat2N * 16;
  const std::size_t toneBytes = (kToneN * 4 + 255) / 256 * 256;
  auto cl = dev().begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), nullptr;
  std::string err;
  if (!core().ensureStaging(cl->slot(), toneBytes + latBytes + lat2Bytes, err)) return fail(GpuReason::OutOfMemory, err), nullptr;
  auto& sr = core().slot(cl->slot());
  auto* base = static_cast<std::uint8_t*>(sr.staging.mapped);
  std::memcpy(base, lut.tone.data(), kToneN * 4);
  auto texels = [](const std::vector<float>& rgb, float* o) {
    const std::size_t n = rgb.size() / 3;
    for (std::size_t i = 0; i < n; ++i) {
      o[4 * i] = rgb[3 * i];
      o[4 * i + 1] = rgb[3 * i + 1];
      o[4 * i + 2] = rgb[3 * i + 2];
      o[4 * i + 3] = 1.f;
    }
  };
  texels(lut.rgb, reinterpret_cast<float*>(base + toneBytes));
  texels(*over, reinterpret_cast<float*>(base + toneBytes + latBytes));
  GpuGraph g(dev(), core().pool());
  g.copyBufferToBuffer(sr.staging, g.externalBuf(t->tone), kToneN * 4, 0);
  g.copyBufferToImage(sr.staging, g.external(t->lat), std::uint32_t(lut.n), std::uint32_t(lut.n), std::uint32_t(lut.n), toneBytes);
  g.copyBufferToImage(sr.staging, g.external(t->lat2), kLat2N, kLat2N, kLat2N, toneBytes + latBytes);
  hal::FenceH f;
  s = submit(g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) return failStatus(s, err), nullptr;

  ResultCache::Entry e;
  e.slot = slot;
  e.key = e.family = key;
  e.order = lut.n;
  e.payload = t;
  e.deviceBytes = latBytes + lat2Bytes + kToneN * 4;
  e.costMs = msSince(t0);
  in.cache.put(std::move(e));
  return t;
}

std::shared_ptr<DeviceBuffer> Executor::ensureGeoAux(const GpuRun& in, int& warpOff) {
  warpOff = 0;
  const point::Blocks& P = in.frame;
  std::vector<float> data;
  std::uint64_t key = 0;
  if (P.hdr.active()) {
    data = P.hdr.field->g;
    key = P.hdr.field->id;
  }
  if (P.warp.active()) {
    key = key * 1099511628211ull ^ (P.warp.field->id * 0x9E3779B97F4A7C15ull + 0x5245534841504555ull);  // "RESHAPE"
    warpOff = int(data.size());
    data.insert(data.end(), P.warp.field->d.begin(), P.warp.field->d.end());
  }
  return ensureBuffer(in.cache, kDeviceBufferSlot, key, data.data(), data.size() * sizeof(float));
}

std::shared_ptr<DeviceBuffer> Executor::ensureBuffer(ResultCache& cache, CacheSlot slot, std::uint64_t key64, const void* data, std::size_t bytes) {
  Hasher hk;
  hk.u64(key64);
  hk.u64(bytes);
  hk.u64(slot);
  const Hash128 key = hk.finish();
  if (const ResultCache::Hit h = cache.find(slot, key)) return h.as<DeviceBuffer>();
  const auto t0 = Clock::now();
  auto t = std::make_shared<DeviceBuffer>();
  t->ctx = ctx_;
  t->epoch = epoch_;
  const std::size_t size = std::max<std::size_t>((bytes + 15) / 16 * 16, 16);
  hal::Status s = dev().createBuffer({size, hal::MemKind::DeviceLocal, true}, &t->buf);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;
  if (!upload(t->buf, data, bytes)) return nullptr;
  ResultCache::Entry e;
  e.slot = slot;
  e.key = e.family = key;
  e.payload = t;
  e.deviceBytes = size;
  e.costMs = msSince(t0);
  cache.put(std::move(e));
  return t;
}

bool Executor::ensureLocal(const GpuRun& in, std::shared_ptr<DeviceBuffer>& desc, std::shared_ptr<DeviceBuffer>& planes) {
  desc.reset();
  planes.reset();
  const local::Frame* f = in.blocks.local.frame;
  if (!f) return true;  // a frame with local adjustments binds them (also film frames: stage L on the film input, 40.7)
  desc = ensureBuffer(in.cache, kDeviceBufferSlot, f->descKey, f->desc.data(), f->desc.size() * sizeof(float));
  if (!desc) return false;
  Hasher hk;  // the store is built (copied out of the plane cache) only on a miss
  hk.u64(f->planesKey);
  hk.u64(std::uint64_t(std::max(1, f->planeFloats)) * sizeof(float));
  hk.u64(kDevicePlaneSlot);
  if (const ResultCache::Hit h = in.cache.find(kDevicePlaneSlot, hk.finish())) {
    planes = h.as<DeviceBuffer>();
    return true;
  }
  const std::vector<float> store = local::planeStore(*f);
  planes = ensureBuffer(in.cache, kDevicePlaneSlot, f->planesKey, store.data(), store.size() * sizeof(float));
  return planes != nullptr;
}

LutBind Executor::lutBind(GpuGraph& g, const DeviceTables& t) {
  LutBind b;
  b.tone = g.externalBuf(t.tone);
  b.lat = g.external(t.lat);
  b.lat2 = g.external(t.lat2);
  b.n = t.n;
  b.n2 = t.n2;
  b.toneN = int(kToneN);
  b.toneBase = kToneBase;
  b.max1 = kBakeMax;
  b.max2 = kLat2Max;
  return b;
}

// ---- film -------------------------------------------------------------------------------------------------------------------------------
bool Executor::filmResolve(const GpuRun& in, filmnode::FilmResolved& r) {
  const point::Film& f = in.frame.film;
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  if (!lib) return fail(GpuReason::FilmUnavailable, "film library not available");
  filmnode::FilmFrameArgs a;
  a.lib = lib.get();
  a.stock = f.stock.c_str();
  a.paper = f.paper.c_str();
  for (int c = 0; c < 3; ++c) a.gain[c] = in.frame.gain[c];
  a.filmExposure = f.filmEv;
  a.printEv = f.printEv;
  a.black = f.black;
  a.sceneX = f.sceneX;
  a.sceneY = f.sceneY;
  a.adapt = f.adapt;
  a.warmth = f.warmth;
  a.tint = f.tint;
  a.grainPct = f.grainPct;
  a.halationPct = f.halationPct;
  a.formatScale = f.formatScale;
  a.w = in.frame.geo.outW;
  a.h = in.frame.geo.outH;
  a.quality = f.quality;
  a.keepGrain = f.keepGrain;
  a.coarse = f.coarse;
  a.fullLongEdge = f.fullLongEdge;
  a.frameIndex = f.frameIndex;
  // r.lib / r.stock point into the process-wide default library, which outlives the frame
  if (!filmnode::filmFrameResolved(a, r)) return fail(GpuReason::FilmUnavailable, "film stock / paper not available");
  r.clampOut = f.strength >= 1.f && f.displayPlain && !in.blocks.vignette.active();
  // stock selection (wave 8): an interactive frame of a stock whose tables are not cached shows the previous stock until the background bake landed
  // (the repaint hook fires then); frame fields come from this frame, everything the tables depend on from the previous one, so nothing is baked here.
  // An interactive frame never bakes the 33^3 print table: it takes the exact one when cached, else the coarse 17^3 of its own values.
  if (r.quality == 1 && !deterministicMode()) r.coarse = 1;
  if (r.quality == 1 && haveLastFilmR_ && (r.stock != lastFilmR_.stock || r.paper != lastFilmR_.paper) && !filmnode::tablesCachedFor(r) && !deterministicMode()) {
    const char* off = std::getenv("DAPPLE_FILM_STOCK_FALLBACK");
    if (filmnode::stockTablesReady(r)) {
      // the stock's raw + curves tables are cached: the frame shows its OWN stock, its coarse print table is a ~25 ms bake; the exact tables of
      // the settled frame are prepared in the background
      filmnode::requestStockPrep(r);
    } else if (!(off && off[0] == '0')) {
      filmnode::requestStockPrep(r);
      ftrace::with([](ftrace::Rec& x) { x.stockFallback = 1; });
      filmnode::FilmResolved fb = lastFilmR_;
      for (int c = 0; c < 3; ++c) fb.gain[c] = r.gain[c];
      fb.w = r.w, fb.h = r.h, fb.fullLongEdge = r.fullLongEdge, fb.seed = r.seed, fb.frameIndex = r.frameIndex;
      fb.quality = r.quality, fb.keepGrain = r.keepGrain, fb.coarse = r.coarse, fb.clampOut = r.clampOut;
      fb.formatScale = r.formatScale, fb.pitch = r.pitch, fb.grainPct = r.grainPct, fb.halationPct = r.halationPct, fb.fx = r.fx;
      r = fb;
    }
  }
  std::string why;
  if (!filmnode::filmGpuSupported(r, why)) return fail(GpuReason::FilmEffect, why);
  return true;
}

std::shared_ptr<DeviceFilm> Executor::ensureFilm(const filmnode::FilmResolved& r) {
  filmnode::FilmTablesHandle tb;
  std::string err;
  // The process-wide table cache (raw + curves per stock, print + scan per stock / paper / warmth / tint / print EV): a strength, exposure,
  // grain or halation drag hits it, so nothing is baked and, because the cached payloads are the same objects, nothing is re-uploaded below.
  if (!filmnode::tablesFor(r, tb)) return fail(GpuReason::FilmUnavailable, "film tables: bake failed"), nullptr;
  const auto pd = filmnode::planOf(r);
  // a landed asynchronous copy of the print LUT: swap the binding (the frame that finds the fence done uses the new table)
  if (film_ && film_->epoch == epoch_ && film_->gt.lutNextActive && filmnode::finishGpuPrintLutUpdate(core(), film_->gt)) film_->tb.print = film_->gt.printNext;
  // only the print table changed (a drag landed a new coarse table): replace the LUT texels in place instead of re-creating and re-uploading everything
  if (film_ && film_->epoch == epoch_ && film_->tb.raw == tb.raw && film_->tb.curves == tb.curves && film_->tb.print != tb.print && film_->haveTables &&
      !std::getenv("DAPPLE_FILM_LUT_INPLACE_OFF")) {
    std::string uerr;
    if (r.quality == 1 && film_->gt.lutN == tb.print->t.lutN && !std::getenv("DAPPLE_FILM_LUT_SYNC")) {
      // interactive frame: fence and swap, no wait on the frame thread; the frame shows the table that is bound now (the next landing is one frame behind)
      if (!film_->gt.lutNextActive) filmnode::startGpuPrintLutUpdate(core(), tb, film_->gt, uerr);
      tb.print = film_->tb.print;
    } else {
      if (film_->gt.lutNextActive) {  // a settled frame wants the exact table: wait for the pending copy first
        core().dev().waitAll();
        if (filmnode::finishGpuPrintLutUpdate(core(), film_->gt)) film_->tb.print = film_->gt.printNext;
      }
      if (film_->tb.print != tb.print && filmnode::updateGpuPrintLut(core(), tb, film_->gt, uerr)) film_->tb = tb;
    }
  }
  if (!film_ || film_->epoch != epoch_ || film_->tb.raw != tb.raw || film_->tb.curves != tb.curves || film_->tb.print != tb.print) {
    film_.reset();
    auto f = std::make_shared<DeviceFilm>();
    f->ctx = ctx_;
    f->epoch = epoch_;
    f->tb = tb;
    if (!filmnode::uploadGpuTables(core(), tb, f->gt, err)) return fail(GpuReason::OutOfMemory, "film tables upload: " + err), nullptr;
    f->haveTables = true;
    film_ = std::move(f);
  }
  if (!film_->havePlan || film_->planKeep != pd) {
    if (film_->havePlan) filmnode::releaseGpuPlan(core(), film_->pr);
    film_->havePlan = false;
    film_->planKeep = pd;
    if (!filmnode::uploadGpuPlan(core(), r, film_->pr, err)) return fail(GpuReason::OutOfMemory, "film grain tables upload: " + err), nullptr;
    film_->havePlan = true;
  }
  if (ftrace::on() && film_->tb.print) {
    const std::uint64_t boundGen = film_->tb.print->gen;
    const int boundN = film_->gt.lutN;
    ftrace::with([&](ftrace::Rec& x) {
      if (x.stockFallback) x.printKind = ftrace::kFallbackOtherStock;
      else if (boundGen != x.wantedGen) x.printKind = ftrace::kLagging;
      x.printGen = boundGen;
      x.lutN = boundN;
    });
  }
  lastFilmR_ = r;
  haveLastFilmR_ = true;
  return film_;
}

// DisplayStages after a film: curves + look LUT on the device (the identity for plain frames), cached by content.
std::shared_ptr<DeviceDisplay> Executor::ensureDisplay(const GpuRun& in) {
  const point::Blocks& P = in.frame;
  const point::Display& d = P.display;
  const bool hasLut = d.lut && d.lutMix > 0.f && d.lut->size() >= 2 && d.lut->table().size() == std::size_t(d.lut->size()) * d.lut->size() * d.lut->size() * 3;
  const bool curves = d.curveMask != 0 && d.curveSamples >= 2 && d.curves.size() == std::size_t(4) * d.curveSamples;
  const bool plain = P.film.displayPlain || (!curves && !hasLut);
  Hasher hk;
  hk.i32(plain ? 0 : 1);
  if (!plain) {
    hk.i32(curves ? int(d.curveMask) : 0);
    hk.i32(curves ? d.curveSamples : 0);
    if (curves)
      for (float v : d.curves) hk.f32(v);
    hk.u64(hasLut ? d.lutId : 0);
    hk.i32(hasLut ? d.lut->size() : 0);
    hk.f32(hasLut ? d.lutMix : 0.f);
  }
  const Hash128 key = hk.finish();
  if (const ResultCache::Hit h = in.cache.find(kDeviceDisplaySlot, key)) return h.as<DeviceDisplay>();
  const int n = hasLut ? d.lut->size() : 1;
  if (std::uint32_t(n) > dev().caps().maxImage3D) return fail(GpuReason::LookLutTooLarge, "look LUT larger than the device's 3D image limit"), nullptr;
  const auto t0 = Clock::now();
  auto t = std::make_shared<DeviceDisplay>();
  t->ctx = ctx_;
  t->epoch = epoch_;
  t->lutN = hasLut ? n : 0;
  t->curveMask = curves && !plain ? d.curveMask : 0;
  t->curveSamples = curves && !plain ? d.curveSamples : 0;
  t->lutMix = hasLut ? d.lutMix : 0.f;
  if (hasLut)
    for (int c = 0; c < 3; ++c) {
      t->domMin[c] = d.lut->domainMin()[c];
      t->domMax[c] = d.lut->domainMax()[c];
    }
  const std::size_t curveBytes = curves ? (d.curves.size() * 4 + 255) / 256 * 256 : 256;
  const std::size_t lutBytes = std::size_t(n) * n * n * 16;
  hal::Status s = dev().createBuffer({curveBytes, hal::MemKind::DeviceLocal, true}, &t->curves);
  if (s == hal::Status::Ok) s = dev().createImage({hal::Format::RGBA32F, std::uint32_t(n), std::uint32_t(n), std::uint32_t(n)}, &t->lut);
  if (s != hal::Status::Ok) return failStatus(s, ""), nullptr;
  auto cl = dev().begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), nullptr;
  std::string err;
  if (!core().ensureStaging(cl->slot(), curveBytes + lutBytes, err)) return fail(GpuReason::OutOfMemory, err), nullptr;
  auto& sr = core().slot(cl->slot());
  auto* base = static_cast<std::uint8_t*>(sr.staging.mapped);
  std::memset(base, 0, curveBytes);
  if (curves) std::memcpy(base, d.curves.data(), d.curves.size() * 4);
  auto* texels = reinterpret_cast<float*>(base + curveBytes);
  if (hasLut) {
    const std::vector<float>& tb = d.lut->table();
    const std::size_t cnt = tb.size() / 3;
    for (std::size_t i = 0; i < cnt; ++i) {
      texels[4 * i] = tb[3 * i];
      texels[4 * i + 1] = tb[3 * i + 1];
      texels[4 * i + 2] = tb[3 * i + 2];
      texels[4 * i + 3] = 1.f;
    }
  } else {
    texels[0] = texels[1] = texels[2] = 0.f;
    texels[3] = 1.f;
  }
  GpuGraph g(dev(), core().pool());
  g.copyBufferToBuffer(sr.staging, g.externalBuf(t->curves), curveBytes, 0);
  g.copyBufferToImage(sr.staging, g.external(t->lut), std::uint32_t(n), std::uint32_t(n), std::uint32_t(n), curveBytes);
  hal::FenceH f;
  s = submit(g, *cl, &f, err);
  if (s == hal::Status::Ok) s = dev().wait(f, kWaitNs);
  if (s != hal::Status::Ok) return failStatus(s, err), nullptr;
  ResultCache::Entry e;
  e.slot = kDeviceDisplaySlot;
  e.key = e.family = key;
  e.payload = t;
  e.deviceBytes = curveBytes + lutBytes;
  e.costMs = msSince(t0);
  in.cache.put(std::move(e));
  return t;
}

}  // namespace dapple::engine::gpu
