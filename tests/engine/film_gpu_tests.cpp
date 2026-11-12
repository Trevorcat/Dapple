// Film nodes on the GPU executor (step b4 (2)): per-node parity with the CPU nodes and GPU timings. Built only with -DDAPPLE_GPU=ON.
//   dapple_film_gpu_tests <id>     ids: point mixture grain chain bench     (exit 77: no Vulkan device)
// Budgets (docs 8.5): film point chain mean <= 0.1 / p99 <= 1.0 dE2000 vs the CPU baked path; Gaussian-mixture Blur <= 0.5 % of the
// dynamic range at hard edges; film spatial (grain off) mean <= 0.15 / p99 <= 1.5; grain (same seed) per-pixel agreement after a
// 4x linear box downscale mean <= 0.1 and flat-patch RMS within 3 % per channel; whole film frame mean <= 0.3 after 4x downscale.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

#include "colour_metrics.h"
#include "dapple/film.h"
#include "engine/exec/gpu_executor.h"
#include "engine/nodes/film/film_gpu.h"
#include "engine/nodes/film/film_node_internal.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::engine;
namespace ef = dapple::engine::filmnode;
namespace eg = dapple::engine::gpu;
namespace fd = dapple::film::detail;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::vector<hal::PassTime> g_lastPasses;
std::shared_ptr<film::FilmLibrary> g_lib;
std::shared_ptr<film::FilmLibrary> library() {
  if (!g_lib) {
    std::string err;
    const fs::path data = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film";
    const fs::path pack = data / "dapple.dfp";
    if (fs::exists(pack)) g_lib = film::FilmLibrary::loadPack(pack, &err);
    if (!g_lib) g_lib = film::FilmLibrary::loadJsonDir(data / "profiles", &err);
  }
  return g_lib;
}

