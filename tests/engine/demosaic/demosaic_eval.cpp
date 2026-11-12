// Quality and speed evaluation of the Demosaic node against LibRaw's own output on real RAW files (docs/engine/pipeline-architecture.md 32).
//   dapple_demosaic_eval <file.raw> [--out dir] [--tag name] [--crops N] [--bilinear] [--no-ref] [--threads N] [--cache]
// Prints one summary line per file (machine readable "RESULT ..." line) and, with --out, writes a contact sheet and 200 % crops
// (JPEG, sRGB, side by side: LibRaw | node | difference x4). Only for the public sample files: nothing here leaves build/.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <stb_image_write.h>

#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic_desc.h"
#include "engine/nodes/demosaic/demosaic_session.h"
#ifdef DAPPLE_HAVE_GPU
#include "engine/hal/select.h"
#include "engine/nodes/demosaic/demosaic_gpu.h"
#endif

using namespace dapple;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace {
void printPeak() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) std::printf("  PEAK working set %.0f MB\n", double(pmc.PeakWorkingSetSize) / 1048576.0);
#endif
}
constexpr double M_PI_ = 3.14159265358979323846;
struct Lab { float L, a, b; };

// linear ProPhoto (D50) -> Lab (D50), after the exposure gain `g` (scene -> viewing exposure)
Lab toLab(const float* p, float g) {
  const float r = std::clamp(p[0] * g, 0.f, 4.f), gg = std::clamp(p[1] * g, 0.f, 4.f), b = std::clamp(p[2] * g, 0.f, 4.f);
  const float X = 0.7977f * r + 0.1352f * gg + 0.0313f * b, Y = 0.2880f * r + 0.7119f * gg + 0.0001f * b, Z = 0.8249f * b;
  auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
  const float fx = f(X / 0.9642f), fy = f(Y), fz = f(Z / 0.8249f);
  return {116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz)};
}

double dE2000(const Lab& p, const Lab& q) {
  const double L1 = p.L, a1 = p.a, b1 = p.b, L2 = q.L, a2 = q.a, b2 = q.b;
  const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2), Cm = 0.5 * (C1 + C2);
  const double G = 0.5 * (1.0 - std::sqrt(std::pow(Cm, 7) / (std::pow(Cm, 7) + std::pow(25.0, 7))));
  const double ap1 = (1 + G) * a1, ap2 = (1 + G) * a2, Cp1 = std::hypot(ap1, b1), Cp2 = std::hypot(ap2, b2);
  auto hue = [](double b, double a) { double h = std::atan2(b, a) * 180.0 / M_PI_; return h < 0 ? h + 360 : h; };
  const double hp1 = Cp1 == 0 ? 0 : hue(b1, ap1), hp2 = Cp2 == 0 ? 0 : hue(b2, ap2);
  const double dL = L2 - L1, dC = Cp2 - Cp1;
  double dh = 0;
  if (Cp1 * Cp2 != 0) {
    dh = hp2 - hp1;
    if (dh > 180) dh -= 360;
    else if (dh < -180) dh += 360;
  }
  const double dH = 2 * std::sqrt(Cp1 * Cp2) * std::sin(dh * M_PI_ / 360.0);
  const double Lm = 0.5 * (L1 + L2), Cpm = 0.5 * (Cp1 + Cp2);
  double hm = hp1 + hp2;
  if (Cp1 * Cp2 == 0) hm = hp1 + hp2;
  else if (std::fabs(hp1 - hp2) <= 180) hm *= 0.5;
  else hm = (hp1 + hp2 < 360) ? 0.5 * (hm + 360) : 0.5 * (hm - 360);
  const double T = 1 - 0.17 * std::cos((hm - 30) * M_PI_ / 180) + 0.24 * std::cos(2 * hm * M_PI_ / 180) + 0.32 * std::cos((3 * hm + 6) * M_PI_ / 180) - 0.20 * std::cos((4 * hm - 63) * M_PI_ / 180);
  const double dth = 30 * std::exp(-std::pow((hm - 275) / 25, 2));
  const double Rc = 2 * std::sqrt(std::pow(Cpm, 7) / (std::pow(Cpm, 7) + std::pow(25.0, 7)));
  const double Sl = 1 + 0.015 * std::pow(Lm - 50, 2) / std::sqrt(20 + std::pow(Lm - 50, 2)), Sc = 1 + 0.045 * Cpm, Sh = 1 + 0.015 * Cpm * T;
  const double Rt = -std::sin(2 * dth * M_PI_ / 180) * Rc;
  return std::sqrt(std::pow(dL / Sl, 2) + std::pow(dC / Sc, 2) + std::pow(dH / Sh, 2) + Rt * (dC / Sc) * (dH / Sh));
}

