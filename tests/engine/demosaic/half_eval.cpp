// Half-resolution X-Trans path against LibRaw's half-size picture (docs/engine/pipeline-architecture.md 32.12). Not a test (needs the sample files):
//   dapple_half_eval file.RAF [crops_dir]
// Prints size, timing, gradient-energy ratio, dE2000 (mean / p99, highlight ring) and writes side-by-side crops (LibRaw half | ours | difference x 4).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "de2000.h"
#include "engine/nodes/demosaic/demosaic_backend.h"
#include "engine/nodes/demosaic/demosaic_session.h"

using namespace dapple;

namespace {
#if defined(_WIN32)
void setEnvVar(const char* k, const char* v) { _putenv_s(k, v); }
#else
void setEnvVar(const char* k, const char* v) { setenv(k, v, 1); }
#endif
double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double gradEnergy(const ImageF& im) {
  const int cx = im.width / 2, cy = im.height / 2, h = std::min(512, std::min(cx, cy) - 2);
  double sum = 0;
  long cnt = 0;
  auto Y = [&](int x, int y) { return 0.2880 * im.row(y)[3 * x] + 0.7119 * im.row(y)[3 * x + 1]; };
  for (int y = cy - h; y < cy + h; ++y)
    for (int x = cx - h; x < cx + h; ++x) sum += std::hypot(Y(x + 1, y) - Y(x, y), Y(x, y + 1) - Y(x, y)), ++cnt;
  return sum / double(std::max(1L, cnt));
}

// The same proxy on the luminance after a 3 x 3 box blur: noise and 1 px aliasing speckle are suppressed, edges and texture structure remain
double gradEnergyBlurred(const ImageF& im) {
  const int cx = im.width / 2, cy = im.height / 2, h = std::min(512, std::min(cx, cy) - 4);
  auto Y = [&](int x, int y) { return 0.2880 * im.row(y)[3 * x] + 0.7119 * im.row(y)[3 * x + 1]; };
  auto B = [&](int x, int y) { double s = 0; for (int v = -1; v <= 1; ++v) for (int u = -1; u <= 1; ++u) s += Y(x + u, y + v); return s / 9.0; };
  double sum = 0;
  long cnt = 0;
  for (int y = cy - h; y < cy + h; ++y)
    for (int x = cx - h; x < cx + h; ++x) sum += std::hypot(B(x + 1, y) - B(x, y), B(x, y + 1) - B(x, y)), ++cnt;
  return sum / double(std::max(1L, cnt));
}

void toSrgb8(const float* p, float g, std::uint8_t* o) {
  const float r = std::clamp(p[0] * g, 0.f, 1.f), gg = std::clamp(p[1] * g, 0.f, 1.f), b = std::clamp(p[2] * g, 0.f, 1.f);
  const float X = 0.7977f * r + 0.1352f * gg + 0.0313f * b, Y = 0.2880f * r + 0.7119f * gg + 0.0001f * b, Z = 0.8249f * b;
  const float lin[3] = {3.1339f * X - 1.6169f * Y - 0.4906f * Z, -0.9785f * X + 1.9160f * Y + 0.0333f * Z, 0.0720f * X - 0.2290f * Y + 1.4057f * Z};
  for (int k = 0; k < 3; ++k) {
    const float c = std::clamp(lin[k], 0.f, 1.f);
    o[k] = std::uint8_t(std::lround(255.f * (c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f)));
  }
}

// LibRaw | ours | |difference| x 4, the crop (x0, y0, w, h) shown at 2x (pixel replication)
void writeCrop(const std::string& path, const ImageF& a, const ImageF& b, int x0, int y0, int w, int h, float g) {
  const int Z = 2, W = w * Z * 3 + 8, H = h * Z;
  std::vector<std::uint8_t> img(std::size_t(W) * H * 3, 255);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float* pa = a.row(y0 + y) + 3 * (x0 + x);
      const float* pb = b.row(y0 + y) + 3 * (x0 + x);
      float d[3];
      for (int k = 0; k < 3; ++k) d[k] = 0.5f + 4.f * (pb[k] - pa[k]) * g;
      std::uint8_t pix[3][3];
      toSrgb8(pa, g, pix[0]);
      toSrgb8(pb, g, pix[1]);
      for (int k = 0; k < 3; ++k) pix[2][k] = std::uint8_t(std::lround(255.f * std::clamp(d[k], 0.f, 1.f)));
      for (int v = 0; v < Z; ++v)
        for (int u = 0; u < Z; ++u)
          for (int s = 0; s < 3; ++s) std::memcpy(&img[(std::size_t(y * Z + v) * W + (s * (w * Z + 4)) + x * Z + u) * 3], pix[s], 3);
    }
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  stbi_write_jpg(path.c_str(), W, H, 3, img.data(), 93);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: dapple_half_eval file [crops_dir]\n");
    return 2;
  }
  const std::filesystem::path f = argv[1];
  const std::string cropDir = argc > 2 ? argv[2] : "";
  setEnvVar("DAPPLE_ANCHOR", "off");
