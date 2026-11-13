// Cached automatic film exposure for interactive sessions (docs/engine/pipeline-architecture.md section 26).
// matchFilmExposure() (film_match.cpp) recomputes everything on every call: two develop renders of the 256 px proxy, two sorted scene
// statistics, the film solves, and finally one more film render for the diagnostics. A film pad drag (film_shoot / film_print /
// film_latitude / film_wb) changes only the last part, so the image-dependent statistics are computed once per photo + develop edit and the
// median-L* evaluations of film_print are remembered per (stock, paper, film EV): a pad move is arithmetic on the cached statistics.
// While a slider moves (exact = false) a median that lies between two remembered samples closer than kBracketEv is interpolated; the frame
// after the drag (exact = true) evaluates everything for real, so the final render is identical to the uncached match.
#include "engine/nodes/film/frame_trace.h"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <thread>

#include "dapple/film_match.h"
#include "parallel.h"
#include "determinism.h"
#include "dapple/renderer.h"
#include "film_match_render.h"

namespace dapple {

namespace {
constexpr float kBracketEv = 1.0f;  // two samples closer than this are interpolated (monotone cubic) while a slider moves
// Develop edit as the film match sees it: every film field and every non-tonal stage cleared.
EditParams developEdit(const EditParams& p) {
  EditParams ref = p;
  ref.filmStock.clear();
  ref.filmPaper.clear();
  ref.filmKeyEv = ref.filmPrintEv = 0.f;
  ref.filmExposure = ref.filmWarmth = ref.filmTint = ref.filmGrain = ref.filmHalation = ref.filmShoot = ref.filmPrint = ref.filmBlack = 0.f;
  ref.filmLatitude = ref.filmWb = 0.f;
  ref.filmStrength = 100.f;
  ref.clarity = ref.texture = ref.dehaze = ref.grain = 0.f;
  ref.sharpen = ref.nrLuma = 0.f;  // exactly the fields matchFilmExposure clears (vignette, nrColor, sharpRadius stay: the develop render of the proxy reads them)
  return ref;
}
int quant(float v) { return int(std::lround(v * 1000.f)); }
// Monotone cubic Hermite (Fritsch-Carlson) through the samples (sorted by x) at x, which lies in [s[i-1], s[i]].
float hermite(const std::vector<std::pair<float, float>>& s, std::size_t i, float x) {
  const auto slope = [&](std::size_t a, std::size_t b) { return (s[b].second - s[a].second) / (s[b].first - s[a].first); };
  const std::size_t a = i - 1, b = i;
  const float d = slope(a, b);
  auto tangent = [&](std::size_t k) {
    if (k == 0) return slope(0, 1);
    if (k + 1 == s.size()) return slope(k - 1, k);
    const float m0 = slope(k - 1, k), m1 = slope(k, k + 1);
    return m0 * m1 <= 0.f ? 0.f : 0.5f * (m0 + m1);
  };
  float ta = tangent(a), tb = tangent(b);
  if (d == 0.f) ta = tb = 0.f;
  else {
    const float al = ta / d, be = tb / d, r = al * al + be * be;
    if (r > 9.f) ta *= 3.f / std::sqrt(r), tb *= 3.f / std::sqrt(r);
  }
  const float h = s[b].first - s[a].first, t = (x - s[a].first) / h;
  const float t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * s[a].second + (t3 - 2 * t2 + t) * h * ta + (-2 * t3 + 3 * t2) * s[b].second + (t3 - t2) * h * tb;
}
}  // namespace

std::string filmMatchKeyLean(const EditParams& p) {
  if (p.filmStock.empty()) return {};
  EditParams k = p;
  k.filmExposure = 0.f;
  k.filmKeyEv = 0.f;
  k.filmPrintEv = 0.f;
  k.filmBlack = 0.f;
  k.filmStrength = 100.f;
  k.filmGrain = 0.f;
  k.filmHalation = 0.f;
  k.clarity = k.texture = k.dehaze = k.grain = 0.f;
  k.sharpen = k.nrLuma = 0.f;  // identical to filmMatchKey (the uncached key). vignette, nrColor and sharpRadius stay in: the solve reads them
  // The print filtration only matters when a median of the print is evaluated: film_print != 0 (and a slide's film_shoot, see solveMeter).
  // The key stays complete for those; otherwise a warmth / tint drag is not a new match.
  std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  const film::FilmStock* s = lib ? lib->find(p.filmStock) : nullptr;
  const bool needsMedian = p.filmPrint != 0.f || !s || (s->positive() && p.filmShoot != 0.f);
  if (!needsMedian) k.filmWarmth = k.filmTint = k.filmWb = 0.f;
  return toJson(k);
}

FilmExposure matchFilmExposureCached(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const std::string& lutId,
                                     const SourceImage& proxy, FilmMatchCache& cache, bool exact, FilmMatchInfo* info) try {
  const FilmMatchOptions opt = FilmMatchOptions::fromEnv();
  if (info || opt.mode != FilmMatchMode::Meter || p.filmStock.empty() || proxy.linear.width <= 0 || proxy.linear.height <= 0)
    return matchFilmExposure(p, camera, lut, proxy, info, &opt);
  std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  if (!lib) return {};
  std::string paper = p.filmPaper;
  if (paper.empty() || (paper != "none" && !(lib->find(paper) && lib->find(paper)->isPrintMedium()))) paper = film::defaultPaperFor(*lib, p.filmStock);

  // ---- image-dependent statistics: once per photo + develop edit ---------------------------------------------------------------
  const EditParams ref = developEdit(p);
  // everything matchFilmExposure reads: the develop edit (JSON: tone, colour, geometry, vignette, nrColor, sharpRadius, exposure ...), the look LUT, the as-shot
  // illuminant (not serialised); camera and proxy are fixed per session
  const std::string baseKey = toJson(ref) + "|" + lutId + "|" + std::to_string(p.exposure) + "|" + std::to_string(p.filmSceneX) + "," + std::to_string(p.filmSceneY);
  if (!cache.haveBase || cache.baseKey != baseKey) {
    cache.haveBase = false;
    Renderer r(ref, camera, lut);
    r.setToneProxy(&proxy);
    r.sceneGain(proxy, cache.gain);
    EditParams ref0 = ref;
    ref0.exposure = 0.f;
    // the intended key is rendered at exposure 0: an exposure drag keeps it (one develop render of the proxy less per frame)
    const std::string l0Key = toJson(ref0) + "|" + lutId;
    if (cache.keyL0Key != l0Key) {
      cache.keyL0 = medianLightness(renderMatchProxy(ref0, camera, lut, proxy));
      cache.keyL0Key = l0Key;
    }
    float g0[3];
    const float expGain = std::exp2(p.exposure);
    for (int c = 0; c < 3; ++c) g0[c] = cache.gain[c] / expGain;
    cache.st = sceneLumaStats(proxy.linear, cache.gain);
    cache.st0 = sceneLumaStats(proxy.linear, g0);
    cache.baseKey = baseKey;
    cache.haveBase = true;
    cache.series.clear();
    ++cache.baseBuilds;
  }

  // ---- median L* of the film render, remembered per (print-relevant edit, film EV) ----------------------------------------------
  const std::string ctx = baseKey + "|" + p.filmStock + "|" + paper + "|" + std::to_string(quant(p.filmWarmth)) + "|" +
                          std::to_string(quant(p.filmTint)) + "|" + std::to_string(quant(p.filmBlack)) + "|" + std::to_string(quant(p.filmWb));
  if (cache.seriesCtx != ctx) {
    cache.series.clear();
    cache.seriesCtx = ctx;
  }
  if (cache.seed) {  // medians rendered by the background seeding (below) for this very context
    std::lock_guard<std::mutex> lk(cache.seed->m);
    if (cache.seed->ctx == ctx && !cache.seed->samples.empty()) {
      auto& s = cache.series[quant(cache.seed->fev)];
      for (const auto& smp : cache.seed->samples) {
        auto it = std::lower_bound(s.begin(), s.end(), smp.first, [](const std::pair<float, float>& a, float v) { return a.first < v; });
        if (it == s.end() || std::fabs(it->first - smp.first) > 1e-4f) s.insert(it, smp);
      }
      cache.seed->samples.clear();
    }
  }
  EditParams fp = p;
  fp.filmStrength = 100.f;
  fp.filmExposure = 0.f;
  fp.vignette = 0.f;
  bool approx = false;
  MeterInputs mi;
  mi.lib = lib;
  mi.film = p.filmStock;
  mi.paper = paper;
  mi.st0 = cache.st0;
  mi.stFull = cache.st;
  mi.keyL0 = cache.keyL0;
  mi.shoot = p.filmShoot;
  mi.print = p.filmPrint;
  mi.latitude = p.filmLatitude;
  mi.negativeBiasEv = opt.negativeBiasEv;
  mi.medianL = [&](float fev, float pev) -> float {
    if (cache.series.size() > 48) cache.series.clear();  // a film_shoot drag at film_print != 0 visits many film EVs
    auto& s = cache.series[quant(fev)];  // sorted by print EV
    auto it = std::lower_bound(s.begin(), s.end(), pev, [](const std::pair<float, float>& a, float v) { return a.first < v; });
    if (it != s.end() && std::fabs(it->first - pev) < 1e-4f) return it->second;
    if (!exact && it != s.begin() && it != s.end() && it->first - (it - 1)->first <= kBracketEv) {
      (void)0;
      approx = true;
      ++cache.interpolated;
      return hermite(s, std::size_t(it - s.begin()), pev);
    }
    fp.filmKeyEv = fev;
    fp.filmPrintEv = pev;
    const float L = medianLightness(renderMatchProxy(fp, camera, lut, proxy));
    s.insert(std::lower_bound(s.begin(), s.end(), pev, [](const std::pair<float, float>& a, float v) { return a.first < v; }), {pev, L});
    ++cache.rendered;
    return L;
  };
  const FilmExposure fe = solveMeter(mi, nullptr);
  cache.lastApprox = approx;
  // First frame of a film_print drag: the first print solve renders the proxy several times. Once per (photo, edit, stock) a background thread renders the
  // medians on a 1 EV grid around the automatic print exposure, so the drag finds them (merged above on the next call).
  const film::FilmStock* st = lib->find(p.filmStock);
  if (p.filmPrint == 0.f && st && !st->positive() && cache.proxyOwner && cache.seededCtx != ctx && !std::getenv("DAPPLE_FILM_SEED_OFF") && !deterministicMode()) {
    cache.seededCtx = ctx;
    auto seed = std::make_shared<FilmMatchCache::Seed>();
    seed->ctx = ctx;
    seed->fev = fe.filmEv;
    cache.seed = seed;
    const EditParams fp0 = fp;
    const std::shared_ptr<const SourceImage> px = cache.proxyOwner;
    const std::shared_ptr<const Lut3D> lo = cache.lutOwner;
    const CameraProfile* cam = &camera;
    const float fev = fe.filmEv, pev0 = fe.printEv;
    std::thread([seed, fp0, px, lo, cam, fev, pev0] {
      ftrace::Busy busy(ftrace::S().bgMatch);
      sys::utilityQos();
      try {
        for (float d : {0.f, -1.f, 1.f, -2.f, 2.f, -3.f, 3.f}) {
          sys::yieldToInteraction();
          EditParams f = fp0;
          f.filmKeyEv = fev;
          f.filmPrintEv = pev0 + d;
          const float L = medianLightness(renderMatchProxy(f, *cam, lo.get(), *px));
          std::lock_guard<std::mutex> lk(seed->m);
          seed->samples.push_back({pev0 + d, L});
        }
      } catch (...) {
      }
    }).detach();
  }
  return fe;
} catch (...) {
  return {};
}

}  // namespace dapple

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace dapple {

// Print-exposure calibrations of (stock, paper, EV shift of a moved negative) cost about 50 ms each on the desktop and are image
// independent; a film_shoot drag crosses up to 13 shift buckets. They are computed once per stock + paper on a background thread when the
// stock first meets the match, so the drag only finds cached values.
void prewarmFilmCalibration(const EditParams& p, void (*lowPriority)()) {
  if (p.filmStock.empty() || std::getenv("DAPPLE_FILM_PREWARM_OFF")) return;
  std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  if (!lib) return;
  std::string paper = p.filmPaper;
  if (paper.empty() || (paper != "none" && !(lib->find(paper) && lib->find(paper)->isPrintMedium()))) paper = film::defaultPaperFor(*lib, p.filmStock);
  const film::FilmStock* st = lib->find(p.filmStock);
  if (!st || st->positive() || paper == "none") return;  // a slide has no print constant to calibrate
  static std::mutex mtx;
  static std::set<std::string> done;
  static std::atomic<std::uint64_t> selectEpoch{0};  // bumped whenever a stock meets the match for the first time
  const std::string key = p.filmStock + "|" + paper + "|" + std::to_string(std::uintptr_t(lib.get()));
  std::uint64_t epoch;
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (!done.insert(key).second) return;
    epoch = ++selectEpoch;
  }
  // Sliding across the film map visits a stock for ~150 ms: its calibration starts only once the selection has rested (a first-visit stock that is left
  // again gives its key back, so a later visit starts it afresh); on the A11 the 15 buckets x 2 workers x every visited stock kept both performance cores
  // busy for minutes (frame trace, docs/engine/pipeline-architecture.md section "stock sliding and pad drags").
  auto stillSelected = [epoch] { return selectEpoch.load() == epoch; };
  // Wave 8 (Redmi, Adreno 710 / SM7435): one low-priority thread on an efficiency core needed more than 10 s for the 15 buckets, so a drag started a few
  // seconds after choosing the stock met a cold bucket (~190 ms) in nearly every frame. Two workers on the normal scheduler class (the big cores) pull the
  // buckets outward from 0; `lowPriority` is kept only for a single-core device.
  const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
#if defined(__APPLE__)
  const int workers = 1;  // two performance cores on the oldest supported phones: one utility-QoS worker
#else
  const int workers = hc >= 4 ? 2 : 1;
#endif
  auto next = std::make_shared<std::atomic<int>>(0);
  auto nDone = std::make_shared<std::atomic<int>>(0);
  const auto tStart = std::chrono::steady_clock::now();
  for (int wk = 0; wk < workers; ++wk)
    std::thread([lib, film = p.filmStock, paper, lowPriority, next, workers, nDone, tStart, stillSelected, key] {
      ThreadPool::tlUse() = &ThreadPool::background();
      ftrace::Busy busy(ftrace::S().bgMatch);
      sys::utilityQos();
      if (lowPriority && workers == 1) lowPriority();
      try {
        for (int w = 0; w < 600 && stillSelected(); w += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!stillSelected()) {
          std::lock_guard<std::mutex> lock(mtx);
          done.erase(key);
          return;
        }
        // +-3.5 EV: film_shoot reaches +-3 EV and the constant interpolates to the next bucket. Outward from 0: a drag starts at the automatic exposure
        // and the buckets next to it are needed first (it was -3.5 .. +3.5)
        for (int i; (i = next->fetch_add(1)) < 15;) {
          sys::yieldToInteraction();
          if (!stillSelected()) {  // another stock was chosen: the rest of the buckets are computed if this stock is visited again
            std::lock_guard<std::mutex> lock(mtx);
            done.erase(key);
            return;
          }
          filmPrintConstant(lib, film, paper, i == 0 ? 0.f : (i & 1 ? 0.5f : -0.5f) * float((i + 1) / 2));
          if (nDone->fetch_add(1) + 1 == 15 && std::getenv("DAPPLE_ENGINE_LOG"))
            std::fprintf(stderr, "dapple_engine: print calibration warm-up done after %.0f ms\n",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count());
        }
      } catch (...) {
      }
    }).detach();
}

}  // namespace dapple
