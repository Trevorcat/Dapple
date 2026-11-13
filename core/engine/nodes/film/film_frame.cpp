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
#include <cstring>
#include <vector>

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
FrameWs* frameWsPool() {
  static FrameWs pool[kFrameWs];
  return pool;
}
FrameWs& frameWs(std::unique_lock<std::mutex>& lock) {
  FrameWs* pool = frameWsPool();
  for (int i = 0; i < kFrameWs; ++i) {
    FrameWs& w = pool[i];
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

// One tile of the develop / grain / print / scan pass (shared by the whole-frame driver and the band driver: same windows, same arithmetic).
// pass: 0 whole chain; 1 split pass 1 (develop, grain, paper exposure into Epw); 2 split pass 2 (paper scan, glare, scan from Ebv).
struct TileEnv {
  const FilmResolved* r = nullptr;
  const FilmPlanData* pd = nullptr;
  const void* tCur = nullptr;
  const void* tPrint = nullptr;
  CpuCtx ctx;
  ImageView E2v, Qv, Ebv;
  bool dirActive = false, grain = false, scanStage = false;
  float* S2 = nullptr;  // split pass 1: the frame-sized paper exposure planes
  std::size_t stride = 0, planeStride = 0;
  float* out = nullptr;  // packed rows; row y at out + (y - outY0) * outStride
  std::size_t outStride = 0;
  int outY0 = 0;
};
bool filmTile(const TileEnv& e, const Rect tile, int pass) {
  const FilmPlanData& pd = *e.pd;
  const int Hs = pd.Hs, Hg = pd.Hg, Hd = pd.Hd;
  const bool grain = e.grain, scanStage = e.scanStage;
  bool failed = false;
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
  CpuCtx c = e.ctx;
  ImageView Dout;
  if (pass != 2) {
    {
      const void* tabs[1] = {e.tCur};
      c.tables = tabs;
      const ImageView D = planarView(Dp, dw, dh, std::size_t(dls), dps, rD);
      const ImageView ins[2] = {e.E2v, e.Qv};
      if (runDevelop(c, e.r, {ins, e.dirActive ? std::size_t(2) : std::size_t(1)}, {&D, 1}) != Status::Ok) failed = true;
    }
    const ImageView Dfull = planarView(Dp, dw, dh, std::size_t(dls), dps, rD);
    float* Dsub = Dp + std::size_t(rS.y0 - rD.y0) * dls + (rS.x0 - rD.x0);
    Dout = planarView(Dsub, sw, sh, std::size_t(dls), dps, rS);
    if (grain) {
      if (runGrain(c, e.r, {&Dfull, 1}, {&Dout, 1}) != Status::Ok) failed = true;
    }
  }
  if (pass == 1) {  // FilmPrintExposure into the frame-sized planes (rS == tile)
    const void* tabs[1] = {e.tPrint};
    c.tables = tabs;
    const ImageView ep = planarView(e.S2 + std::size_t(tile.y0) * e.stride + std::size_t(tile.x0), rw(tile), rh(tile), e.stride, e.planeStride, tile);
    if (runPrintExposure(c, e.r, {&Dout, 1}, {&ep, 1}) != Status::Ok) failed = true;
    return !failed;
  }
  if (pass == 2) Dout = e.Ebv;  // FilmPrintScan reads the filtered exposure of the whole frame
  float* outTile = e.out + std::size_t(tile.y0 - e.outY0) * e.outStride + std::size_t(3) * tile.x0;
  const void* tabs[1] = {e.tPrint};
  c.tables = tabs;
  if (!scanStage) {
    const ImageView o = packedView(outTile, rw(tile), rh(tile), e.outStride, tile);
    if (runPrintScan(c, e.r, {&Dout, 1}, {&o, 1}) != Status::Ok) failed = true;
    return !failed;
  }
  const ImageView o = packedView(disp, sw, sh, std::size_t(3) * sw, rS);
  if (runPrintScan(c, e.r, {&Dout, 1}, {&o, 1}) != Status::Ok) failed = true;
  const ImageView Lv = planarView(Lp, sw, sh, std::size_t(sls), sps, rS);
  if (runGlare(c, e.r, {&o, 1}, {&Lv, 1}) != Status::Ok) failed = true;
  const ImageView fin = packedView(outTile, rw(tile), rh(tile), e.outStride, tile);
  if (runScan(c, e.r, {&Lv, 1}, {&fin, 1}) != Status::Ok) failed = true;
  return !failed;
}

}  // namespace

std::size_t releaseFrameWorkspaces() {
  std::unique_lock<std::mutex> lock;
  std::size_t b = 0;
  for (int i = 0; i < kFrameWs; ++i) {
    FrameWs& w = frameWsPool()[i];
    std::unique_lock<std::mutex> l(w.m, std::try_to_lock);  // a frame in flight keeps its workspace
    if (!l.owns_lock()) continue;
    for (auto& v : w.set) b += v.size() * sizeof(float), fd::AVec().swap(v);
    b += w.arena.freeAll();
  }
  return b + releaseNodeWorkspace();
}

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
  const int TW = std::min(W, tileW > 0 ? tileW : 256), TH = std::min(H, tileH > 0 ? tileH : 64);
  const int ntx = (W + TW - 1) / TW, nty = (H + TH - 1) / TH;
  TileEnv te;
  te.r = &r, te.pd = pd.get(), te.tCur = tCur, te.tPrint = tPrint, te.ctx = ctx;
  te.E2v = E2v, te.Qv = Qv, te.Ebv = Ebv;
  te.dirActive = dirActive, te.grain = pl.grain.active, te.scanStage = pd->scanStage;
  te.S2 = S2, te.stride = stride, te.planeStride = planeStride;
  te.out = out, te.outStride = outStride, te.outY0 = 0;
  std::atomic<int> failed{0};
  auto tilePass = [&](int ti, int pass) {
    const int tx = ti % ntx, ty = ti / ntx;
    const Rect tile{tx * TW, ty * TH, std::min(W, tx * TW + TW), std::min(H, ty * TH + TH)};
    if (!filmTile(te, tile, pass)) failed = 1;
  };
  (void)Epv;
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

// ---- the film in row bands (streaming export, docs/engine/pipeline-architecture.md 42.6) ----------------------------------------------
namespace {
// Planar 3-channel rows [y0, y1) of a W-wide plane.
// Planar 3-channel rows [y0, y1) of a W-wide plane, one buffer (plane stride = rows * W): views over it need no copy.
struct Band {
  int y0 = 0, y1 = 0, w = 0;
  std::vector<float> d;
  std::size_t plane() const { return std::size_t(std::max(y1 - y0, 0)) * std::size_t(w); }
  void make(int a, int b, int width) {
    y0 = a, y1 = b, w = width;
    d.assign(plane() * 3, 0.f);
  }
  void release() { std::vector<float>().swap(d); }
  float* row(int ch, int y) { return d.data() + std::size_t(ch) * plane() + std::size_t(y - y0) * w; }
  const float* row(int ch, int y) const { return d.data() + std::size_t(ch) * plane() + std::size_t(y - y0) * w; }
  ImageView view(int fw) const { return planarView(const_cast<float*>(d.data()), fw, y1 - y0, std::size_t(w), plane(), Rect{0, y0, fw, y1}); }
};
}  // namespace

struct FilmBands::Impl {
  FilmResolved r;
  FilmTablesHandle tb;
  std::shared_ptr<const FilmPlanData> pd;
  InputRows in;
  int W = 0, H = 0;
  bool spatial = false, preMix = false, haloMix = false, dir = false, dirHalf = false;
  fd::MixtureBands pre[3], halo[3], dirM[3];
  fd::KView viewRaw{}, viewCur{}, viewAll{};
  float miRaw[12] = {}, miAll[12] = {}, miCur[12] = {};
  std::size_t kept = 0;
  int passes = 0;

  // E rows [a, b): the film input through FilmExposure
  bool exposure(int a, int b, Band& E) {
    E.make(a, b, W);
    if (b <= a) return true;
    if (!in(a, b, nullptr)) return false;  // the source of all the rows at once (one window), then the input in small slices
    const fd::KernelSet& ks = fd::selectKernels();
    constexpr int kSlice = 64;
    std::vector<float> rgb(std::size_t(std::min(kSlice, b - a)) * W * 3);
    for (int s0 = a; s0 < b; s0 += kSlice) {
      const int s1 = std::min(b, s0 + kSlice);
      if (!in(s0, s1, rgb.data())) return false;
      parallelFor(
          s1 - s0,
          [&](int k) {
            thread_local std::vector<float> row;
            if (row.size() < std::size_t(3) * W) row.resize(std::size_t(3) * W);
            ks.exposure(viewRaw, rgb.data() + std::size_t(k) * W * 3, row.data(), W, 1.f);
            float* d[3] = {E.row(0, s0 + k), E.row(1, s0 + k), E.row(2, s0 + k)};
            for (int x = 0; x < W; ++x) d[0][x] = row[3 * x], d[1][x] = row[3 * x + 1], d[2][x] = row[3 * x + 2];
          },
          4);
    }
    return true;
  }
  // rows [a, b) of a mixture stage from the stage below (get(lo, hi, band))
  template <class Below>
  bool mixStage(fd::MixtureBands* m, int a, int b, Band& out, Below&& below) {
    int ia = H, ib = 0;
    for (int ch = 0; ch < 3; ++ch) {
      int x0, x1;
      m[ch].inputRange(a, b, x0, x1);
      if (x1 > x0) ia = std::min(ia, x0), ib = std::max(ib, x1);
    }
    if (ib <= ia) ia = ib = a;
    Band src;
    if (!below(ia, ib, src)) return false;
    out.make(a, b, W);
    const fd::SpatialKernels& K = fd::selectSpatialKernels();
    for (int ch = 0; ch < 3; ++ch)
      m[ch].rows(K, a, b, [&](int y) -> const float* { return src.row(ch, y); }, [&](int y) { return out.row(ch, y); });
    src.release();
    return true;
  }
  bool mid(int a, int b, Band& out) {
    if (!preMix) return exposure(a, b, out);
    return mixStage(pre, a, b, out, [&](int x, int y, Band& s) { return exposure(x, y, s); });
  }
  bool e2(int a, int b, Band& out) {
    if (!haloMix) return mid(a, b, out);
    return mixStage(halo, a, b, out, [&](int x, int y, Band& s) { return mid(x, y, s); });
  }
  // DIR input rows [a, b) (half: rows of the half-resolution plane) from E2 rows: runDirField's arithmetic
  bool dirInput(int a, int b, const Band& E2, Band& Qin) {
    const fd::KernelSet& ks = fd::selectKernels();
    if (dirHalf) {
      const int hw = (W + 1) / 2;
      Qin.make(a, b, hw);
      parallelFor(
          b - a,
          [&](int k) {
            const int j = a + k;
            thread_local std::vector<float> e, x, q;
            if (int(e.size()) < 6 * W) e.resize(std::size_t(6) * W), x.resize(std::size_t(6) * W), q.resize(std::size_t(6) * W);
            const int ya = std::min(2 * j, H - 1), yb = std::min(2 * j + 1, H - 1);
            for (int i = 0; i < W; ++i) {
              e[3 * i] = E2.row(0, ya)[i], e[3 * i + 1] = E2.row(1, ya)[i], e[3 * i + 2] = E2.row(2, ya)[i];
              e[3 * (W + i)] = E2.row(0, yb)[i], e[3 * (W + i) + 1] = E2.row(1, yb)[i], e[3 * (W + i) + 2] = E2.row(2, yb)[i];
            }
            ks.logE(viewCur, e.data(), x.data(), 2 * W);
            ks.develop1(viewCur, x.data(), q.data(), 2 * W);
            const float* qa = q.data();
            const float* qb = q.data() + 3 * std::size_t(W);
            for (int ch = 0; ch < 3; ++ch) {
              float* o = Qin.row(ch, j);
              const int full = W / 2;
              for (int i = 0; i < full; ++i) o[i] = 0.25f * (qa[6 * i + ch] + qa[6 * i + 3 + ch] + qb[6 * i + ch] + qb[6 * i + 3 + ch]);
              if (hw > full) o[full] = 0.5f * (qa[6 * full + ch] + qb[6 * full + ch]);
            }
          },
          2);
    } else {
      Qin.make(a, b, W);
      parallelFor(
          b - a,
          [&](int k) {
            const int y = a + k;
            thread_local std::vector<float> e, x, q;
            if (int(e.size()) < 3 * W) e.resize(std::size_t(3) * W), x.resize(std::size_t(3) * W), q.resize(std::size_t(3) * W);
            for (int i = 0; i < W; ++i) e[3 * i] = E2.row(0, y)[i], e[3 * i + 1] = E2.row(1, y)[i], e[3 * i + 2] = E2.row(2, y)[i];
            ks.logE(viewCur, e.data(), x.data(), W);
            ks.develop1(viewCur, x.data(), q.data(), W);
            for (int i = 0; i < W; ++i) Qin.row(0, y)[i] = q[3 * i], Qin.row(1, y)[i] = q[3 * i + 1], Qin.row(2, y)[i] = q[3 * i + 2];
          },
          4);
    }
    return true;
  }
  // E2 rows the DIR input rows [a, b) read
  void dirInputSpan(int a, int b, int& ea, int& eb) const {
    if (dirHalf) ea = std::min(2 * a, H - 1), eb = std::min(2 * (b - 1) + 1, H - 1) + 1;
    else ea = a, eb = b;
  }
};

FilmBands::FilmBands() : m_(std::make_unique<Impl>()) {}
FilmBands::~FilmBands() = default;
std::size_t FilmBands::keptBytes() const { return m_->kept; }
int FilmBands::passes() const { return m_->passes; }

bool FilmBands::init(const FilmFrameArgs& a, InputRows in, std::string* why) {
  Impl& m = *m_;
  auto no = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (!filmFrameResolved(a, m.r)) return no("film: unknown stock / paper");
  if (!tablesFor(m.r, m.tb)) return no("film: table bake failed");
  m.pd = planOf(m.r);
  if (!m.pd->error.empty()) return no("film: plan error");
  const fd::SpatialPlan& pl = m.pd->plan;
  if (m.pd->split) return no("film: enlarger diffusion filter (a whole-frame blur after the grain) is not streamed");
  if (pl.boost) return no("film: highlight boost (a maximum of the whole frame) is not streamed");
  m.in = std::move(in);
  m.W = m.r.w, m.H = m.r.h;
  m.viewRaw = viewOf(m.tb.raw.get(), nullptr, nullptr, m.r.gain, m.miRaw);
  m.viewCur = viewOf(nullptr, m.tb.curves.get(), nullptr, nullptr, m.miCur);
  m.viewAll = viewOf(m.tb.raw.get(), m.tb.curves.get(), m.tb.print.get(), m.r.gain, m.miAll);
  m.spatial = pl.any();
  if (!m.spatial) return true;
  m.preMix = pl.preMix, m.haloMix = pl.haloMix, m.dir = pl.dirDiffuse, m.dirHalf = m.pd->dirHalf;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  for (int ch = 0; ch < 3; ++ch) {
    if (m.preMix) m.pre[ch].setup(pl.pre[ch], m.W, m.H, false);
    if (m.haloMix) m.halo[ch].setup(pl.halo[ch], m.W, m.H, false);
    if (m.dir) m.dirM[ch].setup(pl.dir[ch], m.W, m.H, m.dirHalf);
  }
  // the streamed pre-passes: one per mixture whose pyramid reaches level 2 (its levels >= 2 are kept whole)
  constexpr int kChunk = 256;
  auto needs = [](fd::MixtureBands* mb) { return mb[0].needsPrepare() || mb[1].needsPrepare() || mb[2].needsPrepare(); };
  auto feedAll = [&](fd::MixtureBands* mb, int y0, int y1, Band& b) {
    for (int ch = 0; ch < 3; ++ch)
      if (mb[ch].needsPrepare()) mb[ch].feed(K, y0, y1, [&](int y) -> const float* { return b.row(ch, y); });
  };
  if (m.preMix && needs(m.pre)) {
    for (int y = 0; y < m.H; y += kChunk) {
      Band b;
      if (!m.exposure(y, std::min(m.H, y + kChunk), b)) return no("cancelled");
      feedAll(m.pre, y, std::min(m.H, y + kChunk), b);
    }
    ++m.passes;
  }
  if (m.haloMix && needs(m.halo)) {
    for (int y = 0; y < m.H; y += kChunk) {
      Band b;
      if (!m.mid(y, std::min(m.H, y + kChunk), b)) return no("cancelled");
      feedAll(m.halo, y, std::min(m.H, y + kChunk), b);
    }
    ++m.passes;
  }
  if (m.dir && needs(m.dirM)) {
    const int ih = m.dirM[0].inputH();
    for (int j = 0; j < ih; j += kChunk) {
      const int j1 = std::min(ih, j + kChunk);  // (half-resolution DIR input: kChunk rows = 2 kChunk rows of E2)
      int ea, eb;
      m.dirInputSpan(j, j1, ea, eb);
      Band e2, q;
      if (!m.e2(ea, eb, e2)) return no("cancelled");
      m.dirInput(j, j1, e2, q);
      feedAll(m.dirM, j, j1, q);
    }
    ++m.passes;
  }
  for (int ch = 0; ch < 3; ++ch) m.kept += m.pre[ch].wholeBytes() + m.halo[ch].wholeBytes() + m.dirM[ch].wholeBytes();
  return true;
}

bool FilmBands::rows(int y0, int y1, float* out, std::size_t outStride) {
  Impl& m = *m_;
  const int W = m.W, H = m.H;
  if (y1 <= y0) return true;
  if (!m.spatial) {  // the fused point chain (runFilmFrame without spatial effects)
    std::vector<float> rgb(std::size_t(y1 - y0) * W * 3);
    if (!m.in(y0, y1, rgb.data())) return false;
    const fd::KernelSet& ks = fd::selectKernels();
    parallelFor(y1 - y0, [&](int k) { ks.block(m.viewAll, rgb.data() + std::size_t(k) * W * 3, out + std::size_t(k) * outStride, W, 1.f); }, 4);
    return true;
  }
  const FilmPlanData& pd = *m.pd;
  const fd::SpatialPlan& pl = pd.plan;
  // rows of E2 / q the tiles read: develop over the tile grown by the scan and grain halos (reflect boundary)
  const int g = pd.Hs + (pl.grain.active ? pd.Hg + pd.Hd : 0);
  int ea = H, eb = 0;
  for (int y = y0 - g; y < y1 + g; ++y) {
    const int ry = reflectIdx(y, H);
    ea = std::min(ea, ry), eb = std::max(eb, ry + 1);
  }
  // q rows [ea, eb) need DIR input rows [qa, qb), i.e. E2 rows [ua, ub)
  int qa = 0, qb = 0, ua = ea, ub = eb;
  if (m.dir) {
    qa = m.dirM[0].inputH(), qb = 0;
    for (int ch = 0; ch < 3; ++ch) {
      int x0, x1;
      m.dirM[ch].inputRange(ea, eb, x0, x1);
      if (x1 > x0) qa = std::min(qa, x0), qb = std::max(qb, x1);
    }
    if (qb > qa) {
      int sa, sb;
      m.dirInputSpan(qa, qb, sa, sb);
      ua = std::min(ua, sa), ub = std::max(ub, sb);
    }
  }
  Band E2;
  if (!m.e2(ua, ub, E2)) return false;
  // planar views (frame coordinates) of E2 / q over rows [ua, ub) / [ea, eb)
  const Rect frame{0, 0, W, H};
  const ImageView E2v = E2.view(W);
  ImageView Qv = E2v;  // unread without DIR diffusion
  Band Q;
  if (m.dir) {
    Band Qin;
    if (qb > qa) m.dirInput(qa, qb, E2, Qin);
    Q.make(ea, eb, W);
    const fd::SpatialKernels& K = fd::selectSpatialKernels();
    for (int ch = 0; ch < 3; ++ch)
      m.dirM[ch].rows(K, ea, eb, [&](int y) -> const float* { return Qin.row(ch, y); }, [&](int y) { return Q.row(ch, y); });
    Qv = Q.view(W);
  }
  TileEnv te;
  te.r = &m.r, te.pd = m.pd.get();
  te.tCur = static_cast<const PayloadBase*>(m.tb.curves.get());
  te.tPrint = static_cast<const PayloadBase*>(m.tb.print.get());
  te.ctx.frame = frame;
  te.E2v = E2v, te.Qv = Qv;
  te.dirActive = m.dir, te.grain = pl.grain.active, te.scanStage = pd.scanStage;
  te.out = out, te.outStride = outStride, te.outY0 = y0;
  const int TW = std::min(W, 256), TH = 64;
  const int ntx = (W + TW - 1) / TW, nty = (y1 - y0 + TH - 1) / TH;
  std::atomic<int> failed{0};
  parallelFor(
      ntx * nty,
      [&](int ti) {
        const int tx = ti % ntx, ty = ti / ntx;
        const Rect tile{tx * TW, y0 + ty * TH, std::min(W, tx * TW + TW), std::min(y1, y0 + ty * TH + TH)};
        if (!filmTile(te, tile, 0)) failed = 1;
      },
      1);
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