struct Stats { double mean = 0, p99 = 0, max = 0; };
Stats statsOf(std::vector<float>& v) {
  Stats s;
  if (v.empty()) return s;
  double sum = 0;
  for (float x : v) sum += x;
  s.mean = sum / double(v.size());
  const std::size_t k = std::min(v.size() - 1, std::size_t(0.99 * double(v.size())));
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
  s.p99 = v[k];
  s.max = *std::max_element(v.begin(), v.end());
  return s;
}

float lumaOf(const float* p) { return 0.2880f * p[0] + 0.7119f * p[1] + 0.0001f * p[2]; }

// exposure gain that puts the median luma at 0.18
float exposureGain(const ImageF& im) {
  std::vector<float> l;
  for (int y = 0; y < im.height; y += 8)
    for (int x = 0; x < im.width; x += 8) l.push_back(lumaOf(im.row(y) + 3 * x));
  if (l.empty()) return 1.f;
  std::nth_element(l.begin(), l.begin() + std::ptrdiff_t(l.size() / 2), l.end());
  return 0.18f / std::max(l[l.size() / 2], 1e-4f);
}

// sRGB 8-bit from linear ProPhoto (D50) with exposure gain g
void toSrgb8(const float* p, float g, std::uint8_t* o) {
  const float r = p[0] * g, gg = p[1] * g, b = p[2] * g;
  const float X = 0.7977f * r + 0.1352f * gg + 0.0313f * b, Y = 0.2880f * r + 0.7119f * gg + 0.0001f * b, Z = 0.8249f * b;
  // XYZ D50 -> linear sRGB (Bradford adapted)
  const float R = 3.1339f * X - 1.6169f * Y - 0.4906f * Z, G = -0.9788f * X + 1.9161f * Y + 0.0335f * Z, B = 0.0719f * X - 0.2290f * Y + 1.4052f * Z;
  auto enc = [](float v) {
    v = std::clamp(v, 0.f, 1.f);
    return std::uint8_t(255.f * (v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f) + 0.5f);
  };
  o[0] = enc(R), o[1] = enc(G), o[2] = enc(B);
}

// Metrics of one picture on a mask of pixels
struct Zip { double flat = 0, edge = 0, flatN = 0, edgeN = 0; };
// Nyquist chroma energy: |2c - c(x-1) - c(x+1)| and the vertical equivalent on chroma = (R - G, B - G) / (G + eps), averaged over flat / edge pixels of the reference
Zip zipper(const ImageF& im, const ImageF& ref) {
  Zip z;
  const int W = im.width, H = im.height;
  for (int y = 2; y < H - 2; y += 2)
    for (int x = 2; x < W - 2; x += 2) {
      const float* r = ref.row(y) + 3 * x;
      const float gx = std::fabs(lumaOf(r + 3) - lumaOf(r - 3)) + std::fabs(lumaOf(ref.row(y + 1) + 3 * x) - lumaOf(ref.row(y - 1) + 3 * x));
      const float lr = lumaOf(r);
      if (lr < 0.02f) continue;
      const float rel = gx / lr;
      auto chroma = [&](int xx, int yy, int c) { const float* p = im.row(yy) + 3 * xx; return (p[c] - p[1]) / (p[1] + 0.01f); };
      double e = 0;
      for (int c : {0, 2}) {
        e += std::fabs(2 * chroma(x, y, c) - chroma(x - 1, y, c) - chroma(x + 1, y, c));
        e += std::fabs(2 * chroma(x, y, c) - chroma(x, y - 1, c) - chroma(x, y + 1, c));
      }
      if (rel < 0.05f) z.flat += e, z.flatN += 1;
      else if (rel > 0.4f) z.edge += e, z.edgeN += 1;
    }
  z.flat /= std::max(1.0, z.flatN);
  z.edge /= std::max(1.0, z.edgeN);
  return z;
}