std::uint32_t hash32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}
ImageF scene(int w, int h) {
  ImageF s(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float* p = s.row(y) + 3 * x;
      const float n = float(hash32(5u * 7919u + std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      p[0] = (0.05f + 0.95f * u * (1.f - 0.4f * v)) * (0.9f + 0.2f * n);
      p[1] = (0.04f + 0.7f * (1.f - u) * v + 0.15f * u) * (0.9f + 0.2f * n);
      p[2] = (0.03f + 0.8f * (1.f - v) * (1.f - 0.5f * u)) * (0.9f + 0.2f * n);
      if ((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f) < 0.01f) p[0] = 2.2f, p[1] = 1.9f, p[2] = 1.5f;
    }
  return s;
}

struct GpuCtx {
  std::unique_ptr<eg::Core> core;
  hal::Device& dev() { return core->dev(); }
};

// Source as RGBA32F image
bool uploadSource(eg::Core& core, const ImageF& im, hal::ImageH* out, std::string& err) {
  hal::Device& dev = core.dev();
  if (dev.createImage({hal::Format::RGBA32F, std::uint32_t(im.width), std::uint32_t(im.height), 1}, out) != hal::Status::Ok) return err = dev.lastError(), false;
  const std::size_t bytes = std::size_t(im.width) * im.height * 16;
  auto cl = dev.begin();
  if (!cl || !core.ensureStaging(cl->slot(), bytes, err)) return false;
  auto* dst = static_cast<float*>(core.slot(cl->slot()).staging.mapped);
  for (std::size_t i = 0; i < std::size_t(im.width) * im.height; ++i) {
    dst[4 * i] = im.data[3 * i];
    dst[4 * i + 1] = im.data[3 * i + 1];
    dst[4 * i + 2] = im.data[3 * i + 2];
    dst[4 * i + 3] = 1.f;
  }
  eg::GpuGraph g(dev, core.pool());
  g.copyBufferToImage(core.slot(cl->slot()).staging, g.external(*out), std::uint32_t(im.width), std::uint32_t(im.height), 1, 0);
  if (eg::runGraph(dev, g, *cl, nullptr, err) != hal::Status::Ok) return false;
  return dev.waitAll() == hal::Status::Ok;
}

// Runs the film passes and reads the display image back (RGB floats).
bool gpuFilm(eg::Core& core, const ef::FilmResolved& r, const ef::GpuFilmTables& tb, const ef::GpuFilmPlanRes& pr, const hal::ImageH& src, int w,
             int h, ImageF& out, double* gpuMs, std::string& err, int repeat = 1) {
  hal::Device& dev = core.dev();
  for (int rep = 0; rep < repeat; ++rep) {
    auto cl = dev.begin();
    if (!cl || !core.ensureReadback(cl->slot(), std::size_t(w) * h * 16, err)) return false;
    eg::GpuGraph g(dev, core.pool());
    eg::GpuRecorder rec(dev, g, err);
    rec.weights = core.slot(cl->slot()).weights;
    rec.W = w;
    rec.H_ = h;
    const eg::H sh = g.external(src);
    const eg::H o = ef::recordFilmFrame(rec, r, tb, pr, sh, w, h);
    if (o < 0) return false;
    g.copyImageToBuffer(o, core.slot(cl->slot()).readback, 0, 0, std::uint32_t(w), std::uint32_t(h));
    hal::FenceH f;
    const auto t0 = Clock::now();
    if (eg::runGraph(dev, g, *cl, &f, err) != hal::Status::Ok || dev.wait(f, 30ull * 1000 * 1000 * 1000) != hal::Status::Ok) return err = dev.lastError(), false;
    if (gpuMs) *gpuMs = core.statsOf(f).gpuMs > 0 ? core.statsOf(f).gpuMs : msSince(t0);
    g_lastPasses = core.statsOf(f).times;
    dev.invalidate(core.slot(cl->slot()).readback, 0, std::size_t(w) * h * 16);
    out = ImageF(w, h);
    const float* sp = static_cast<const float*>(core.slot(cl->slot()).readback.mapped);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
      out.data[3 * i] = sp[4 * i];
      out.data[3 * i + 1] = sp[4 * i + 1];
      out.data[3 * i + 2] = sp[4 * i + 2];
    }
  }
  return true;
}

struct DeStats {
  double mean = 0, p99 = 0, max = 0;
  int maxX = 0, maxY = 0;
};
DeStats deltaE(const ImageF& a, const ImageF& b) {
  std::vector<float> d;
  d.reserve(std::size_t(a.width) * a.height);
  double sum = 0, mx = 0;
  std::size_t mxi = 0;
  for (std::size_t i = 0; i < std::size_t(a.width) * a.height; ++i) {
    const float pa[3] = {a.data[3 * i], a.data[3 * i + 1], a.data[3 * i + 2]};
    const float pb[3] = {b.data[3 * i], b.data[3 * i + 1], b.data[3 * i + 2]};
    const double e = metrics::deltaE2000(metrics::srgbToLab(pa), metrics::srgbToLab(pb));
    d.push_back(float(e));
    sum += e;
    if (e > mx) {
      mx = e;
      mxi = i;
    }
  }
  {
    std::size_t over = 0, edge = 0;
    for (std::size_t i = 0; i < d.size(); ++i)
      if (d[i] > 0.5f) {
        ++over;
        const int x = int(i % a.width), y = int(i / a.width);
        edge += (x < 3 || y < 3 || x >= a.width - 3 || y >= a.height - 3);
      }
    if (over) std::printf("      %zu pixels above 0.5 dE (%zu within 3 px of the border)\n", over, edge);
  }
  std::sort(d.begin(), d.end());
  return {sum / d.size(), d[std::size_t(d.size() * 0.99)], mx, int(mxi % a.width), int(mxi / a.width)};
}

ef::FilmResolved resolved(const char* stock, int w, int h, int quality) {
  ef::FilmFrameArgs a;
  a.lib = library().get();
  a.stock = stock;
  const film::FilmStock* s = library()->find(stock);
  static std::string paper;
  paper = film::defaultPaperFor(*library(), stock);
  a.paper = paper.c_str();
  a.gain[0] = 1.3f;
  a.gain[1] = 1.0f;
  a.gain[2] = 1.6f;
  a.w = w;
  a.h = h;
  a.quality = quality;
  a.formatScale = 1.f;
  (void)s;
  ef::FilmResolved r;
  ef::filmFrameResolved(a, r);
  return r;
}

// 4x4 linear box downscale (the comparison used for the grain budgets)
ImageF box4(const ImageF& a) {
  ImageF o(a.width / 4, a.height / 4);
  for (int y = 0; y < o.height; ++y)
    for (int x = 0; x < o.width; ++x)
      for (int c = 0; c < 3; ++c) {
        double s = 0;
        for (int j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i) s += a.row(y * 4 + j)[3 * (x * 4 + i) + c];
        o.row(y)[3 * x + c] = float(s / 16);
      }
  return o;
}

struct CaseSpec {
  const char* name;
  const char* stock;
  int w, h;
  int quality;
  float grain, halation;
  double meanBudget, p99Budget;  // on the full-resolution comparison
  bool downscale;                // compare after a 4x box downscale instead (grain cases)
  bool keepGrain = false;        // interactive tier with the grain kept (grain-family drag)
  int fullLongEdge = 0;          // scale-aware grain against a full-resolution render of this long edge
};

void runCase(GpuCtx& G, const hal::ImageH& src, const ImageF& in, const CaseSpec& c) {
  std::string err;
  ef::FilmFrameArgs a;
  a.lib = library().get();
  a.stock = c.stock;
  const std::string paper = film::defaultPaperFor(*library(), c.stock);
  a.paper = paper.c_str();
  a.gain[0] = 1.3f, a.gain[1] = 1.0f, a.gain[2] = 1.6f;
  a.w = c.w;
  a.h = c.h;
  a.quality = c.quality;
  a.grainPct = c.grain;
  a.halationPct = c.halation;
  a.keepGrain = c.keepGrain;
  a.fullLongEdge = c.fullLongEdge;
  ef::FilmResolved r;
  ef::filmFrameResolved(a, r);
  ef::FilmTablesHandle tb;
  CHECK(ef::bakeFilmTables(r, tb, &err));
  ef::GpuFilmTables gt;
  ef::GpuFilmPlanRes pr;
  CHECK(ef::uploadGpuTables(*G.core, tb, gt, err));
  CHECK(ef::uploadGpuPlan(*G.core, r, pr, err));
  ImageF cpu(c.w, c.h), gpu;
  CHECK(ef::runFilmFrame(r, tb, in.data.data(), std::size_t(c.w) * 3, cpu.data.data(), std::size_t(c.w) * 3));
  double ms = 0;
  const bool ok = gpuFilm(*G.core, r, gt, pr, src, c.w, c.h, gpu, &ms, err);
  if (!ok || gpu.empty()) {
    std::printf("  %-34s GPU not available: %s\n", c.name, err.c_str());
    ++g_failures;
  } else {
    const DeStats d = c.downscale ? deltaE(box4(cpu), box4(gpu)) : deltaE(cpu, gpu);
    std::printf("  %-34s %s %dx%d: dE2000 mean %.4f p99 %.4f max %.3f   gpu %.2f ms\n", c.name, c.downscale ? "(4x box)" : "        ", c.w, c.h, d.mean,
                d.p99, d.max, ms);
    if (d.max > 2.0) {
      std::printf("      worst pixel at (%d, %d)\n", d.maxX, d.maxY);
      for (int dy = 0; dy < 3; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int x = std::clamp(d.maxX + dx, 0, c.w - 1), y = std::clamp(d.maxY + dy, 0, c.h - 1);
          const float* pc = cpu.row(y) + 3 * x;
          const float* pg = gpu.row(y) + 3 * x;
          std::printf("        (%d,%d) cpu %.4f %.4f %.4f  gpu %.4f %.4f %.4f\n", x, y, pc[0], pc[1], pc[2], pg[0], pg[1], pg[2]);
        }
    }
    CHECK(d.mean <= c.meanBudget && d.p99 <= c.p99Budget);
  }
  ef::releaseGpuTables(*G.core, gt);
  ef::releaseGpuPlan(*G.core, pr);
}

void testCases(GpuCtx& G, bool grainOnly) {
  const int w = 960, h = 640;
  const ImageF in = scene(w, h);
  hal::ImageH src;
  std::string err;
  CHECK(uploadSource(*G.core, in, &src, err));
  const CaseSpec cases[] = {
      // point chain: 0.1 / 1.0
      {"point chain portra400", "kodak_portra_400", w, h, 0, 0.f, 0.f, 0.1, 1.0, false},
      {"point chain velvia (scan)", "fujifilm_velvia_100", w, h, 0, 0.f, 0.f, 0.1, 1.0, false},
      {"point chain vision3 500t", "kodak_vision3_500t", w, h, 0, 0.f, 0.f, 0.1, 1.0, false},
      // spatial, grain off: 0.15 / 1.5 (halation + scatter, DIR, glare, scanner)
      {"halation (velvia scan, interactive)", "fujifilm_velvia_100", w, h, 1, 0.f, 100.f, 0.15, 1.5, false},
      {"halation + glare (portra, interactive)", "kodak_portra_400", w, h, 1, 0.f, 100.f, 0.15, 1.5, false},
      {"final tier, grain off (DIR, unsharp)", "kodak_portra_400", w, h, 2, 0.f, 100.f, 0.15, 1.5, false},
      {"final tier, grain off (ektar)", "kodak_ektar_100", w, h, 2, 0.f, 100.f, 0.15, 1.5, false},
      // grain on: the whole film frame, 0.3 after a 4x box downscale
      {"final tier with grain", "kodak_portra_400", w, h, 2, 100.f, 100.f, 0.3, 3.0, true},
      {"final tier with grain (slide)", "fujifilm_velvia_100", w, h, 2, 100.f, 100.f, 0.3, 3.0, true},
      // the same grain compared pixel by pixel (the hash is identical, so no downscale is needed to agree)
      {"final tier with grain, per pixel", "kodak_portra_400", w, h, 2, 100.f, 100.f, 0.1, 1.0, false},
      // grain-family drag at the interactive tier: grain kept, scale-aware against a 3x larger full-resolution render
      {"interactive grain drag, scale-aware", "kodak_portra_400", w, h, 1, 100.f, 100.f, 0.1, 1.0, false, true, 2880},
  };
  for (const CaseSpec& c : cases) {
    if (grainOnly && c.grain == 0.f) continue;
    runCase(G, src, in, c);
  }
  G.dev().destroy(src);
}

// A mixture of one plane: GPU vs the CPU engine, in units of the dynamic range of the plane.
void testMixture(GpuCtx& G) {
  const int w = 200, h = 150;
  std::vector<float> plane(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      plane[std::size_t(y) * w + x] = 0.2f + 0.5f * float(x) / w + 0.3f * float(y) / h + ((x > 120 && y < 40) ? 3.0f : 0.0f) + 0.05f * float(hash32(x * 7919u + y * 104729u) & 255) / 255.f;
  const fd::SpatialKernels& K = fd::selectSpatialKernels();
  struct M {
    const char* name;
    fd::Mixture m;
    bool half;
  };
  std::vector<M> ms;
  ms.push_back({"single sigma 1.8 (level 0)", fd::mixGauss(1.8), false});
  ms.push_back({"single sigma 6 (pyramid level 1)", fd::mixGauss(6.0), false});
  ms.push_back({"affine exp tail + gauss", fd::mixAffine(fd::mixConvolve(fd::mixExpTail(9.0), fd::mixGauss(2.0)), 0.3, 0.7), false});
  ms.push_back({"wide 40 px", fd::mixGauss(40.0), false});
  ms.push_back({"half-resolution input, sigma 12", fd::mixGauss(12.0), true});
  for (auto& c : ms) {
    fd::Arena arena;
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    std::vector<float> half(std::size_t(hw) * hh);
    for (int y = 0; y < hh; ++y)
      for (int x = 0; x < hw; ++x) {
        const int ya = std::min(2 * y, h - 1), yb = std::min(2 * y + 1, h - 1);
        float s = 0;
        int n = 0;
        for (int yy : {ya, yb})
          for (int xx : {2 * x, std::min(2 * x + 1, w - 1)}) {
            s += plane[std::size_t(yy) * w + xx];
            ++n;
          }
        half[std::size_t(y) * hw + x] = s / n;
      }
    std::vector<float> cpu(std::size_t(w) * h), gpu;
    fd::Plane dst{cpu.data(), w, h, w};
    if (c.half) {
      fd::Plane src{half.data(), hw, hh, hw};
      fd::applyMixtureHalf(K, arena, src, dst, c.m);
    } else {
      fd::Plane src{plane.data(), w, h, w};
      fd::applyMixture(K, arena, src, dst, c.m);
    }
    std::string err;
    if (!ef::gpuMixtureForTest(*G.core, c.half ? half.data() : plane.data(), w, h, &c.m, c.half, gpu, err)) {
      std::printf("  %s: %s\n", c.name, err.c_str());
      ++g_failures;
      continue;
    }
    if (std::string(c.name).find("sigma 1.8") != std::string::npos) {
      // brute-force reference with reflect boundary, double precision
      std::vector<float> w1; int r1 = 0; fd::gaussTaps(1.8, w1, r1);
      auto refl = [](int i, int n) { const int per = 2 * n; i %= per; if (i < 0) i += per; return i < n ? i : per - 1 - i; };
      for (int y : {0, 1, 2, 70}) {
        const int x = 150;
        double acc = 0;
        for (int dy = -r1; dy <= r1; ++dy)
          for (int dx = -r1; dx <= r1; ++dx)
            acc += double(w1[std::abs(dx)]) * w1[std::abs(dy)] * plane[std::size_t(refl(y + dy, h)) * w + refl(x + dx, w)];
        std::printf("      y=%d x=%d: reference %.5f cpu %.5f gpu %.5f\n", y, x, acc, cpu[std::size_t(y) * w + x], gpu[std::size_t(y) * w + x]);
      }
    }
    double worst = 0, worstEdge = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const double e = std::fabs(double(cpu[std::size_t(y) * w + x]) - gpu[std::size_t(y) * w + x]);
        worst = std::max(worst, e);
        if (x < 3 || y < 3 || x >= w - 3 || y >= h - 3) worstEdge = std::max(worstEdge, e);
      }
    std::printf("  %-36s max abs diff %.3g (border band %.3g), plane range 3.5 => %.3f %%\n", c.name, worst, worstEdge, 100.0 * worst / 3.5);
    CHECK(worst / 3.5 < 0.005);
  }
}

// Steady-state timings of the film frame on this GPU: per-pass GPU time (timestamps) and wall time of submit + wait.
void testBench(GpuCtx& G) {
  struct B {
    const char* name;
    const char* stock;
    int w, h, quality;
    bool keepGrain;
  };
  const B bs[] = {{"1600 px, final tier (all effects)", "kodak_portra_400", 1600, 1067, 2, false},
                  {"1600 px, interactive tier", "kodak_portra_400", 1600, 1067, 1, false},
                  {"1600 px, interactive, grain drag", "kodak_portra_400", 1600, 1067, 1, true},
                  {"1080 px, interactive tier", "kodak_portra_400", 1080, 720, 1, false},
                  {"3600 px (8.6 MP), final tier", "kodak_portra_400", 3600, 2400, 2, false}};
  for (const B& b : bs) {
    const ImageF in = scene(b.w, b.h);
    hal::ImageH src;
    std::string err;
    CHECK(uploadSource(*G.core, in, &src, err));
    ef::FilmFrameArgs a;
    a.lib = library().get();
    a.stock = b.stock;
    const std::string paper = film::defaultPaperFor(*library(), b.stock);
    a.paper = paper.c_str();
    a.gain[0] = 1.3f, a.gain[1] = 1.0f, a.gain[2] = 1.6f;
    a.w = b.w;
    a.h = b.h;
    a.quality = b.quality;
    a.keepGrain = b.keepGrain;
    a.fullLongEdge = b.keepGrain ? b.w * 3 : 0;
    ef::FilmResolved r;
    ef::filmFrameResolved(a, r);
    ef::FilmTablesHandle tb;
    CHECK(ef::bakeFilmTables(r, tb, &err));
    ef::GpuFilmTables gt;
    ef::GpuFilmPlanRes pr;
    CHECK(ef::uploadGpuTables(*G.core, tb, gt, err) && ef::uploadGpuPlan(*G.core, r, pr, err));
    std::vector<double> gpuMs, wall;
    ImageF out;
    for (int i = 0; i < 12; ++i) {
      double ms = 0;
      const auto t0 = Clock::now();
      if (!gpuFilm(*G.core, r, gt, pr, src, b.w, b.h, out, &ms, err)) {
        std::printf("  %s: %s\n", b.name, err.c_str());
        ++g_failures;
        break;
      }
      if (i >= 2) {
        gpuMs.push_back(ms);
        wall.push_back(msSince(t0));
      }
    }
    if (!g_lastPasses.empty() && b.w == 1600 && b.quality == 2) {
      std::map<std::string, std::pair<double, int>> agg;
      for (const auto& t : g_lastPasses) {
        agg[t.name].first += t.ms;
        ++agg[t.name].second;
      }
      std::vector<std::pair<double, std::string>> v;
      for (auto& kv : agg) v.push_back({kv.second.first, kv.first + " x" + std::to_string(kv.second.second)});
      std::sort(v.rbegin(), v.rend());
      std::printf("      passes (last frame):");
      for (std::size_t i = 0; i < std::min<std::size_t>(v.size(), 8); ++i) std::printf(" %s %.2f ms;", v[i].second.c_str(), v[i].first);
      std::printf("\n");
    }
    if (!gpuMs.empty()) {
      std::sort(gpuMs.begin(), gpuMs.end());
      std::sort(wall.begin(), wall.end());
      std::printf("  %-36s GPU %.2f ms (median of %zu%s), wall incl. readback %.2f ms\n", b.name, gpuMs[gpuMs.size() / 2], gpuMs.size(),
                  G.dev().caps().timestamps ? ", timestamps" : "", wall[wall.size() / 2]);
    }
    ef::releaseGpuTables(*G.core, gt);
    ef::releaseGpuPlan(*G.core, pr);
    G.dev().destroy(src);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (!library()) return 77;
  hal::DeviceOptions opt;
  opt.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") != nullptr;
  opt.profile = std::getenv("DAPPLE_GPU_PROFILE") != nullptr;
  std::string err;
  GpuCtx G;
  G.core = eg::Core::create(opt, err);
  if (!G.core) {
    std::printf("no usable GPU: %s\n", err.c_str());
    return 77;
  }
  std::printf("device: %s\n", G.dev().caps().text().c_str());
  const std::string id = argc >= 2 ? argv[1] : "";
  if (id == "mixture") testMixture(G);
  else if (id == "bench") testBench(G);
  else if (id.empty() || id == "all") testCases(G, false);
  else if (id == "grain") testCases(G, true);
  std::printf(g_failures ? "FAILED: %d check(s)\n" : "ok\n", g_failures);
  return g_failures ? 1 : 0;
}