#ifdef DAPPLE_HAVE_GPU
  if (const char* e = std::getenv("DAPPLE_HALF_GPU"); e && e[0] == '1') demosaic::installGpuBackend();  // the verdict then picks GPU or CPU
#endif
  DecodeOptions o;
  o.quality = DecodeQuality::Preview;
  setEnvVar("DAPPLE_PREVIEW_MOSAIC", "off");
  double t0 = nowMs();
  auto a = makeLibRawDecoder()->decode(f, o);
  const double tLib = nowMs() - t0;
  setEnvVar("DAPPLE_PREVIEW_MOSAIC", "on");
  o.wantLongEdge = 4096;
  clearMosaicCache();
  std::string why;
  t0 = nowMs();
  auto warm = decodePreviewFromMosaic(f, o, &why);  // unpack + half path
  const double tCold = nowMs() - t0;
  t0 = nowMs();
  auto b = decodePreviewFromMosaic(f, o, &why);  // mosaic cached
  const double tHot = nowMs() - t0;
  if (!a || !b) {
    std::printf("%s: %s\n", f.filename().string().c_str(), !a ? "LibRaw failed" : why.c_str());
    return 1;
  }
  const ImageF& A = a->linear;
  const ImageF& B = b->linear;
  std::printf("%s: LibRaw half %dx%d (%.0f ms) | ours %dx%d (cold %.0f ms, mosaic cached %.0f ms)\n", f.filename().string().c_str(), A.width, A.height, tLib, B.width, B.height, tCold, tHot);
  if (A.width != B.width || A.height != B.height) {
    std::printf("  size differs\n");
    return 1;
  }
  const double ga = gradEnergy(A), gb = gradEnergy(B);
  std::printf("  sharpness (gradient energy, centre 1024^2): ours / LibRaw %.3f\n", gb / ga);
  std::printf("  structure sharpness (gradient energy of the 3x3-blurred luminance): ours / LibRaw %.3f\n", gradEnergyBlurred(B) / gradEnergyBlurred(A));
  double sumY = 0;
  for (int y = 0; y < A.height; y += 3)
    for (int x = 0; x < A.width; x += 3) sumY += 0.2880 * A.row(y)[3 * x] + 0.7119 * A.row(y)[3 * x + 1];
  const float g = float(0.18 / std::max(1e-4, sumY / (double((A.width + 2) / 3) * ((A.height + 2) / 3))));
  std::vector<float> de;
  de.reserve(std::size_t(A.width) * A.height / 4);
  double sum = 0;
  long hlN = 0, hlBad = 0;
  double hlSum = 0;
  for (int y = 4; y < A.height - 4; y += 2)
    for (int x = 4; x < A.width - 4; x += 2) {
      const float v = float(demo::dE2000(demo::toLab(A.row(y) + 3 * x, g), demo::toLab(B.row(y) + 3 * x, g)));
      de.push_back(v);
      sum += v;
      const float* q = A.row(y) + 3 * x;
      if (std::max({q[0], q[1], q[2]}) >= 0.98f) {  // highlight ring: pixels within 3 px of a (near) clipped LibRaw pixel but not clipped themselves
        continue;
      }
      bool nearClip = false;
      for (int dy = -3; dy <= 3 && !nearClip; dy += 3)
        for (int dx = -3; dx <= 3; dx += 3) {
          const float* r = A.row(y + dy) + 3 * (x + dx);
          if (std::max({r[0], r[1], r[2]}) >= 0.98f) { nearClip = true; break; }
        }
      if (nearClip) ++hlN, hlSum += v, hlBad += v > 10.f;
    }
  std::sort(de.begin(), de.end());
  std::printf("  dE2000 vs LibRaw half (exposure-normalised): mean %.2f p50 %.2f p99 %.2f; highlight ring (%ld px): mean %.2f, >10: %ld\n", sum / double(de.size()), de[de.size() / 2],
              de[std::size_t(0.99 * double(de.size() - 1))], hlN, hlN ? hlSum / double(hlN) : 0.0, hlBad);
  {  // low-frequency colour: both pictures reduced by 4 x 4 block means (noise and speckle average out), dE2000 per block
    std::vector<float> lf;
    for (int y = 0; y + 4 <= A.height; y += 4)
      for (int x = 0; x + 4 <= A.width; x += 4) {
        float pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
        for (int v = 0; v < 4; ++v)
          for (int u = 0; u < 4; ++u)
            for (int k = 0; k < 3; ++k) pa[k] += A.row(y + v)[3 * (x + u) + k] / 16.f, pb[k] += B.row(y + v)[3 * (x + u) + k] / 16.f;
        lf.push_back(float(demo::dE2000(demo::toLab(pa, g), demo::toLab(pb, g))));
      }
    std::sort(lf.begin(), lf.end());
    double m = 0;
    for (float v : lf) m += v;
    std::printf("  low-frequency (4x4 block means) dE2000: mean %.2f p99 %.2f\n", m /double(lf.size()), lf[std::size_t(0.99 * double(lf.size() - 1))]);
  }
  if (!cropDir.empty()) {
    const int C = 160, step = 160;
    struct Blk { double score; int x, y; };
    std::vector<Blk> sharp, clip;
    for (int y = 8; y + C < A.height - 8; y += step)
      for (int x = 8; x + C < A.width - 8; x += step) {
        double e = 0, c = 0;
        for (int yy = y; yy < y + C; yy += 2)
          for (int xx = x; xx < x + C; xx += 2) {
            const float* p = B.row(yy) + 3 * xx;
            e += std::fabs(p[1] - B.row(yy)[3 * (xx + 1) + 1]) + std::fabs(p[1] - B.row(yy + 1)[3 * xx + 1]);
            c += std::max({p[0], p[1], p[2]}) >= 0.9f;
          }
        sharp.push_back({e, x, y});
        clip.push_back({c > 2 && c < 0.5 * (C / 2) * (C / 2) ? c : 0, x, y});
      }
    auto top = [](std::vector<Blk>& v, int n) { std::sort(v.begin(), v.end(), [](const Blk& p, const Blk& q) { return p.score > q.score; }); v.resize(std::min<std::size_t>(v.size(), n)); };
    top(sharp, 3), top(clip, 2);
    int n = 0;
    const std::string stem = f.stem().string();
    for (auto* v : {&sharp, &clip})
      for (const Blk& k : *v) {
        if (k.score <= 0) continue;
        char nm[64];
        std::snprintf(nm, sizeof nm, "%s_%s%d.jpg", stem.c_str(), v == &sharp ? "detail" : "highlight", n++);
        writeCrop((std::filesystem::path(cropDir) / nm).string(), A, B, k.x, k.y, C, C, g);
      }
  }
  (void)warm;
  return 0;
}