void writeJpeg(const std::string& path, const std::vector<std::uint8_t>& rgb, int w, int h) {
  stbi_write_jpg(path.c_str(), w, h, 3, rgb.data(), 92);
}

// crop (x, y, w, h) of an ImageF as 8-bit at 2x nearest
void blit2x(std::vector<std::uint8_t>& dst, int dstW, int ox, int oy, const ImageF& im, int cx, int cy, int cw, int ch, float g, int scale) {
  for (int y = 0; y < ch * scale; ++y)
    for (int x = 0; x < cw * scale; ++x) {
      const float* p = im.row(std::min(im.height - 1, cy + y / scale)) + 3 * std::min(im.width - 1, cx + x / scale);
      toSrgb8(p, g, &dst[(std::size_t(oy + y) * dstW + (ox + x)) * 3]);
    }
}

void blitDiff(std::vector<std::uint8_t>& dst, int dstW, int ox, int oy, const ImageF& a, const ImageF& b, int cx, int cy, int cw, int ch, float g, int scale) {
  for (int y = 0; y < ch * scale; ++y)
    for (int x = 0; x < cw * scale; ++x) {
      const int sx = std::min(a.width - 1, cx + x / scale), sy = std::min(a.height - 1, cy + y / scale);
      const float* p = a.row(sy) + 3 * sx;
      const float* q = b.row(sy) + 3 * sx;
      std::uint8_t* o = &dst[(std::size_t(oy + y) * dstW + (ox + x)) * 3];
      for (int c = 0; c < 3; ++c) o[c] = std::uint8_t(std::clamp(128.f + 4.f * 255.f * (p[c] - q[c]) * g * 0.5f, 0.f, 255.f));
    }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dapple_demosaic_eval <file> [--out dir] [--tag name] [--crops N] [--bilinear] [--no-ref] [--threads N] [--cache]\n");
    return 2;
  }
  std::string file = argv[1], outDir, tag;
  int crops = 4, threads = 0;
  bool bil = false, noRef = false, cache = false;
  int atX = -1, atY = -1, atS = 4, gpuTile = 0;
  bool proxyTiming = false, gpuOnly = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    else if (a == "--tag" && i + 1 < argc) tag = argv[++i];
    else if (a == "--crops" && i + 1 < argc) crops = std::atoi(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
    else if (a == "--bilinear") bil = true;
    else if (a == "--no-ref") noRef = true;
    else if (a == "--cache") cache = true;
    else if (a == "--proxy") proxyTiming = true;
    else if (a == "--gpu-only") gpuOnly = true;
    else if (a == "--gpu" && i + 1 < argc) gpuTile = std::atoi(argv[++i]);
    else if (a == "--at" && i + 1 < argc) std::sscanf(argv[++i], "%d,%d,%d", &atX, &atY, &atS);
  }
#if defined(_WIN32)
  _putenv_s("DAPPLE_ANCHOR", "off");
  _putenv_s("DAPPLE_SHADOW_CHROMA", "off");
#else
  setenv("DAPPLE_ANCHOR", "off", 1);
  setenv("DAPPLE_SHADOW_CHROMA", "off", 1);
