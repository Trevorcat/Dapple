// dapple gpu-bench <raw> [--synth W H] [--repeat N] [--no-full] [--no-tiled] [--seam] [--tile N] [--info] [--lut N]
//
// The GPU backend against the CPU baked path: Delta E 2000 (mean / p99 / max), per-pass GPU timings, upload and readback,
// at 1600 px, at the decoded full resolution and as a tiled export of a 61 MP picture (`--synth 9504 6336` builds one
// from the decoded photo). See docs/engine/gpu-backend.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace dapple::gpu {
using engine::GpuStats;
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}

void report(const char* what, const DeStats& s) {
  std::printf("      %-30s dE2000 mean %.4f  p99 %.3f  max %.2f   (pixels differing %.1f%%, max channel diff %d/255)\n", what, s.mean, s.p99, s.max,
              s.differing * 100.0, s.maxChan);
}

struct PassAgg {
  std::map<std::string, double> ms;
  void add(const GpuStats& st) {
    for (const auto& p : st.passTimes) ms[p.first] += p.second;
  }
  void print(int div) const {
    std::vector<std::pair<std::string, double>> v(ms.begin(), ms.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::printf("      passes (GPU ms, summed by kernel):");
    for (auto& p : v) std::printf("  %s %.2f", p.first.c_str(), p.second / div);
    std::printf("\n");
  }
};

struct Variant {
  const char* name;
  SpatialQuality q;
  bool noise;
  bool spatialParams;
};

}  // namespace

