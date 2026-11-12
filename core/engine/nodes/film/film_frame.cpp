// Whole-frame reference executor for the film node chain (see film_nodes.h): the planner-independent driver that reproduces
// what FilmSimulator::render does, by calling the node run() functions on tiles. Used by the tests, film-render-bench and as
// the executable specification of the Film group for the CPU executor (tile size 256 x 64, halo accumulated backwards).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <mutex>

#include "engine/nodes/film/film_node_internal.h"
#include "parallel.h"

namespace dapple::engine::filmnode {

using dapple::parallelFor;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Frame workspaces (the E / E2 / q planes and the mixture arena): a few of them, so concurrent frames (a filmstrip thumbnail next
// to the edit preview) run side by side; a frame takes the first free one.
struct FrameWs {
  std::mutex m;
  fd::AVec set[3];
  fd::Arena arena;
};
constexpr int kFrameWs = 3;
FrameWs& frameWs(std::unique_lock<std::mutex>& lock) {
  static FrameWs pool[kFrameWs];
  for (FrameWs& w : pool) {
    std::unique_lock<std::mutex> l(w.m, std::try_to_lock);
    if (l.owns_lock()) {
      lock = std::move(l);
      return w;
    }
  }
  lock = std::unique_lock<std::mutex>(pool[0].m);
  return pool[0];
}
struct ArenaScope {
  explicit ArenaScope(fd::Arena* a) { g_frameArena = a; }
  ~ArenaScope() { g_frameArena = nullptr; }
};

ImageView planarView(float* data, int w, int h, std::size_t stride, std::size_t planeStride, Rect origin) {
  ImageView v;
  v.data = data;
  v.width = w;
  v.height = h;
  v.channels = 3;
  v.strideFloats = stride;
  v.origin = origin;
  v.planeStrideFloats = planeStride;
  return v;
}
ImageView packedView(float* data, int w, int h, std::size_t stride, Rect origin) {
  ImageView v;
  v.data = data;
  v.width = w;
  v.height = h;
  v.channels = 3;
  v.strideFloats = stride;
  v.origin = origin;
  return v;
}

}  // namespace

bool runFilmFrame(const FilmResolved& r, const FilmTablesHandle& tb, const float* in, std::size_t inStride, float* out,
                  std::size_t outStride, int tileW, int tileH, FilmTimingsNodes* tm) {
  const auto tStart = Clock::now();
  const auto pd = planOf(r);
  if (!pd->error.empty() || !tb.raw || !tb.curves || !tb.print) return false;
  const fd::SpatialPlan& pl = pd->plan;
  const int W = r.w, H = r.h;
  const fd::KernelSet& ks = fd::selectKernels();
  const void* tRaw = static_cast<const PayloadBase*>(tb.raw.get());
  const void* tCur = static_cast<const PayloadBase*>(tb.curves.get());
  const void* tPrint = static_cast<const PayloadBase*>(tb.print.get());
  FilmTimingsNodes local;
  FilmTimingsNodes& T = tm ? *tm : local;

  if (!pl.any()) {  // no spatial effect: the fused point chain (the CPU group of FilmExposure -> FilmDevelop -> FilmPrintScan)
    float mi[12];
    const fd::KView view = viewOf(tb.raw.get(), tb.curves.get(), tb.print.get(), r.gain, mi);
    parallelFor(H, [&](int y) { ks.block(view, in + std::size_t(y) * inStride, out + std::size_t(y) * outStride, W, 1.f); }, 4);
    T.totalMs = msSince(tStart);
    return true;
  }

  std::unique_lock<std::mutex> wsLock;
  FrameWs& fw = frameWs(wsLock);
  ArenaScope arenaScope(&fw.arena);
  const std::size_t stride = (std::size_t(W) + 15) & ~std::size_t(15);
  const std::size_t planeStride = stride * H;
  for (int i = 0; i < (pd->split ? 3 : 2); ++i)
    if (fw.set[i].size() < planeStride * 3) fw.set[i] = fd::AVec(planeStride * 3);
  float* const S0 = fw.set[0].data();
  float* const S1 = fw.set[1].data();
  const Rect frame{0, 0, W, H};
  CpuCtx ctx;
  ctx.frame = frame;

  // ---- FilmExposure ------------------------------------------------------------------------------------------------------------
  auto t0 = Clock::now();
  {
    const void* tabs[1] = {tRaw};
    CpuCtx c = ctx;
    c.tables = tabs;
    const int nb = (H + 3) / 4;
    parallelFor(
        nb,
        [&](int b) {
          const int y0 = b * 4, y1 = std::min(H, y0 + 4);
          const ImageView src = packedView(const_cast<float*>(in) + std::size_t(y0) * inStride, W, y1 - y0, inStride, {0, y0, W, y1});
          const ImageView E = planarView(S0 + std::size_t(y0) * stride, W, y1 - y0, stride, planeStride, {0, y0, W, y1});
          runExposure(c, &r, {&src, 1}, {&E, 1});
        },
        1);
  }
  T.exposureMs = msSince(t0);

  // ---- FilmBlurExposure ------------------------------------------------------------------------------------------------------------
  t0 = Clock::now();
  const bool blurActive = pl.preMix || pl.haloMix || pl.boost;  // boost: FilmBlurExposure (global maximum, then the curve)
  float* E2d = S0;
  float* Qd = S1;
  if (blurActive) {
    const ImageView Ev = planarView(S0, W, H, stride, planeStride, frame);
    const ImageView E2o = planarView(S1, W, H, stride, planeStride, frame);
    if (runBlurExposure(ctx, &r, {&Ev, 1}, {&E2o, 1}) != Status::Ok) return false;
    E2d = S1;
    Qd = S0;
  }
  T.blurMs = msSince(t0);
  const ImageView E2v = planarView(E2d, W, H, stride, planeStride, frame);

  // ---- FilmDirField ---------------------------------------------------------------------------------------------------------------
  t0 = Clock::now();
  const bool dirActive = pl.dirDiffuse;
  const ImageView Qv = planarView(Qd, W, H, stride, planeStride, frame);
  if (dirActive) {
    const void* tabs[1] = {tCur};
    CpuCtx c = ctx;
    c.tables = tabs;
    if (runDirField(c, &r, {&E2v, 1}, {&Qv, 1}) != Status::Ok) return false;
  }
  T.dirMs = msSince(t0);

  // ---- enlarger diffusion path (spec 03 s6.1): tile pass 1 (FilmDevelop -> FilmGrain -> FilmPrintExposure) writes the linear paper
  // exposure into frame-sized planes, FilmEnlargerBlur filters them, tile pass 2 (FilmPrintScan -> [FilmGlare -> FilmScan]) reads them
  // with the reflect boundary. E2 / q are dead after pass 1, their set takes the filtered exposure.
  const bool split = pd->split;
  float* const S2 = split ? fw.set[2].data() : nullptr;
  const ImageView Epv = split ? planarView(S2, W, H, stride, planeStride, frame) : ImageView{};
  const ImageView Ebv = split ? planarView(E2d, W, H, stride, planeStride, frame) : ImageView{};

  // ---- tile pass: FilmDevelop -> FilmGrain -> FilmPrintScan [-> FilmGlare -> FilmScan] -----------------------------------------------
  t0 = Clock::now();
  const int Hs = pd->Hs, Hg = pd->Hg, Hd = pd->Hd;
  const int TW = std::min(W, tileW > 0 ? tileW : 256), TH = std::min(H, tileH > 0 ? tileH : 64);
  const int ntx = (W + TW - 1) / TW, nty = (H + TH - 1) / TH;
  const bool scanStage = pd->scanStage;
  const bool grain = pl.grain.active;
  std::atomic<int> failed{0};
  // pass: 0 whole chain; 1 split pass 1 (develop, grain, paper exposure); 2 split pass 2 (paper scan, glare, scan)
  auto tilePass = [&](int ti, int pass) {
        const int tx = ti % ntx, ty = ti / ntx;
        const Rect tile{tx * TW, ty * TH, std::min(W, tx * TW + TW), std::min(H, ty * TH + TH)};
        const Rect rS = grow(tile, pass == 1 ? 0 : Hs);                 // print/scan output, glare, scan input
        const Rect rD = grow(rS, grain && pass != 2 ? Hg + Hd : 0);  // develop output / grain input
        const int dw = rw(rD), dh = rh(rD);
        const std::ptrdiff_t dls = (dw + 15) & ~15;
        const std::size_t dps = std::size_t(dls) * dh;
        const int sw = rw(rS), sh = rh(rS);
        const std::ptrdiff_t sls = (sw + 15) & ~15;
        const std::size_t sps = std::size_t(sls) * sh;
        thread_local fd::Scratch tileScratch;
        float* base = tileScratch.get(dps * 3 + (scanStage ? std::size_t(3) * sw * sh : 0) + 64);
        float* Dp = base;
        float* disp = base + dps * 3;
        float* Lp = Dp;  // the density planes are dead once FilmPrintScan has read them: the linear-light planes reuse them
        CpuCtx c = ctx;
        ImageView Dout;
        if (pass != 2) {
          {
            const void* tabs[1] = {tCur};
            c.tables = tabs;
            const ImageView D = planarView(Dp, dw, dh, std::size_t(dls), dps, rD);
            const ImageView ins[2] = {E2v, Qv};
            if (runDevelop(c, &r, {ins, dirActive ? std::size_t(2) : std::size_t(1)}, {&D, 1}) != Status::Ok) failed = 1;
          }
          const ImageView Dfull = planarView(Dp, dw, dh, std::size_t(dls), dps, rD);
          float* Dsub = Dp + std::size_t(rS.y0 - rD.y0) * dls + (rS.x0 - rD.x0);
          Dout = planarView(Dsub, sw, sh, std::size_t(dls), dps, rS);
          if (grain) {
            if (runGrain(c, &r, {&Dfull, 1}, {&Dout, 1}) != Status::Ok) failed = 1;
          }
        }
        if (pass == 1) {  // FilmPrintExposure into the frame-sized planes (rS == tile)
          const void* tabs[1] = {tPrint};
          c.tables = tabs;
          const ImageView ep = planarView(S2 + std::size_t(tile.y0) * stride + std::size_t(tile.x0), rw(tile), rh(tile), stride, planeStride, tile);
          if (runPrintExposure(c, &r, {&Dout, 1}, {&ep, 1}) != Status::Ok) failed = 1;
          return;
        }
        if (pass == 2) Dout = Ebv;  // FilmPrintScan reads the filtered exposure of the whole frame
        float* outTile = out + std::size_t(tile.y0) * outStride + std::size_t(3) * tile.x0;
        const void* tabs[1] = {tPrint};
        c.tables = tabs;
        if (!scanStage) {
          const ImageView o = packedView(outTile, rw(tile), rh(tile), outStride, tile);
          if (runPrintScan(c, &r, {&Dout, 1}, {&o, 1}) != Status::Ok) failed = 1;
          return;
        }
        const ImageView o = packedView(disp, sw, sh, std::size_t(3) * sw, rS);
        if (runPrintScan(c, &r, {&Dout, 1}, {&o, 1}) != Status::Ok) failed = 1;
        const ImageView Lv = planarView(Lp, sw, sh, std::size_t(sls), sps, rS);
        if (runGlare(c, &r, {&o, 1}, {&Lv, 1}) != Status::Ok) failed = 1;
        const ImageView fin = packedView(outTile, rw(tile), rh(tile), outStride, tile);
        if (runScan(c, &r, {&Lv, 1}, {&fin, 1}) != Status::Ok) failed = 1;
  };
  if (!split) {
    parallelFor(ntx * nty, [&](int ti) { tilePass(ti, 0); }, 1);
  } else {
    parallelFor(ntx * nty, [&](int ti) { tilePass(ti, 1); }, 1);
    if (failed.load() == 0 && runEnlargerBlur(ctx, &r, {&Epv, 1}, {&Ebv, 1}) != Status::Ok) failed = 1;
    if (failed.load() == 0) parallelFor(ntx * nty, [&](int ti) { tilePass(ti, 2); }, 1);
  }
  T.tileMs = msSince(t0);
  T.totalMs = msSince(tStart);
  return failed.load() == 0;
}

bool enabled() {
  static const bool on = [] {
    const char* e = std::getenv("DAPPLE_FILM_NODES");
    return !(e && e[0] == '0');
  }();
  return on;
}

bool filmFrameResolved(const FilmFrameArgs& a, FilmResolved& r) {
  r = FilmResolved{};
  if (!a.lib || !a.stock || !a.paper || a.w <= 0 || a.h <= 0) return false;
  r.lib = a.lib;
  r.stock = a.lib->find(a.stock);
  if (!r.stock || r.stock->isPrintMedium()) return false;
  if (std::string(a.paper) == "none") {
    r.scan = 1;
  } else {
    r.paper = a.lib->find(a.paper);
    if (!r.paper || !r.paper->isPrintMedium()) return false;
  }
  const float evGain = std::exp2(a.filmExposure);
  for (int c = 0; c < 3; ++c) r.gain[c] = a.gain[c] * evGain;
  r.warmth = a.warmth;
  r.tint = a.tint;
  r.printEv = a.printEv;
  r.black = a.black;
  r.sceneX = a.sceneX;
  r.sceneY = a.sceneY;
  r.adapt = a.adapt;
  r.grainPct = a.grainPct;
  r.halationPct = a.halationPct;
  r.formatScale = a.formatScale;
  r.w = a.w;
  r.h = a.h;
  r.fx = a.fx;
  r.quality = std::uint8_t(a.quality);
  r.keepGrain = a.keepGrain ? 1 : 0;
  r.coarse = a.coarse ? 1 : 0;
  r.fullLongEdge = a.fullLongEdge;
  r.frameIndex = a.frameIndex;
  r.seed = a.frameIndex ? filmSeed(a.frameIndex) : 0u;
  return true;
}

bool renderFilmFrame(const FilmFrameArgs& a, const float* in, std::size_t inStride, float* out, std::size_t outStride) try {
  FilmResolved r;
  if (!filmFrameResolved(a, r)) return false;
  FilmTablesHandle tb;
  if (!tablesFor(r, tb)) return false;
  return runFilmFrame(r, tb, in, inStride, out, outStride);
} catch (...) {
  return false;
}

}  // namespace dapple::engine::filmnode