#endif
  const std::filesystem::path path = std::filesystem::path(file);
  if (tag.empty()) tag = path.stem().string();

  auto t = Clock::now();
  auto mr = decodeRawMosaic(path);
  const double tMosaic = msSince(t);
  if (!mr) {
    std::printf("RESULT %s error %s\n", tag.c_str(), mr.error().message.c_str());
    return 1;
  }
  MosaicImage& m = *mr;
  if (m.cfa == CfaKind::None) {
    std::printf("RESULT %s nomosaic reason=\"%s\" mosaic_ms=%.0f\n", tag.c_str(), m.reason.c_str(), tMosaic);
    return 0;
  }
  std::printf("%s: %s %dx%d (%.1f MP) period %d, mosaic decode %.0f ms, wb %.3f %.3f %.3f %.3f, black %.0f white %.0f flip %d crop %d\n", tag.c_str(), m.cfa == CfaKind::XTrans ? "X-Trans" : "Bayer", m.width, m.height,
              double(m.width) * m.height / 1e6, m.period, tMosaic, m.wb[0], m.wb[1], m.wb[2], m.wb[3], m.blackMin, m.white, m.flip, int(m.hasCrop));
  if (proxyTiming) {  // the first picture and the visible region: binned half size, 1920 x 1080 regions at 1:1 (CPU, 28 threads / 1 thread)
    t = Clock::now();
    ImageF half = demosaic::binnedHalf(m);
    const double tHalf = msSince(t);
    int ow, oh;
    demosaic::orientedSize(m, &ow, &oh);
    const int rx = std::max(0, ow / 2 - 960), ry = std::max(0, oh / 2 - 540);
    t = Clock::now();
    ImageF roiN = demosaic::demosaicOrientedRegion(m, rx, ry, rx + 1920, ry + 1080, 0);
    const double tRoiN = msSince(t);
    t = Clock::now();
    ImageF roi1 = demosaic::demosaicOrientedRegion(m, rx, ry, rx + 1920, ry + 1080, 1);
    const double tRoi1 = msSince(t);
    std::printf("  PROXY binned half size (%dx%d, 1 thread) %.0f ms | ROI 1920x1080 at 1:1: CPU %d threads %.1f ms, 1 thread %.1f ms\n", half.width, half.height, tHalf, int(std::thread::hardware_concurrency()), tRoiN, tRoi1);
  }
#ifdef DAPPLE_HAVE_GPU
  if (gpuTile > 0) {  // GPU timing of the node on the whole frame: compute only (result stays on the device) and with the read-back
    dapple::hal::DeviceOptions dopt;
    if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) dopt.allowSoftware = v[0] == '1';
    auto sel = dapple::hal::selectDevice(dopt);
    if (sel.device) {
      std::string gerr;
      const demosaic::Desc d = demosaic::makeDesc(m);
      for (int half = 0; half < 2; ++half) {
        auto gpu = demosaic::GpuDemosaic::create(*sel.device, half != 0, gerr);
        demosaic::GpuStats st;
        gpu->run(d, {0, 0, m.width, m.height}, nullptr, 0, gpuTile, &st);  // warm-up (pipelines, allocation)
        const double devMB = double(st.deviceBytes) / 1048576.0;
        double best = 1e30, bestPrep = 0, bestGpu = 0;
        for (int rep = 0; rep < 3; ++rep) {
          gpu->run(d, {0, 0, m.width, m.height}, nullptr, 0, gpuTile, &st);
          if (st.totalMs < best) best = st.totalMs, bestPrep = st.prepMs, bestGpu = st.gpuMs;
        }
        std::vector<float> rb(std::size_t(m.width) * m.height * 3);
        t = Clock::now();
        gpu->run(d, {0, 0, m.width, m.height}, rb.data(), std::size_t(m.width) * 3, gpuTile, &st);
        std::printf("  GPU %s tile %d (%d tiles): compute-only %.0f ms (host window build %.0f, device %.0f) | with read-back to host %.0f ms (conversion %.0f) | device memory %.0f MB | %s\n", half ? "RGBA16F" : "RGBA32F", gpuTile, st.tiles, best, bestPrep, bestGpu, msSince(t), st.readbackMs,
                    devMB, sel.device->caps().name.c_str());
        if (half) {  // the visible region at 1:1 (1920 x 1080 of the sensor, no orientation), read back to the host
          const demosaic::Rect roi{m.width / 2 - 960, m.height / 2 - 540, m.width / 2 + 960, m.height / 2 + 540};
          std::vector<float> rr(std::size_t(roi.w()) * roi.h() * 3);
          gpu->run(d, roi, rr.data(), std::size_t(roi.w()) * 3, gpuTile, &st);
          double bestR = 1e30;
          for (int rep = 0; rep < 5; ++rep) {
            gpu->run(d, roi, rr.data(), std::size_t(roi.w()) * 3, gpuTile, &st);
            bestR = std::min(bestR, st.totalMs);
          }
          std::printf("  GPU ROI 1920x1080 at 1:1 (tile %d, %d tile(s)): %.1f ms with read-back to the host\n", gpuTile, st.tiles, bestR);
        }
      }
    } else {
      std::printf("  GPU: no device\n");
    }
  }