int gpuBench(const std::vector<std::string>& args) {
  std::string path;
  int repeat = 7, latticeN = 41, synthW = 0, synthH = 0, tile = 2048;
  bool noFull = false, noTiled = false, infoOnly = false, selfTest = true, seam = false;
  std::string only;  // --only nrl,nrc,dehaze,clarity,texture,sharpen,grain,vignette: just these spatial stages, one variant
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--repeat" && i + 1 < args.size()) repeat = std::atoi(args[++i].c_str());
    else if (args[i] == "--lut" && i + 1 < args.size()) latticeN = std::atoi(args[++i].c_str());
    else if (args[i] == "--tile" && i + 1 < args.size()) tile = std::atoi(args[++i].c_str());
    else if (args[i] == "--synth" && i + 2 < args.size()) {
      synthW = std::atoi(args[++i].c_str());
      synthH = std::atoi(args[++i].c_str());
    } else if (args[i] == "--no-full") noFull = true;
    else if (args[i] == "--no-tiled") noTiled = true;
    else if (args[i] == "--only" && i + 1 < args.size()) only = args[++i];
    else if (args[i] == "--seam") seam = true;
    else if (args[i] == "--no-selftest") selfTest = false;
    else if (args[i] == "--info") infoOnly = true;
    else path = args[i];
  }
  std::string err;
  engine::gpu::ContextOptions bo;
  bo.device.profile = true;
  bo.selfTest = selfTest;
  bo.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") && std::getenv("DAPPLE_GPU_SOFTWARE")[0] == '1';
  auto be = engine::gpu::Context::create(bo, err);
  if (!be) {
    std::fprintf(stderr, "gpu-bench: no usable GPU backend: %s\n", err.c_str());
    return 3;
  }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::printf("GPU: %s\n%sself-test: %s\n", be->deviceText().c_str(), be->caps().memoryTypes.c_str(), be->selfTestReport().c_str());
  if (infoOnly) return 0;
  if (path.empty()) {
    std::fputs("usage: dapple gpu-bench <raw> [--synth W H] [--repeat N] [--no-full] [--no-tiled] [--tile N] [--lut N] [--info]\n", stderr);
    return 2;
  }

  const fs::path p(std::u8string(path.begin(), path.end()));
  const auto decoders = DecoderRegistry::withBuiltins();
  const auto cameras = CameraRegistry::withBuiltins();
  const ImageDecoder* dec = decoders.find(p);
  if (!dec) {
    std::fputs("unsupported file\n", stderr);
    return 1;
  }
  auto t0 = Clock::now();
  auto full = dec->decode(p, DecodeOptions{noFull && noTiled ? DecodeQuality::Preview : DecodeQuality::Full});
  if (!full) {
    std::fprintf(stderr, "decode: %s\n", full.error().message.c_str());
    return 1;
  }
  std::printf("%s\ndecoded %dx%d in %.0f ms (CPU threads %u)\n", path.c_str(), full->linear.width, full->linear.height, msSince(t0),
              std::thread::hardware_concurrency());
  const CameraProfile& cam = cameras.resolve(full->meta.make, full->meta.model);

  EditParams prm = computeAutoAdjust(*full, cam).correction;
  prm.toneStrength = 60.f;
  prm.temperature = 0.f;
  prm.tint = 0.f;
  prm.contrast = 15;
  prm.highlights = -30;
  prm.shadows = 25;
  prm.vibrance = 20;
  prm.saturation = 8;
  prm.hsl[kOrange].saturation = 15;
  prm.gradeShadows = {220, 30, 0};
  prm.gradeHighlights = {40, 25, 0};
  EditParams prmSp = prm;  // plus every neighbourhood stage
  prmSp.nrLuma = 30;
  prmSp.nrColor = 30;
  prmSp.dehaze = 20;
  prmSp.clarity = 25;
  prmSp.texture = 20;
  prmSp.sharpen = 50;
  prmSp.sharpRadius = 12.f;
  prmSp.grain = 15;
  prmSp.vignette = -15;

  if (!only.empty()) {
    auto has = [&](const char* k) { return only.find(k) != std::string::npos; };
    if (!has("nrl")) prmSp.nrLuma = 0;
    if (!has("nrc")) prmSp.nrColor = 0;
    if (!has("dehaze")) prmSp.dehaze = 0;
    if (!has("clarity")) prmSp.clarity = 0;
    if (!has("texture")) prmSp.texture = 0;
    if (!has("sharpen")) prmSp.sharpen = 0;
    if (!has("grain")) prmSp.grain = 0;
    if (!has("vignette")) prmSp.vignette = 0;
  }

  struct Case {
    std::string name;
    SourceImage src;
  };
  std::vector<Case> cases;
  {
    Case c{"1600 px (CPU-downscaled source)", {}};
    c.src.sceneReferred = full->sceneReferred;
    c.src.meta = full->meta;
    c.src.linear = resizeToLongEdge(full->linear, 1600);
    cases.push_back(std::move(c));
  }
  SourceImage big;
  if (!noFull || !noTiled) {
    big.sceneReferred = full->sceneReferred;
    big.meta = full->meta;
    big.linear = std::move(full->linear);
  }
  if (!noFull) {
    Case c{"full resolution", {}};
    c.src.sceneReferred = big.sceneReferred;
    c.src.meta = big.meta;
    c.src.linear = big.linear;  // copy: the tiled case may replace `big`
    cases.push_back(std::move(c));
  }

  const Variant variants[] = {{"point-wise only", SpatialQuality::Off, true, false},
                              {"interactive (no NR / grain)", SpatialQuality::Interactive, false, true},
                              {"full quality (all stages)", SpatialQuality::Full, true, true}};

  const std::size_t variantFrom = only.empty() ? 0 : 2;
  for (Case& cs : cases) {
    const int W = cs.src.linear.width, H = cs.src.linear.height;
    const std::size_t npx = std::size_t(W) * H;
    std::printf("\n================ %s: %dx%d (%.1f MP) ================\n", cs.name.c_str(), W, H, double(npx) / 1e6);
    Rig rig(be, cs.src, cam);  // the engine session of this photo: Planner -> GpuExecutor
    for (std::size_t vi = variantFrom; vi < 3; ++vi) {
      const Variant& v = variants[vi];
      Renderer r(v.spatialParams ? prmSp : prm, cam, nullptr);
      FastOptions fo;
      fo.spatial = v.q;
      fo.noise = v.noise;
      t0 = Clock::now();
      const auto lut = r.bake(cs.src, latticeN);
      const double bakeMs = msSince(t0);
      int cw = 0, ch = 0;
      std::uint8_t* cpu = nullptr;
      const int cpuRep = npx > 8000000 ? 2 : std::min(repeat, 5);
      std::vector<double> cpuT;
      for (int i = 0; i < cpuRep; ++i) {
        std::free(cpu);
        t0 = Clock::now();
        cpu = r.renderBakedRgba(cs.src, *lut, fo, cw, ch);
        cpuT.push_back(msSince(t0));
      }
      std::printf("  --- %s ---\n      CPU baked path (all cores): %.1f ms/frame (lattice bake %.1f ms)\n", v.name, median(cpuT), bakeMs);
      // first frame: resident upload + lattice upload + pipeline creation
      int gw = 0, gh = 0;
      GpuStats st0;
      t0 = Clock::now();
      std::uint8_t* g0 = rig.render(v.spatialParams ? prmSp : prm, r, cs.src, fo, latticeN, gw, gh, &st0, err);
      const double first = msSince(t0);
      if (!g0) {
        std::printf("      GPU render failed: %s\n", err.c_str());
        std::free(cpu);
        continue;
      }
      std::free(g0);
      std::vector<double> wall, gpuT, rb, recT, subT, waT, trT;
      PassAgg agg;
      std::uint8_t* g = nullptr;
      for (int i = 0; i < repeat; ++i) {
        std::free(g);
        GpuStats st;
        g = rig.render(v.spatialParams ? prmSp : prm, r, cs.src, fo, latticeN, gw, gh, &st, err);
        if (!g) break;
        wall.push_back(st.wallMs - st.uploadMs);
        gpuT.push_back(st.gpuMs);
        rb.push_back(st.readbackMs);
        recT.push_back(st.recordMs);
        subT.push_back(st.submitMs);
        waT.push_back(st.waitMs);
        trT.push_back(st.trimMs);
        agg.add(st);
      }
      if (!g) {
        std::printf("      GPU render failed: %s\n", err.c_str());
        std::free(cpu);
        continue;
      }
      std::printf("      GPU first frame (source + lattice upload, pipeline build): %.1f ms\n", first);
      std::printf("      GPU frame, source resident: %.2f ms wall (GPU %.2f ms in %d passes / %d barriers, readback copy %.2f ms)   speed-up vs CPU %.1fx\n",
                  median(wall), median(gpuT), st0.passes, st0.barriers, median(rb), median(cpuT) / std::max(median(wall), 1e-3));
      if (g && v.q == SpatialQuality::Off) {
        std::vector<std::uint8_t> dst(std::size_t(gw) * gh * 4);
        const auto tm = Clock::now();
        std::memcpy(dst.data(), g, dst.size());
        std::printf("      reference: plain memcpy of the same %zu bytes between ordinary heap buffers: %.2f ms\n", dst.size(), msSince(tm));
      }
      std::printf("      host: record %.2f ms, submit %.2f ms, fence wait %.2f ms, copy-out %.2f ms, pool trim %.2f ms\n", median(recT), median(subT), median(waT), median(rb), median(trT));
      if (std::getenv("DAPPLE_BENCH_PASSES")) {
        GpuStats stp;
        std::free(rig.render(v.spatialParams ? prmSp : prm, r, cs.src, fo, latticeN, gw, gh, &stp, err));
        std::printf("      pass list (one frame):");
        for (const auto& pt : stp.passTimes) std::printf(" %s=%.2f", pt.first.c_str(), pt.second);
        std::printf(" ");
        std::printf(" ");
        std::putchar(10);
      }
      agg.print(repeat);
      if (cpu && cw == gw && ch == gh) report("GPU vs CPU baked path", compareRgba8(g, cpu, std::size_t(cw) * ch));
      std::free(g);
      std::free(cpu);
    }

    if (&cs == &cases.back() || cs.name.rfind("full", 0) == 0) {
      // preview from the resident full-resolution source: GPU Lanczos downscale to 1600 px
      if (W > 1700) {
        Renderer r(prmSp, cam, nullptr);
        FastOptions fo;
        fo.spatial = SpatialQuality::Interactive;
        fo.noise = false;
        int gw = 0, gh = 0;
        std::vector<double> wall;
        GpuStats st;
        t0 = Clock::now();
        std::uint8_t* o = rig.render(prmSp, r, cs.src, fo, latticeN, gw, gh, &st, err, 1600);
        const double firstMs = msSince(t0);
        std::free(o);
        for (int i = 0; i < repeat && o; ++i) {
          o = rig.render(prmSp, r, cs.src, fo, latticeN, gw, gh, &st, err, 1600);
          wall.push_back(st.wallMs - st.uploadMs);
          std::free(o);
        }
        std::printf("  --- preview 1600 px from the resident full source (Lanczos-3 on the GPU) ---\n      first frame (incl. resize pass): %.1f ms; later frames: %.2f ms (%dx%d)\n", firstMs, median(wall), gw, gh);
      }
    }
  }

  if (!noTiled) {
    SourceImage* src = &big;
    SourceImage synth;
    if (synthW > 0 && synthH > 0) {  // stand-in for a 61 MP body: the decoded picture tiled to WxH
      synth.sceneReferred = big.sceneReferred;
      synth.meta = big.meta;
      synth.linear = ImageF(synthW, synthH);
      for (int y = 0; y < synthH; ++y)
        for (int x = 0; x < synthW; ++x)
          std::memcpy(synth.linear.row(y) + 3 * x, big.linear.row(y % big.linear.height) + 3 * (x % big.linear.width), 12);
      src = &synth;
    }
    const int W = src->linear.width, H = src->linear.height;
    std::printf("\n================ tiled export: %dx%d (%.1f MP), tile %d + halo ================\n", W, H, double(W) * H / 1e6, tile);
    Rig rig(be, *src, cam);
    Renderer r(prmSp, cam, nullptr);
    FastOptions fo;
    const auto lut = r.bake(*src, latticeN);
    for (const bool stream : {true, false}) {
      if (!stream && std::uint64_t(W) * H > 40000000ull) continue;
      std::uint64_t sum = 0;
      std::size_t covered = 0;
      std::vector<std::uint8_t> kept;  // --seam: the whole tiled result, compared with a single-frame render
      if (seam && stream) kept.resize(std::size_t(W) * H * 4);
      auto sink = [&](int x, int y, int w, int h, const std::uint8_t* rgba, std::size_t stride) {
        for (int j = 0; j < h; j += 61) sum += rgba[std::size_t(j) * stride];
        if (!kept.empty())
          for (int j = 0; j < h; ++j) std::memcpy(kept.data() + (std::size_t(y + j) * W + x) * 4, rgba + std::size_t(j) * stride, std::size_t(w) * 4);
        covered += std::size_t(w) * h;
        return true;
      };
      GpuStats st;
      t0 = Clock::now();
      const bool ok = rig.exportTiled(prmSp, r, *src, 0, latticeN, tile, stream, 0.f, nullptr, sink, &st, err);
      std::printf("  %s source: %s  %.0f ms wall, GPU %.0f ms, %d passes, %d barriers, pool %.0f MB, %.1f MP/s (%zu px)\n",
                  stream ? "streamed" : "resident", ok ? "ok" : err.c_str(), msSince(t0), st.gpuMs, st.passes, st.barriers, double(be->pooledBytes()) / 1048576.0,
                  double(covered) / 1e3 / std::max(msSince(t0), 1.0), covered);
      (void)sum;
      if (!kept.empty() && ok) {
        // the same picture in one piece (resident source, whole-frame working images): the tiles must not show
        int gw = 0, gh = 0;
        GpuStats st1;
        t0 = Clock::now();
        std::uint8_t* whole = rig.render(prmSp, r, *src, fo, latticeN, gw, gh, &st1, err);
        if (whole && gw == W && gh == H) {
          std::printf("  single frame render (%.0f ms, pool %.0f MB):\n", msSince(t0), double(be->pooledBytes()) / 1048576.0);
          report("tiled vs single frame", compareRgba8(kept.data(), whole, std::size_t(W) * H));
        } else {
          std::printf("  single frame render failed: %s\n", err.c_str());
        }
        std::free(whole);
      }
    }
    // tiled vs CPU full-frame reference, on a size the CPU reference can hold
    if (std::uint64_t(W) * H <= 30000000ull) {
      int cw = 0, ch = 0;
      t0 = Clock::now();
      std::uint8_t* cpu = r.renderBakedRgba(*src, *lut, fo, cw, ch);
      std::printf("  CPU full-frame reference: %.0f ms\n", msSince(t0));
      std::vector<std::uint8_t> img(std::size_t(cw) * ch * 4);
      auto sink2 = [&](int x, int y, int w, int h, const std::uint8_t* rgba, std::size_t stride) {
        for (int j = 0; j < h; ++j) std::memcpy(img.data() + (std::size_t(y + j) * cw + x) * 4, rgba + std::size_t(j) * stride, std::size_t(w) * 4);
        return true;
      };
      GpuStats st;
      if (cpu && rig.exportTiled(prmSp, r, *src, 0, latticeN, tile, false, 0.f, nullptr, sink2, &st, err)) report("tiled GPU vs CPU full frame", compareRgba8(img.data(), cpu, std::size_t(cw) * ch));
      std::free(cpu);
    }
  }
  return 0;
}

}  // namespace dapple::gpu
