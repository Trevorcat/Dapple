// The cached film exposure match (core/src/film_match_cache.cpp, docs/engine/pipeline-architecture.md section 26.2):
//   * with exact = true it returns what matchFilmExposure returns,
//   * a print drag with exact = false (interpolated medians) stays close to the exact solve, and the settled (exact) frame equals it,
//   * the lean key ignores print warmth / tint only when no median of the print is evaluated.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../deterministic.h"
#include "dapple/camera.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "engine/nodes/point/point.h"
#include "parallel.h"

using namespace dapple;

static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static SourceImage proxy(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float* p = s.linear.row(y) + 3 * x;
      const float u = float(x) / float(w), v = float(y) / float(h);
      p[0] = 0.02f + 0.6f * u * u + 0.1f * v;
      p[1] = 0.03f + 0.5f * u * (1.f - v) + 0.15f * v;
      p[2] = 0.01f + 0.3f * (1.f - u) * v + 0.05f;
    }
  return s;
}

// `seedpool`: the background median seeding (film_match_cache.cpp) renders its proxies without the frame pool. Before the fix the seed thread rendered on
// ThreadPool::instance(), the pool every frame's parallel job queues on (one job at a time), so a drag frame after a pause waited for seed renders
// (Redmi: 0.1 - 2.5 s on the first frames of a film drag; Note10+: 3.5 % of film drag frames with 0.3 - 1.5 s of `pre` / `tone` table time). Here a
// frame-pool job is held for 2 s while the match runs on the background pool, as the session's match worker does; the seed must deliver samples meanwhile.
static int seedPool(const CameraProfile& cam, const SourceImage& src) {
  EditParams p;
  p.filmStock = "kodak_portra_400";  // a negative with its paper: the print medians are seeded
  p.exposure = 0.3f;
  FilmMatchCache cache;
  cache.proxyOwner = std::make_shared<SourceImage>(src);
  std::atomic<bool> holding{false}, release{false};
  std::thread holder([&] {  // a long job on the frame pool (stands for the frames' own jobs; the pool runs one job at a time)
    ThreadPool::instance().run(2, 1, [&](int, int) {
      holding.store(true);
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
  });
  while (!holding.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
  std::atomic<bool> matched{false};
  std::thread matcher([&] {  // the session's match worker: background pool
    ThreadPool::tlUse() = &ThreadPool::background();
    matchFilmExposureCached(p, cam, nullptr, "", src, cache, false);
    matched.store(true);
  });
  std::size_t samples = 0;
  while (ms() < 2000.0) {
    if (matched.load() && cache.seed) {
      std::lock_guard<std::mutex> lk(cache.seed->m);
      samples = cache.seed->samples.size();
    }
    if (samples >= 7) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double tSeed = ms();
  release.store(true);
  holder.join();
  matcher.join();
  std::printf("  seeding while the frame pool is busy: match %s, %zu of 7 medians after %.0f ms\n", matched.load() ? "done" : "blocked", samples, tSeed);
  CHECK(matched.load(), "the match on the background pool waited for the frame pool");
  CHECK(samples >= 7, "the median seeding waited for the frame pool (%zu of 7 medians in 2 s)", samples);
  if (cache.seed)  // let the detached seed finish before the process exits
    for (int i = 0; i < 2000; ++i) {
      {
        std::lock_guard<std::mutex> lk(cache.seed->m);
        if (cache.seed->samples.size() >= 7) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  const bool seedMode = argc > 1 && std::string(argv[1]) == "seedpool";
  if (seedMode) {  // the seeding is a timing dependent path: off in deterministic mode (read once, on first use)
#ifdef _WIN32
    _putenv_s("DAPPLE_DETERMINISTIC", "0");
#else
    setenv("DAPPLE_DETERMINISTIC", "0", 1);
#endif
  }
  std::string err;
  // pack folder: DAPPLE_FILM_DATA (a phone: the pushed folder holding dapple.dfp), else the source tree
  const char* envData = std::getenv("DAPPLE_FILM_DATA");
  const std::filesystem::path pack = (envData ? std::filesystem::path(envData) : std::filesystem::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film") / "dapple.dfp";
  if (!film::setLibraryPath(pack, &err)) return std::printf("SKIP: no film pack (%s)\n", err.c_str()), 77;
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  const SourceImage src = proxy(160, 107);
  if (seedMode) return seedPool(cam, src);
  FilmMatchOptions opt;  // Meter
  EditParams base;
  base.filmStock = "kodak_portra_400";

  // 1. exact == uncached
  struct Set {
    const char* name;
    EditParams p;
  };
  std::vector<Set> sets;
  {
    EditParams p = base;
    sets.push_back({"default", p});
    p = base, p.filmShoot = 0.6f;
    sets.push_back({"shoot 0.6", p});
    p = base, p.filmShoot = -0.8f, p.filmLatitude = -0.5f;
    sets.push_back({"shoot -0.8 latitude -0.5", p});
    p = base, p.filmPrint = 0.5f;
    sets.push_back({"print 0.5", p});
    p = base, p.filmPrint = -0.7f, p.filmWarmth = 25.f, p.exposure = 0.4f;
    sets.push_back({"print -0.7 warmth 25 exposure 0.4", p});
    p = base, p.filmStock = "fujifilm_velvia_100", p.filmShoot = 0.5f;
    sets.push_back({"slide shoot 0.5", p});
  }
  FilmMatchCache cache;
  for (const Set& s : sets) {
    const FilmExposure a = matchFilmExposure(s.p, cam, nullptr, src, nullptr, &opt);
    const FilmExposure b = matchFilmExposureCached(s.p, cam, nullptr, "", src, cache, true);
    std::printf("  %-34s uncached (%.4f, %.4f)  cached (%.4f, %.4f)\n", s.name, double(a.filmEv), double(a.printEv), double(b.filmEv), double(b.printEv));
    CHECK(std::fabs(a.filmEv - b.filmEv) < 1e-4f && std::fabs(a.printEv - b.printEv) < 1e-4f, "%s: cached exact match differs from matchFilmExposure", s.name);
  }

  // 2. a print drag: interpolated frames close to the exact solve, the settled frame identical to it
  {
    FilmMatchCache c2;
    float worst = 0.f;
    int interpolated = 0;
    for (int i = 0; i <= 20; ++i) {
      EditParams p = base;
      p.filmPrint = -1.f + 0.1f * float(i);
      const FilmExposure fast = matchFilmExposureCached(p, cam, nullptr, "", src, c2, false);
      if (c2.lastApprox) ++interpolated;
      const FilmExposure ref = matchFilmExposure(p, cam, nullptr, src, nullptr, &opt);
      worst = std::max(worst, std::fabs(fast.printEv - ref.printEv));
      CHECK(std::fabs(fast.filmEv - ref.filmEv) < 1e-4f, "print drag: film EV differs");
    }
    std::printf("  print drag: %d of 21 frames interpolated, worst print EV deviation %.4f EV, renders %d, interpolated lookups %d\n", interpolated, double(worst), c2.rendered,
                c2.interpolated);
    CHECK(worst < 0.08f, "interpolated print EV deviates by %.3f EV", double(worst));
    EditParams p = base;
    p.filmPrint = 0.37f;
    const FilmExposure settled = matchFilmExposureCached(p, cam, nullptr, "", src, c2, true);
    const FilmExposure ref = matchFilmExposure(p, cam, nullptr, src, nullptr, &opt);
    CHECK(std::fabs(settled.printEv - ref.printEv) < 1e-4f, "settled frame after the drag is not the exact solve");
  }

  // 3. the lean key
  {
    EditParams a = base, b = base;
    b.filmWarmth = 30.f, b.filmTint = -20.f;
    CHECK(filmMatchKeyLean(a) == filmMatchKeyLean(b), "warmth / tint must not be part of the key without film_print");
    a.filmPrint = b.filmPrint = 0.3f;
    CHECK(filmMatchKeyLean(a) != filmMatchKeyLean(b), "warmth / tint must be part of the key with film_print");
    EditParams c = base;
    c.contrast = 20.f;
    CHECK(filmMatchKeyLean(base) != filmMatchKeyLean(c), "a tonal edit must change the key");
    // wave 8: every input the solve reads is in both keys (vignette, nrColor, sharpRadius were not)
    for (int f = 0; f < 3; ++f) {
      EditParams v = base;
      if (f == 0) v.vignette = -30.f;
      if (f == 1) v.nrColor = 40.f;
      if (f == 2) v.sharpRadius = 2.f;
      CHECK(filmMatchKeyLean(base) != filmMatchKeyLean(v), "vignette / nrColor / sharpRadius must be part of the lean key");
      CHECK(filmMatchKey(base) != filmMatchKey(v), "vignette / nrColor / sharpRadius must be part of the legacy key");
    }
  }
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : 0;
}