#endif
  if (gpuOnly) {
    printPeak();
    return 0;
  }
  t = Clock::now();
  ImageF ours = demosaic::demosaicFull(m, threads);
  const double tDemo = msSince(t);
  ImageF base;
  double tBil = 0;
  if (bil) {
    t = Clock::now();
    const demosaic::Desc d = demosaic::makeDesc(m);
    ImageF b(m.width, m.height);
    demosaic::bilinear(d, {0, 0, m.width, m.height}, b.data.data(), std::size_t(m.width) * 3, threads);
    base = finishMosaicRgb(m, std::move(b));
    tBil = msSince(t);
  }
  double tRef = 0;
  ImageF ref;
  if (!noRef) {
    t = Clock::now();
    DecodeOptions o;
    o.quality = DecodeQuality::Full;
    auto sr = makeLibRawDecoder()->decode(path, o);
    tRef = msSince(t);
    if (!sr) {
      std::printf("RESULT %s error libraw %s\n", tag.c_str(), sr.error().message.c_str());
      return 1;
    }
    ref = std::move(sr->linear);
  }
  std::printf("  demosaic %.0f ms (%d threads), libraw full decode %.0f ms%s\n", tDemo, threads ? threads : int(std::thread::hardware_concurrency()), tRef, bil ? "" : "");
  if (noRef) {
    std::printf("RESULT %s ok demosaic_ms=%.0f mosaic_ms=%.0f\n", tag.c_str(), tDemo, tMosaic);
    printPeak();
    return 0;
  }
  if (ref.width != ours.width || ref.height != ours.height) {
    std::printf("RESULT %s sizemismatch ours=%dx%d libraw=%dx%d\n", tag.c_str(), ours.width, ours.height, ref.width, ref.height);
    return 1;
  }
  const int W = ours.width, H = ours.height;
  const float g = exposureGain(ref);
  // full frame dE (every 2nd pixel in each direction), borders (24 px) separately
  std::vector<float> all, border;
  double meanLumaRatio = 0, nl = 0;
  for (int y = 0; y < H; y += 2) {
    for (int x = 0; x < W; x += 2) {
      const float* p = ours.row(y) + 3 * x;
      const float* q = ref.row(y) + 3 * x;
      const float de = float(dE2000(toLab(p, g), toLab(q, g)));
      (x < 24 || y < 24 || x >= W - 24 || y >= H - 24 ? border : all).push_back(de);
      if (lumaOf(q) > 0.02f) meanLumaRatio += double(lumaOf(p)) / lumaOf(q), nl += 1;
    }
  }
  {  // per-channel mean of the two pictures: a gain / white balance mismatch shows here
    double sa[3] = {0, 0, 0}, sb[3] = {0, 0, 0};
    for (int y = 0; y < H; y += 4)
      for (int x = 0; x < W; x += 4)
        for (int c = 0; c < 3; ++c) sa[c] += ours.row(y)[3 * x + c], sb[c] += ref.row(y)[3 * x + c];
    std::printf("  channel means ours/libraw: R %.4f G %.4f B %.4f\n", sa[0] / sb[0], sa[1] / sb[1], sa[2] / sb[2]);
  }
  Stats sAll = statsOf(all), sBorder = statsOf(border);
  // high-detail crops: the 64 x 64 blocks of the reference with the largest luma gradient energy, evaluated at full resolution
  struct Blk { int x, y; double e; };
  std::vector<Blk> blocks;
  const int B = 64;
  for (int by = 16; by + B + 16 <= H; by += B)
    for (int bx = 16; bx + B + 16 <= W; bx += B) {
      double e = 0, lm = 0;
      for (int y = by; y < by + B; y += 2)
        for (int x = bx; x < bx + B; x += 2) {
          const float* p = ref.row(y) + 3 * x;
          e += std::fabs(lumaOf(p + 6) - lumaOf(p)) + std::fabs(lumaOf(ref.row(y + 2) + 3 * x) - lumaOf(p));
          lm += lumaOf(p);
        }
      lm /= (B / 2) * (B / 2);
      if (lm > 0.02 && lm < 0.8) blocks.push_back({bx, by, e / std::max(lm, 0.02)});  // relative contrast of unsaturated blocks
    }
  std::sort(blocks.begin(), blocks.end(), [](const Blk& a, const Blk& b) { return a.e > b.e; });
  const std::size_t nTop = std::max<std::size_t>(1, blocks.size() / 50);  // the top 2 %
  std::vector<float> det;
  for (std::size_t i = 0; i < std::min(nTop, blocks.size()); ++i)
    for (int y = blocks[i].y; y < blocks[i].y + B; ++y)
      for (int x = blocks[i].x; x < blocks[i].x + B; ++x) det.push_back(float(dE2000(toLab(ours.row(y) + 3 * x, g), toLab(ref.row(y) + 3 * x, g))));
  Stats sDet = statsOf(det);
  // highlight pixels: luma of the reference above 0.9 of white: hue shift (our chroma vs reference) and fraction of magenta cast pixels
  std::vector<float> hl;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      const float* q = ref.row(y) + 3 * x;
      if (std::max({q[0], q[1], q[2]}) >= 0.98f) hl.push_back(float(dE2000(toLab(ours.row(y) + 3 * x, g), toLab(q, g))));
    }
  Stats sHl = statsOf(hl);
  if (const char* dp = std::getenv("DEMO_PIX")) {  // DEMO_PIX=x,y : 12 x 6 pixels around it: scaled mosaic sample, ours, libraw (G channel and the sample's own channel)
    int px = 0, py = 0;
    std::sscanf(dp, "%d,%d", &px, &py);
    const demosaic::Desc dd = demosaic::makeDesc(m);
    for (int y = py; y < py + 6; ++y) {
      for (int x = px; x < px + 12; ++x) {
        const int c = dd.cfa3[(y % dd.period) * dd.period + x % dd.period];
        std::printf(" %c%5.3f/%5.3f/%5.3f", "RGB"[c], demosaic::scaleSample(dd, x, y), ours.row(y)[3 * x + 1], ref.row(y)[3 * x + 1]);
      }
      std::printf("\n");
    }
  }
  if (std::getenv("DEMO_DEBUG") && m.flip == 0 && !m.hasCrop) {  // channel bias in the shadows by site type (mosaic coordinates = picture coordinates)
    double sum[3][2] = {}, cnt[3][2] = {};
    for (int y = 8; y < H - 8; y += 7)
      for (int x = 8; x < W - 8; x += 1) {
        const float* q = ref.row(y) + 3 * x;
        const float* p = ours.row(y) + 3 * x;
        if (lumaOf(q) > 0.06f || lumaOf(q) < 0.01f) continue;
        const int c = m.cfaIdx[(y % m.period) * m.period + x % m.period] & 3;
        for (int k = 0; k < 3; ++k) sum[k][c == 1 || c == 3 ? 1 : 0] += double(p[k]) / std::max(q[k], 1e-4f), cnt[k][c == 1 || c == 3 ? 1 : 0] += 1;
      }
    for (int site = 0; site < 2; ++site)
      std::printf("  shadow ratio ours/libraw at %s sites: R %.3f G %.3f B %.3f\n", site ? "green" : "red/blue", sum[0][site] / std::max(1.0, cnt[0][site]), sum[1][site] / std::max(1.0, cnt[1][site]), sum[2][site] / std::max(1.0, cnt[2][site]));
  }
  if (bil) {
    std::vector<float> b1, b2;
    for (int y = 0; y < H; y += 2)
      for (int x = 0; x < W; x += 2) b1.push_back(float(dE2000(toLab(base.row(y) + 3 * x, g), toLab(ref.row(y) + 3 * x, g))));
    for (std::size_t i = 0; i < std::min(nTop, blocks.size()); ++i)
      for (int y = blocks[i].y; y < blocks[i].y + B; ++y)
        for (int x = blocks[i].x; x < blocks[i].x + B; ++x) b2.push_back(float(dE2000(toLab(base.row(y) + 3 * x, g), toLab(ref.row(y) + 3 * x, g))));
    const Stats a1 = statsOf(b1), a2 = statsOf(b2);
    std::printf("  bilinear vs LibRaw: full mean %.3f p99 %.3f | detail mean %.3f p99 %.3f\n", a1.mean, a1.p99, a2.mean, a2.p99);
  }
  {  // colour accuracy at low frequency: dE2000 of 8 x 8 box averages (separates the colour / white balance from the detail and noise)
    std::vector<float> lo;
    for (int by = 0; by + 8 <= H; by += 8)
      for (int bx = 0; bx + 8 <= W; bx += 8) {
        float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        for (int y = by; y < by + 8; ++y)
          for (int x = bx; x < bx + 8; ++x)
            for (int c = 0; c < 3; ++c) a[c] += ours.row(y)[3 * x + c], b[c] += ref.row(y)[3 * x + c];
        for (int c = 0; c < 3; ++c) a[c] /= 64.f, b[c] /= 64.f;
        lo.push_back(float(dE2000(toLab(a, g), toLab(b, g))));
        if (std::getenv("DEMO_DEBUG") && lo.back() > 12.f && (bx / 8 + by / 8) % 211 == 0)
          std::printf("    block (%d,%d) ours %.4f %.4f %.4f  libraw %.4f %.4f %.4f dE %.1f\n", bx, by, a[0], a[1], a[2], b[0], b[1], b[2], lo.back());
      }
    const Stats sl = statsOf(lo);
    std::printf("  low-frequency (8x8 means) dE2000: mean %.3f p99 %.3f\n", sl.mean, sl.p99);
  }
  const Zip zo = zipper(ours, ref), zr = zipper(ref, ref), zb = bil ? zipper(base, ref) : Zip{};
  std::printf("  dE2000 vs LibRaw: full mean %.3f p99 %.3f | border(24px) mean %.3f p99 %.3f | detail crops (%zu x 64^2) mean %.3f p99 %.3f | clipped-highlight px (%zu) mean %.3f p99 %.3f\n", sAll.mean, sAll.p99, sBorder.mean, sBorder.p99, std::min(nTop, blocks.size()), sDet.mean, sDet.p99, hl.size(), sHl.mean, sHl.p99);
  std::printf("  zipper (Nyquist chroma energy, lower is cleaner): flat ours %.4f libraw %.4f%s | edge ours %.4f libraw %.4f%s | luma ratio ours/libraw %.4f\n", zo.flat, zr.flat, bil ? (" bilinear " + std::to_string(zb.flat)).c_str() : "", zo.edge, zr.edge, bil ? (" bilinear " + std::to_string(zb.edge)).c_str() : "", meanLumaRatio / std::max(1.0, nl));
  std::printf("RESULT %s ok cfa=%s mp=%.1f demosaic_ms=%.0f libraw_ms=%.0f mosaic_ms=%.0f de_mean=%.3f de_p99=%.3f border_mean=%.3f detail_mean=%.3f detail_p99=%.3f hl_mean=%.3f zip_flat=%.4f/%.4f zip_edge=%.4f/%.4f bil_ms=%.0f\n", tag.c_str(),
              m.cfa == CfaKind::XTrans ? "xtrans" : "bayer", double(m.width) * m.height / 1e6, tDemo, tRef, tMosaic, sAll.mean, sAll.p99, sBorder.mean, sDet.mean, sDet.p99, sHl.mean, zo.flat, zr.flat, zo.edge, zr.edge, tBil);
  if (cache) {
    demosaic::MosaicCache mc(std::size_t(2) << 30);
    bool h1 = false, h2 = false, h3 = false;
    t = Clock::now();
    auto a = mc.mosaic(path, {}, &h1);
    const double c1 = msSince(t);
    t = Clock::now();
    auto f1 = mc.full(*a, threads, &h2);
    const double c2 = msSince(t);
    t = Clock::now();
    auto a2 = mc.mosaic(path, {}, &h3);
    const double c3 = msSince(t);
    bool h4 = false;
    t = Clock::now();
    auto f2 = mc.full(*a2, threads, &h4);
    const double c4 = msSince(t);
    std::printf("CACHE %s first: mosaic %.0f ms (hit %d) full %.0f ms (hit %d) | reopen: mosaic %.2f ms (hit %d) full %.2f ms (hit %d), same pointer %d\n", tag.c_str(), c1, h1, c2, h2, c3, h3, c4, h4, int(f1.get() == f2.get()));
  }
  if (!outDir.empty() && atX >= 0) {  // one crop at a given place: LibRaw | node | difference x4, scaled up by atS
    std::filesystem::create_directories(outDir);
    const int cw = 64, chh = 64;
    const int cx = std::clamp(atX, 0, W - cw), cy = std::clamp(atY, 0, H - chh);
    std::vector<std::uint8_t> img(std::size_t(cw * atS * 3) * (chh * atS) * 3, 0);
    blit2x(img, cw * atS * 3, 0, 0, ref, cx, cy, cw, chh, g, atS);
    blit2x(img, cw * atS * 3, cw * atS, 0, ours, cx, cy, cw, chh, g, atS);
    blitDiff(img, cw * atS * 3, cw * atS * 2, 0, ours, ref, cx, cy, cw, chh, g, atS);
    writeJpeg(outDir + "/" + tag + "_at.jpg", img, cw * atS * 3, chh * atS);
  }
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    // contact sheet: the whole picture reduced to 1200 px wide, LibRaw | node
    const int sw = 600, sh = std::max(1, int(double(H) * sw / W));
    std::vector<std::uint8_t> sheet(std::size_t(sw * 2) * sh * 3);
    for (int y = 0; y < sh; ++y)
      for (int x = 0; x < sw; ++x) {
        const int x0 = int(std::int64_t(x) * W / sw), x1 = std::max(x0 + 1, int(std::int64_t(x + 1) * W / sw)), y0 = int(std::int64_t(y) * H / sh), y1 = std::max(y0 + 1, int(std::int64_t(y + 1) * H / sh));
        float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        int n = 0;
        for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4))
          for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4)) {
            for (int c = 0; c < 3; ++c) a[c] += ref.row(yy)[3 * xx + c], b[c] += ours.row(yy)[3 * xx + c];
            ++n;
          }
        for (int c = 0; c < 3; ++c) a[c] /= float(n), b[c] /= float(n);
        toSrgb8(a, g, &sheet[(std::size_t(y) * sw * 2 + x) * 3]);
        toSrgb8(b, g, &sheet[(std::size_t(y) * sw * 2 + sw + x) * 3]);
      }
    writeJpeg(outDir + "/" + tag + "_sheet.jpg", sheet, sw * 2, sh);
    // 200 % crops of the most detailed blocks: LibRaw | node | difference x4 (+ bilinear when asked)
    const int cw = 96, chh = 96;
    for (int i = 0; i < crops && std::size_t(i) < blocks.size(); ++i) {
      const int cx = std::clamp(blocks[std::size_t(i) * std::max<std::size_t>(1, blocks.size() / 50 / std::max(1, crops))].x - 16, 0, W - cw), cy = std::clamp(blocks[std::size_t(i) * std::max<std::size_t>(1, blocks.size() / 50 / std::max(1, crops))].y - 16, 0, H - chh);
      const int panels = bil ? 4 : 3;
      std::vector<std::uint8_t> img(std::size_t(cw * 2 * panels) * (chh * 2) * 3, 0);
      blit2x(img, cw * 2 * panels, 0, 0, ref, cx, cy, cw, chh, g, 2);
      blit2x(img, cw * 2 * panels, cw * 2, 0, ours, cx, cy, cw, chh, g, 2);
      blitDiff(img, cw * 2 * panels, cw * 4, 0, ours, ref, cx, cy, cw, chh, g, 2);
      if (bil) blit2x(img, cw * 2 * panels, cw * 6, 0, base, cx, cy, cw, chh, g, 2);
      writeJpeg(outDir + "/" + tag + "_crop" + std::to_string(i) + ".jpg", img, cw * 2 * panels, chh * 2);
    }
  }
  return 0;
}
