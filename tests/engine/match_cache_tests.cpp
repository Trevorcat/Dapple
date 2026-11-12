// The cached film exposure match (core/src/film_match_cache.cpp, docs/engine/pipeline-architecture.md section 26.2):
//   * with exact = true it returns what matchFilmExposure returns,
//   * a print drag with exact = false (interpolated medians) stays close to the exact solve, and the settled (exact) frame equals it,
//   * the lean key ignores print warmth / tint only when no median of the print is evaluated.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "dapple/camera.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/renderer.h"

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

int main() {
  std::string err;
  const std::filesystem::path pack = std::filesystem::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film" / "dapple.dfp";
  if (!film::setLibraryPath(pack, &err)) return std::printf("SKIP: no film pack (%s)\n", err.c_str()), 77;
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("", "");
  const SourceImage src = proxy(160, 107);
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
  }
  std::printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail);
  return g_fail ? 1 : 0;
}
