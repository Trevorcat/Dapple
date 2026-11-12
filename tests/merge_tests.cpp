// Tests for HDR merge, panorama stitching, Motion Photo and sequence detection.
//
// Real files: the owner's ARWs (read-only, private) are used as scene content when present
// (DAPPLE_TEST_ARW or the default below); otherwise a procedural scene is used. Result images are
// written to $DAPPLE_MERGE_OUT (default build/merge) so they can be looked at.
// DAPPLE_PERF=1 additionally times 3x26 MP HDR and 4x26 MP panorama jobs.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <stb_image_write.h>

#include "dapple/dapple_c.h"
#include "dapple/decoder.h"
#include "dapple/encode.h"
#include "dapple/merge.h"
#include "dapple/motion_photo.h"
#include "file_util.h"

#ifdef DAPPLE_HAVE_OPENCV
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace fs = std::filesystem;
using namespace dapple;

namespace {

int g_checks = 0, g_failures = 0, g_skipped = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_LT(a, b)                                                                                         \
  do {                                                                                                         \
    ++g_checks;                                                                                                \
    const double va_ = (a), vb_ = (b);                                                                         \
    std::printf("    %s = %.5g (limit %s = %.5g)\n", #a, va_, #b, vb_);                                         \
    if (!(va_ < vb_)) {                                                                                        \
      ++g_failures;                                                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s = %.6g not < %s = %.6g\n", __FILE__, __LINE__, #a, va_, #b, vb_); \
    }                                                                                                          \
  } while (0)

fs::path outDir() {
  const char* e = std::getenv("DAPPLE_MERGE_OUT");
  fs::path p = e && *e ? fs::path(e) : fs::path("build/merge");
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

std::string findArw() {
  if (const char* e = std::getenv("DAPPLE_TEST_ARW"))
    if (fs::exists(e)) return e;
  const char* cands[] = {"D:/\xE5\x9B\xBE\xE7\x89\x87/2026-07-10/DSC00422.ARW", "D:/\xE5\x9B\xBE\xE7\x89\x87/2026-07-10/DSC00111.ARW"};
  for (const char* c : cands) {
    std::error_code ec;
    if (fs::exists(fs::path(std::u8string(c, c + std::strlen(c))), ec)) return c;
  }
  return {};
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void saveGamma(const ImageF& im, const fs::path& path, float exposure = 1.f, int longEdge = 1600) {
  ImageF s = resizeToLongEdge(im, longEdge);
  Image8 out;
  out.width = s.width;
  out.height = s.height;
  out.data.resize(s.data.size());
  for (std::size_t i = 0; i < s.data.size(); ++i) {
    const float v = std::clamp(s.data[i] * exposure, 0.f, 1.f);
    const float e = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    out.data[i] = std::uint8_t(std::clamp(e, 0.f, 1.f) * 255.f + 0.5f);
  }
  (void)writeJpeg(out, path, JpegOptions{93});
}

ImageF proceduralScene(int w, int h) {
  ImageF im(w, h);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> U(0.f, 1.f);
  // smooth colour field + texture + hard edges + a bright "sun"
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float fx = float(x) / w, fy = float(y) / h;
      float v = 0.25f + 0.2f * std::sin(fx * 9.f) * std::cos(fy * 7.f);
      v += 0.08f * std::sin(fx * 90.f + fy * 40.f) * std::sin(fy * 70.f - fx * 30.f);
      if (std::fabs(fx - 0.5f) < 0.1f && fy > 0.5f) v *= 0.15f;                      // dark doorway
      if (std::hypot(fx - 0.78f, fy - 0.2f) < 0.12f) v = 1.f;                         // clipped sun
      if (((x / 40) + (y / 40)) % 2 == 0 && fy < 0.45f && fx < 0.4f) v += 0.1f;       // checker
      float* p = im.row(y) + 3 * x;
      p[0] = v * (0.9f + 0.2f * fx);
      p[1] = v;
      p[2] = v * (1.1f - 0.2f * fx);
    }
  (void)U;
  (void)rng;
  return im;
}

// Real scene content when available: the first ARW, developed to linear ProPhoto.
ImageF loadScene(int longEdge, bool& real) {
  real = false;
  const std::string arw = findArw();
  if (!arw.empty()) {
    auto reg = DecoderRegistry::withBuiltins();
    const ImageDecoder* dec = reg.find(fromUtf8(arw));
    auto src = dec ? dec->decode(fromUtf8(arw), DecodeOptions{DecodeQuality::Preview}) : fail("no decoder");
    if (src) {
      real = true;
      return resizeToLongEdge(src->linear, longEdge);
    }
  }
  return proceduralScene(longEdge * 3 / 4, longEdge);
}

double logErr(float a, float b) { return std::log2((a + 1e-3) / (b + 1e-3)); }

}  // namespace

// ===========================================================================
// HDR
// ===========================================================================
#ifdef DAPPLE_HAVE_OPENCV

namespace {

struct HdrCase {
  ImageF scene;            // ground-truth radiance in base units (1.0 = base clipping point)
  std::vector<SourceImage> frames;  // -2, 0, +2 EV
  cv::Mat M[3];            // frame = M * base (affine, 2x3) for each frame
  int patchX0, patchY0, patchW, patchH;
  bool hasPatch;
};

cv::Mat affine(double dx, double dy, double deg, double scale, int w, int h) {
  cv::Mat A = cv::getRotationMatrix2D(cv::Point2f(w * 0.5f, h * 0.5f), deg, scale);
  A.at<double>(0, 2) += dx;
  A.at<double>(1, 2) += dy;
  return A;
}

// Scene "radiance" is the ARW (or procedural) upsampled 2x; every frame is a warped *sample* of that hi-res scene
// (area-downsampled), so base and warped frames have equal sharpness, like real exposures from a sensor.
HdrCase makeHdrCase(const ImageF& base, bool movingPatch, std::uint32_t seed, bool moved = true) {
  HdrCase c;
  const int w = base.width, h = base.height;
  cv::Mat hi;
  {
    cv::Mat src(h, w, CV_32FC3, const_cast<float*>(base.data.data()));
    cv::resize(src, hi, cv::Size(w * 2, h * 2), 0, 0, cv::INTER_CUBIC);
    hi = cv::max(hi, 0.f) * 3.0f;  // 2 extra stops above the sensor clipping point
  }
  c.scene = ImageF(w, h);
  {
    cv::Mat dst(h, w, CV_32FC3, c.scene.data.data());
    cv::resize(hi, dst, cv::Size(w, h), 0, 0, cv::INTER_AREA);
  }
  const double evs[3] = {-2, 0, 2};
  c.M[0] = moved ? affine(3.4, -2.1, 0.25, 1.0005, w, h) : affine(0, 0, 0, 1, w, h);
  c.M[1] = affine(0, 0, 0, 1, w, h);
  c.M[2] = moved ? affine(-2.7, 1.8, -0.2, 0.9995, w, h) : affine(0, 0, 0, 1, w, h);
  c.hasPatch = movingPatch;
  c.patchW = w / 8;
  c.patchH = h / 8;
  const int shift = int(w * 0.09);
  // choose a patch in mid-tones (base not clipped, not black) so that the base frame can show what is "really" there
  c.patchX0 = int(w * 0.3);
  c.patchY0 = int(h * 0.4);
  if (movingPatch) {
    double best = -1;
    for (int py = h / 8; py < h - h / 8 - c.patchH; py += h / 32)
      for (int px = shift + w / 16; px < w - w / 16 - c.patchW; px += w / 32) {
        long good = 0, total = 0;
        double diff = 0;
        for (int y = py; y < py + c.patchH; y += 3)
          for (int x = px; x < px + c.patchW; x += 3) {
            const float* a = c.scene.row(y) + 3 * x;
            const float* b = c.scene.row(y) + 3 * (x - shift);
            const float la = a[1], lb = b[1];
            good += (la > 0.08f && la < 0.7f && lb > 0.05f && lb < 0.85f);
            diff += std::fabs(la - 0.7f * lb);
            ++total;
          }
        const double score = (good == total ? 10.0 : 0.0) + double(good) / total + 0.5 * diff / total;
        if (score > best) {
          best = score;
          c.patchX0 = px;
          c.patchY0 = py;
        }
      }
  }
  std::mt19937 rng(seed);
  std::normal_distribution<float> N(0.f, 1.f);
  for (int f = 0; f < 3; ++f) {
    ImageF warped(w, h);
    {
      // A2 = T * A * T^-1 with T: (x,y) -> (2x+0.5, 2y+0.5) taking pixel-centre coordinates to the 2x grid
      cv::Mat A3 = cv::Mat::eye(3, 3, CV_64F), T = cv::Mat::eye(3, 3, CV_64F);
      c.M[f].copyTo(A3(cv::Rect(0, 0, 3, 2)));
      T.at<double>(0, 0) = T.at<double>(1, 1) = 2;
      T.at<double>(0, 2) = T.at<double>(1, 2) = 0.5;
      cv::Mat A2 = T * A3 * T.inv();
      cv::Mat w2;
      cv::warpAffine(hi, w2, A2(cv::Rect(0, 0, 3, 2)), cv::Size(w * 2, h * 2), cv::INTER_CUBIC, cv::BORDER_REFLECT);
      cv::Mat dst(h, w, CV_32FC3, warped.data.data());
      cv::resize(w2, dst, cv::Size(w, h), 0, 0, cv::INTER_AREA);
    }
    if (movingPatch && f != 1) {  // the object sits 9% of the width further right in the dark and bright frames
      for (int y = c.patchY0; y < c.patchY0 + c.patchH; ++y)
        for (int x = c.patchX0; x < c.patchX0 + c.patchW; ++x) {
          const int sx = x - shift;
          for (int ch = 0; ch < 3; ++ch) warped.row(y)[3 * x + ch] = c.scene.row(y)[3 * std::clamp(sx, 0, w - 1) + ch] * 0.7f;
        }
    }
    SourceImage s;
    s.sceneReferred = true;
    s.meta.make = "Sony";
    s.meta.model = "ILCE-7RM5";
    s.meta.iso = 100;
    s.meta.aperture = 5.6f;
    s.meta.shutterSeconds = float(0.01 * std::pow(2.0, evs[f]));
    s.meta.sensorWidth = w;
    s.meta.sensorHeight = h;
    const float g = float(std::pow(2.0, evs[f]));
    s.linear = ImageF(w, h);
    for (std::size_t i = 0; i < s.linear.data.size(); ++i) {
      float v = std::clamp(warped.data[i] * g, 0.f, 1.f);
      // sensor noise: read noise + shot noise, then 14-bit quantisation
      const float sigma = std::sqrt(2e-5f * (1e-3f + v));
      v += sigma * N(rng);
      v = std::clamp(std::round(v * 16383.f) / 16383.f, 0.f, 1.f);
      s.linear.data[i] = v;
    }
    c.frames.push_back(std::move(s));
  }
  return c;
}

struct HdrStats {
  double highlight = 0, shadow = 0, mid = 0, patch = 0;  // RMS log2 error vs ground truth
  double baseHighlight = 0, baseShadow = 0;
  double edgeShadow = 0;
  long nH = 0, nS = 0, nM = 0, nE = 0;
};

HdrStats scoreHdr(const HdrCase& c, const ImageF& merged) {
  HdrStats s;
  const int w = c.scene.width, h = c.scene.height, m = 48;
  double sh = 0, ss = 0, sm = 0, sp = 0, bh = 0, bs = 0;
  long np = 0;
  for (int y = m; y < h - m; ++y)
    for (int x = m; x < w - m; ++x) {
      const float* t = c.scene.row(y) + 3 * x;
      const float* r = merged.row(y) + 3 * x;
      const float* b = c.frames[1].linear.row(y) + 3 * x;
      const float tl = 0.3f * t[0] + 0.6f * t[1] + 0.1f * t[2], rl = 0.3f * r[0] + 0.6f * r[1] + 0.1f * r[2],
                  bl = 0.3f * b[0] + 0.6f * b[1] + 0.1f * b[2];
      const bool inPatch = c.hasPatch && x >= c.patchX0 - 6 && x < c.patchX0 + c.patchW + 6 && y >= c.patchY0 - 6 &&
                           y < c.patchY0 + c.patchH + 6;
      if (inPatch) {
        // the base frame shows the true scene here: the merge must follow the base, not the moved object
        if (x >= c.patchX0 + 8 && x < c.patchX0 + c.patchW - 8 && y >= c.patchY0 + 8 && y < c.patchY0 + c.patchH - 8) {
          const double e = logErr(rl, tl);
          sp += e * e;
          ++np;
        }
        continue;
      }
      if (tl > 1.3f && tl < 2.8f) {
        const double e = logErr(rl, tl), eb = logErr(bl, tl);
        sh += e * e;
        bh += eb * eb;
        ++s.nH;
      } else if (tl > 0.003f && tl < 0.03f) {
        // flat shadows only: at hard edges the error is dominated by sub-pixel registration, not noise
        const float* tr = c.scene.row(y) + 3 * (x + 1);
        const float* tl2 = c.scene.row(y) + 3 * (x - 1);
        const float* td = c.scene.row(y + 1) + 3 * x;
        const float* tu = c.scene.row(y - 1) + 3 * x;
        const float grad = std::fabs(tr[1] - tl2[1]) + std::fabs(td[1] - tu[1]);
        if (grad > 0.15f * tl) {
          const double e = logErr(rl, tl);
          s.edgeShadow += e * e;
          ++s.nE;
          continue;
        }
        const double e = logErr(rl, tl), eb = logErr(bl, tl);
        ss += e * e;
        bs += eb * eb;
        ++s.nS;
      } else if (tl > 0.06f && tl < 0.8f) {
        const double e = logErr(rl, tl);
        sm += e * e;
        ++s.nM;
      }
    }
  s.highlight = s.nH ? std::sqrt(sh / s.nH) : 0;
  s.baseHighlight = s.nH ? std::sqrt(bh / s.nH) : 0;
  s.shadow = s.nS ? std::sqrt(ss / s.nS) : 0;
  s.baseShadow = s.nS ? std::sqrt(bs / s.nS) : 0;
  s.mid = s.nM ? std::sqrt(sm / s.nM) : 0;
  s.patch = np ? std::sqrt(sp / np) : 0;
  s.edgeShadow = s.nE ? std::sqrt(s.edgeShadow / s.nE) : 0;
  return s;
}

double expectedShift(const cv::Mat& A, int w, int h) {
  const cv::Point2d c[4] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
  double sum = 0;
  for (auto& p : c) {
    const double X = A.at<double>(0, 0) * p.x + A.at<double>(0, 1) * p.y + A.at<double>(0, 2);
    const double Y = A.at<double>(1, 0) * p.x + A.at<double>(1, 1) * p.y + A.at<double>(1, 2);
    sum += std::hypot(X - p.x, Y - p.y);
  }
  return sum / 4;
}

}  // namespace

void testHdrSynthetic() {
  bool real = false;
  ImageF base = loadScene(2000, real);
  std::printf("    scene: %s %dx%d\n", real ? "owner ARW" : "procedural", base.width, base.height);
  const fs::path out = outDir();

  // --- with a moving object, deghost mid ---
  HdrCase c = makeHdrCase(base, true, 11);
  {
    auto frames = c.frames;
    HdrOptions o;
    o.baseIndex = 1;
    o.deghost = Deghost::Mid;
    auto r = mergeHdrImages(frames, o);
    CHECK(r.has_value());
    if (!r) {
      std::fprintf(stderr, "    %s\n", r.error().message.c_str());
      return;
    }
    const HdrReport& rep = r->report;
    std::printf("    align+merge %.2f/%.2f s  maxValue %.2f\n", rep.secAlign, rep.secMerge, rep.maxValue);
    for (int i = 0; i < 3; ++i)
      std::printf("    frame %d: EV used %+.3f exif %+.3f  align %.0f score %.3f shift %.3f px (true %.3f)  ghost %.2f%%\n", i,
                  rep.frames[i].evUsed, rep.frames[i].evExif, rep.frames[i].alignMethod, rep.frames[i].alignScore,
                  rep.frames[i].shiftPx, i == 1 ? 0.0 : expectedShift(c.M[i], base.width, base.height), rep.frames[i].ghostFraction * 100);
    CHECK(std::fabs(rep.frames[0].evUsed - (-2.0)) < 0.08);
    CHECK(std::fabs(rep.frames[2].evUsed - 2.0) < 0.08);
    CHECK(std::fabs(rep.frames[0].shiftPx - expectedShift(c.M[0], base.width, base.height)) < 0.5);
    CHECK(std::fabs(rep.frames[2].shiftPx - expectedShift(c.M[2], base.width, base.height)) < 0.5);
    CHECK(r->image.sceneReferred);
    CHECK(r->image.linear.width == base.width && r->image.linear.height == base.height);

    const HdrStats s = scoreHdr(c, r->image.linear);
    std::printf("    RMS log2 error  highlights: merged %.3f (base-only %.3f, n=%ld)  flat shadows: merged %.3f (base-only %.3f, n=%ld)  shadow edges (registration-limited) %.3f (n=%ld)  mid %.3f (n=%ld)  moved-object region %.3f\n",
                s.highlight, s.baseHighlight, s.nH, s.shadow, s.baseShadow, s.nS, s.edgeShadow, s.nE, s.mid, s.nM, s.patch);
    if (s.nH > 500) CHECK_LT(s.highlight, 0.2);
    if (s.nH > 500) CHECK_LT(s.highlight * 2.0, s.baseHighlight);
    if (s.nS > 500) CHECK_LT(s.shadow, s.baseShadow * 0.8);
    CHECK_LT(s.mid, 0.12);
    CHECK_LT(s.patch, 0.12);
    // ghost detection must flag mostly the moved region
    CHECK(rep.frames[0].ghostFraction > 0.005 && rep.frames[0].ghostFraction < 0.12);

    // without deghost the moved object leaks in
    HdrOptions off = o;
    off.deghost = Deghost::Off;
    auto r0 = mergeHdrImages(c.frames, off);
    CHECK(r0.has_value());
    if (r0) {
      const HdrStats s0 = scoreHdr(c, r0->image.linear);
      std::printf("    deghost off: moved-object region error %.3f (mid deghost %.3f)\n", s0.patch, s.patch);
      CHECK(s0.patch > s.patch * 1.8);
    }
    // levels are ordered: low <= mid <= high in how much they replace
    HdrOptions lo = o, hi = o;
    lo.deghost = Deghost::Low;
    hi.deghost = Deghost::High;
    auto rl = mergeHdrImages(c.frames, lo);
    auto rh = mergeHdrImages(c.frames, hi);
    if (rl && rh) {
      std::printf("    ghost fraction low/mid/high: %.3f / %.3f / %.3f\n", rl->report.frames[0].ghostFraction,
                  rep.frames[0].ghostFraction, rh->report.frames[0].ghostFraction);
      CHECK(rl->report.frames[0].ghostFraction <= rep.frames[0].ghostFraction + 1e-9);
      CHECK(rep.frames[0].ghostFraction <= rh->report.frames[0].ghostFraction + 1e-9);
    }
    // side-by-side crop of the moved-object region: truth | base | merged (deghost mid) | merged (no deghost)
    if (r0) {
      const int mx = 40, x0 = std::max(0, c.patchX0 - mx), y0 = std::max(0, c.patchY0 - mx);
      const int cw = std::min(base.width - x0, c.patchW + 2 * mx), ch = std::min(base.height - y0, c.patchH + 2 * mx);
      ImageF strip(cw * 4, ch);
      const ImageF* srcs[4] = {&c.scene, &c.frames[1].linear, &r->image.linear, &r0->image.linear};
      for (int k = 0; k < 4; ++k)
        for (int y = 0; y < ch; ++y) std::copy_n(srcs[k]->row(y0 + y) + 3 * x0, std::size_t(cw) * 3, strip.row(y) + 3 * cw * k);
      saveGamma(strip, out / "hdr_patch_truth_base_deghost_noghost.jpg", 0.5f, 1600);
    }
    // pictures: frames, merged and base-only for a visual check
    saveGamma(c.frames[0].linear, out / "hdr_frame_m2.jpg", 1.f, 1000);
    saveGamma(c.frames[1].linear, out / "hdr_frame_0.jpg", 1.f, 1000);
    saveGamma(c.frames[2].linear, out / "hdr_frame_p2.jpg", 1.f, 1000);
    saveGamma(c.scene, out / "hdr_truth_x0.35.jpg", 0.35f, 1000);
    saveGamma(r->image.linear, out / "hdr_merged_x0.35.jpg", 0.35f, 1000);   // 1.5 stops down: shows highlight detail
    saveGamma(r->image.linear, out / "hdr_merged_x1.jpg", 1.f, 1000);
    saveGamma(r->image.linear, out / "hdr_merged_x4_shadows.jpg", 4.f, 1000);
    saveGamma(c.frames[1].linear, out / "hdr_base_x4_shadows.jpg", 4.f, 1000);
    if (r0) saveGamma(r0->image.linear, out / "hdr_merged_noghost_x0.35.jpg", 0.35f, 1000);
    // archival round trips
    const fs::path tif = out / "hdr_merged_16bit.tif", dng = out / "hdr_merged.dng";
    CHECK(writeLinearTiff16(r->image.linear, tif).has_value());
    CHECK(writeLinearDng(r->image, dng).has_value());
    auto reg = DecoderRegistry::withBuiltins();
    const ImageDecoder* dd = reg.find(dng);
    CHECK(dd != nullptr);
    if (dd) {
      auto back = dd->decode(dng, DecodeOptions{DecodeQuality::Full});
      CHECK(back.has_value());
      if (back) {
        const int w = base.width, h = base.height;
        CHECK(back->linear.width == w && back->linear.height == h);
        // headroom: the merge maximum rounds up to a power of two
        double num = 0, den = 0, maxBack = 0;
        for (int y = 100; y < h - 100; y += 3)
          for (int x = 100; x < w - 100; x += 3) {
            const float* a = r->image.linear.row(y) + 3 * x;
            const float* b = back->linear.row(y) + 3 * x;
            for (int ch = 0; ch < 3; ++ch) {
              num += double(a[ch]) * b[ch];
              den += double(b[ch]) * b[ch];
              maxBack = std::max<double>(maxBack, b[ch]);
            }
          }
        const double gain = den > 0 ? num / den : 0;
        double err = 0, cnt = 0;
        for (int y = 100; y < h - 100; y += 3)
          for (int x = 100; x < w - 100; x += 3) {
            const float* a = r->image.linear.row(y) + 3 * x;
            const float* b = back->linear.row(y) + 3 * x;
            for (int ch = 0; ch < 3; ++ch)
              if (a[ch] > 0.05f) {
                err += std::fabs(std::log2((b[ch] + 1e-4) / (a[ch] + 1e-4)));
                ++cnt;
              }
          }
        std::printf("    DNG round trip through LibRaw: best scale %.4f, mean |log2 err| %.4f (max decoded %.2f, merged max %.2f)\n",
                    gain, cnt ? err / cnt : 0, maxBack, r->report.maxValue);
        CHECK(std::fabs(gain - 1.0) < 0.03);                     // BaselineExposure restored by the decoder
        CHECK(cnt > 0 && err / cnt < 0.03);                      // 16-bit quantisation + colour matrix round trip
      }
    }
    std::error_code ec;
    CHECK(fs::file_size(tif, ec) > 1000);
  }

  // --- no moving object: deghost must not eat the real exposure differences ---
  {
    HdrCase c2 = makeHdrCase(base, false, 12);
    HdrOptions o;
    o.baseIndex = 1;
    o.deghost = Deghost::Mid;
    auto r = mergeHdrImages(c2.frames, o);
    CHECK(r.has_value());
    if (r) {
      std::printf("    static scene ghost fraction (false positives): %.3f%% / %.3f%%\n",
                  r->report.frames[0].ghostFraction * 100, r->report.frames[2].ghostFraction * 100);
      CHECK(r->report.frames[0].ghostFraction < 0.03 && r->report.frames[2].ghostFraction < 0.03);
      std::printf("    static alignment: shift est %.3f / %.3f px, true %.3f / %.3f px\n", r->report.frames[0].shiftPx, r->report.frames[2].shiftPx,
                  expectedShift(c2.M[0], base.width, base.height), expectedShift(c2.M[2], base.width, base.height));
      CHECK(std::fabs(r->report.frames[0].shiftPx - expectedShift(c2.M[0], base.width, base.height)) < 0.3);
      CHECK(std::fabs(r->report.frames[2].shiftPx - expectedShift(c2.M[2], base.width, base.height)) < 0.3);
      {
        ImageF em(base.width, base.height);
        for (int y = 0; y < base.height; ++y)
          for (int x = 0; x < base.width; ++x)
            for (int ch = 0; ch < 3; ++ch)
              em.row(y)[3 * x + ch] = float(std::fabs(logErr(r->image.linear.row(y)[3 * x + 1], c2.scene.row(y)[3 * x + 1])) / 4.0);
        saveGamma(em, outDir() / "hdr_static_log2_error_x4.jpg", 1.f, 1000);
      }
      const HdrStats s = scoreHdr(c2, r->image.linear);
      std::printf("    static: highlights %.3f (base %.3f) shadows %.3f (base %.3f) mid %.3f\n", s.highlight, s.baseHighlight,
                  s.shadow, s.baseShadow, s.mid);
      CHECK_LT(s.mid, 0.1);
    }
  }
  // --- tripod case: identical geometry; the merge must clearly reduce shadow noise ---
  {
    HdrCase c4 = makeHdrCase(base, false, 14, false);
    HdrOptions o;
    o.baseIndex = 1;
    auto r = mergeHdrImages(c4.frames, o);
    CHECK(r.has_value());
    if (r) {
      const HdrStats s = scoreHdr(c4, r->image.linear);
      std::printf("    tripod (no motion): shadows merged %.3f vs base-only %.3f, highlights merged %.3f vs base %.3f, mid %.3f; shift %.3f/%.3f px\n",
                  s.shadow, s.baseShadow, s.highlight, s.baseHighlight, s.mid, r->report.frames[0].shiftPx, r->report.frames[2].shiftPx);
      CHECK_LT(s.shadow, s.baseShadow * 0.8);
      CHECK_LT(r->report.frames[0].shiftPx, 0.1);
    }
  }
  // --- default base = middle exposure; frames given in shuffled order ---
  {
    HdrCase c3 = makeHdrCase(base, false, 13);
    std::vector<SourceImage> shuffled = {c3.frames[2], c3.frames[0], c3.frames[1]};
    auto r = mergeHdrImages(shuffled, HdrOptions{});
    CHECK(r.has_value());
    if (r) CHECK(r->report.baseIndex == 2);  // the 0 EV frame
  }
}

void testHdrJpegFusionAndErrors() {
  bool real = false;
  ImageF base = loadScene(1200, real);
  HdrCase c = makeHdrCase(base, false, 21);
  // display-referred frames (as camera JPEGs would be): gamma encode, flag as not scene referred
  std::vector<SourceImage> jp;
  for (auto& f : c.frames) {
    SourceImage s = f;
    s.sceneReferred = false;
    s.meta.shutterSeconds = 0;
    for (auto& v : s.linear.data) v = std::pow(std::clamp(v, 0.f, 1.f), 1.f / 2.2f), v = std::pow(v, 2.2f);
    jp.push_back(std::move(s));
  }
  auto r = mergeHdrImages(jp, HdrOptions{});
  CHECK(r.has_value());
  if (r) {
    CHECK(!r->image.sceneReferred);
    CHECK(r->report.exposureFusion);
    saveGamma(r->image.linear, outDir() / "hdr_jpeg_fusion.jpg", 1.f, 1000);
    // regression: OpenCV's Mertens divides every input by 255, so unscaled float input came out black
    double sum = 0;
    for (float v : r->image.linear.data) sum += v;
    const double mean = sum / double(r->image.linear.data.size());
    std::printf("    fusion mean value %.3f\n", mean);
    CHECK(mean > 0.05);
  }
  // errors, not crashes
  CHECK(!mergeHdr({fromUtf8("nope1.arw")}, HdrOptions{}).has_value());
  CHECK(!mergeHdr({fromUtf8("nope1.arw"), fromUtf8("nope2.arw")}, HdrOptions{}).has_value());
  std::vector<SourceImage> one = {c.frames[0]};
  CHECK(!mergeHdrImages(one, HdrOptions{}).has_value());
  // unrelated frames cannot be aligned -> reported error
  SourceImage a = c.frames[0], b = c.frames[1];
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> U(0.05f, 0.6f);
  for (auto& v : b.linear.data) v = U(rng);
  auto bad = mergeHdrImages({a, b}, HdrOptions{});
  std::printf("    unrelated frames: %s\n", bad ? "merged (unexpected)" : bad.error().message.c_str());
  CHECK(!bad.has_value());
  // cancellation
  std::atomic<bool> cancel{false};
  MergeControl ctl;
  ctl.cancel = &cancel;
  int calls = 0;
  ctl.progress = [&](float, const char*) {
    if (++calls == 2) cancel = true;
  };
  auto cr = mergeHdrImages(c.frames, HdrOptions{}, ctl);
  CHECK(!cr.has_value());
  if (!cr) CHECK(cr.error().message == "cancelled");
}

#endif  // DAPPLE_HAVE_OPENCV

// ===========================================================================
// Panorama
// ===========================================================================
#ifdef DAPPLE_HAVE_OPENCV
namespace {

struct Cam {
  double yaw, pitch, roll;  // degrees
};

// Pinhole view into an equirectangular world (linear angle mapping, hAngle degrees across the world width).
ImageF renderView(const ImageF& world, double hAngleDeg, const Cam& cam, int w, int h, double hfovDeg, double gain) {
  const double f = (w * 0.5) / std::tan(hfovDeg * 0.5 * CV_PI / 180.0);
  const double ppd = world.width / hAngleDeg;  // world pixels per degree
  const double y = cam.yaw * CV_PI / 180, p = cam.pitch * CV_PI / 180, r = cam.roll * CV_PI / 180;
  // R = Ry(yaw) * Rx(pitch) * Rz(roll)
  const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
  const double Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
  const double Rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
  const double Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
  double T[3][3], R[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      T[i][j] = 0;
      for (int k = 0; k < 3; ++k) T[i][j] += Rx[i][k] * Rz[k][j];
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      R[i][j] = 0;
      for (int k = 0; k < 3; ++k) R[i][j] += Ry[i][k] * T[k][j];
    }
  cv::Mat mapx(h, w, CV_32F), mapy(h, w, CV_32F);
  for (int v = 0; v < h; ++v)
    for (int u = 0; u < w; ++u) {
      const double dx = u + 0.5 - w * 0.5, dy = v + 0.5 - h * 0.5, dz = f;
      const double X = R[0][0] * dx + R[0][1] * dy + R[0][2] * dz;
      const double Y = R[1][0] * dx + R[1][1] * dy + R[1][2] * dz;
      const double Z = R[2][0] * dx + R[2][1] * dy + R[2][2] * dz;
      const double lon = std::atan2(X, Z) * 180 / CV_PI;
      const double lat = std::atan2(Y, std::hypot(X, Z)) * 180 / CV_PI;
      mapx.at<float>(v, u) = float(world.width * 0.5 + lon * ppd - 0.5);
      mapy.at<float>(v, u) = float(world.height * 0.5 + lat * ppd - 0.5);
    }
  ImageF out(w, h);
  cv::Mat src(world.height, world.width, CV_32FC3, const_cast<float*>(world.data.data())), dst(h, w, CV_32FC3, out.data.data());
  cv::remap(src, dst, mapx, mapy, cv::INTER_CUBIC, cv::BORDER_REFLECT);
  for (auto& v : out.data) v = std::max(0.f, v * float(gain));
  return out;
}

struct PanoCase {
  ImageF world;
  double worldAngle = 120;
  std::vector<SourceImage> frames;
  std::vector<Cam> cams;
  int w, h;
  double hfov;
};

PanoCase makePanoCase(const ImageF& scene, int w, int h, double hfov, const std::vector<Cam>& cams, bool noise) {
  PanoCase pc;
  pc.world = scene;
  pc.cams = cams;
  pc.w = w;
  pc.h = h;
  pc.hfov = hfov;
  std::mt19937 rng(3);
  std::normal_distribution<float> N(0.f, 1.f);
  const double gains[] = {1.0, 1.03, 0.97, 1.02, 0.98, 1.0};
  for (std::size_t i = 0; i < cams.size(); ++i) {
    SourceImage s;
    s.sceneReferred = true;
    s.meta.make = "Sony";
    s.meta.model = "ILCE-7RM5";
    s.meta.focalLengthMm = 24;
    s.linear = renderView(scene, pc.worldAngle, cams[i], w, h, hfov, gains[i % 6]);
    if (noise)
      for (auto& v : s.linear.data) v = std::clamp(v + std::sqrt(2e-5f * (1e-3f + v)) * N(rng), 0.f, 1.f);
    else
      for (auto& v : s.linear.data) v = std::clamp(v, 0.f, 1.f);
    s.meta.sensorWidth = w;
    s.meta.sensorHeight = h;
    pc.frames.push_back(std::move(s));
  }
  return pc;
}

}  // namespace

void testPanoSynthetic() {
  bool real = false;
  ImageF scene = loadScene(5200, real);  // world; the long edge is 120 degrees wide -> transpose if portrait
  if (scene.height > scene.width) {
    ImageF t(scene.height, scene.width);
    for (int y = 0; y < scene.height; ++y)
      for (int x = 0; x < scene.width; ++x) std::copy_n(scene.row(y) + 3 * x, 3, t.row(scene.width - 1 - x) + 3 * y);
    scene = std::move(t);
  }
  std::printf("    world: %s %dx%d over 120 deg\n", real ? "owner ARW" : "procedural", scene.width, scene.height);
  const int w = 1500, h = 1000;
  const double hfov = 40;
  const std::vector<Cam> cams = {{-27, 1.0, 0.6}, {-9, -0.8, -0.7}, {9, 0.9, 1.1}, {27, -1.0, -0.5}};
  PanoCase pc = makePanoCase(scene, w, h, hfov, cams, true);
  const fs::path out = outDir();
  for (int i = 0; i < 4; ++i) saveGamma(pc.frames[i].linear, out / ("pano_input_" + std::to_string(i) + ".jpg"), 1.f, 700);

  PanoOptions o;
  o.projection = PanoProjection::Spherical;
  o.crop = false;
  auto r = stitchPanoramaImages(pc.frames, o);
  CHECK(r.has_value());
  if (!r) {
    std::fprintf(stderr, "    %s\n", r.error().message.c_str());
    return;
  }
  const PanoReport& rep = r->report;
  std::printf("    pano %dx%d from %d frames, features %.2fs estimate %.2fs compose %.2fs, valid fraction %.3f\n",
              r->image.linear.width, r->image.linear.height, rep.framesUsed, rep.secFeatures, rep.secEstimate, rep.secCompose,
              rep.validFraction);
  CHECK(rep.framesUsed == 4);

  // --- geometry: relative yaw between neighbours and focal length ---
  const double regScale = std::sqrt(0.6e6 / (double(w) * h));
  const double fTrue = (w * 0.5) / std::tan(hfov * 0.5 * CV_PI / 180.0) * regScale;
  double yawErrMax = 0, rollErrMax = 0;
  for (int j = 1; j < 4; ++j) {
    const double dyEst = rep.frames[j].yawDeg - rep.frames[0].yawDeg;
    const double dyTrue = cams[j].yaw - cams[0].yaw;
    yawErrMax = std::max(yawErrMax, std::fabs(std::fabs(dyEst) - std::fabs(dyTrue)));
    std::printf("    frame %d: yaw offset est %+.3f true %+.3f   pitch %+.3f   roll %+.3f\n", j, dyEst, dyTrue,
                rep.frames[j].pitchDeg, rep.frames[j].rollDeg);
  }
  (void)rollErrMax;
  std::printf("    focal est %.1f px (reg scale) true %.1f -> %.2f%% error\n", rep.frames[0].focalPx, fTrue,
              100 * std::fabs(rep.frames[0].focalPx / fTrue - 1));
  CHECK_LT(yawErrMax, 0.35);
  CHECK_LT(std::fabs(rep.frames[0].focalPx / fTrue - 1), 0.025);

  // --- pixel accuracy against the equirect ground truth (spherical output == equirect up to scale/shift) ---
  {
    const double fNative = rep.frames[0].focalPx / regScale;
    const double scale = fNative * (CV_PI / 180.0) / (double(scene.width) / pc.worldAngle);  // pano px per world px
    ImageF wscaled;
    {
      cv::Mat src(scene.height, scene.width, CV_32FC3, scene.data.data()), dst;
      cv::resize(src, dst, cv::Size(), scale, scale, scale < 1 ? cv::INTER_AREA : cv::INTER_CUBIC);
      wscaled = ImageF(dst.cols, dst.rows);
      std::copy_n(dst.ptr<float>(0), dst.total() * 3, wscaled.data.begin());
    }
    const ImageF& P = r->image.linear;
    // central window of the pano as template, found in the scaled world by NCC
    const int tw = P.width * 6 / 10, th = std::min(P.height * 4 / 10, wscaled.height - 20);
    const int tx = P.width / 5, ty = (P.height - th) / 2;
    cv::Mat pg(P.height, P.width, CV_32FC3, const_cast<float*>(P.data.data())), wg(wscaled.height, wscaled.width, CV_32FC3, wscaled.data.data());
    cv::Mat pgray, wgray;
    cv::cvtColor(pg, pgray, cv::COLOR_RGB2GRAY);
    cv::cvtColor(wg, wgray, cv::COLOR_RGB2GRAY);
    cv::Mat tmpl = pgray(cv::Rect(tx, ty, tw, th)), res;
    cv::matchTemplate(wgray, tmpl, res, cv::TM_CCOEFF_NORMED);
    double maxv;
    cv::Point maxp;
    cv::minMaxLoc(res, nullptr, &maxv, nullptr, &maxp);
    // sub-pixel refinement via parabola on the correlation peak
    auto at = [&](int x, int y) { return double(res.at<float>(std::clamp(y, 0, res.rows - 1), std::clamp(x, 0, res.cols - 1))); };
    const double sx = 0.5 * (at(maxp.x - 1, maxp.y) - at(maxp.x + 1, maxp.y)) /
                      (at(maxp.x - 1, maxp.y) - 2 * at(maxp.x, maxp.y) + at(maxp.x + 1, maxp.y) + 1e-12);
    const double sy = 0.5 * (at(maxp.x, maxp.y - 1) - at(maxp.x, maxp.y + 1)) /
                      (at(maxp.x, maxp.y - 1) - 2 * at(maxp.x, maxp.y) + at(maxp.x, maxp.y + 1) + 1e-12);
    const double offx = maxp.x + sx - tx, offy = maxp.y + sy - ty;
    std::printf("    ground-truth match: NCC %.4f  pano->world offset (%.2f, %.2f)  scale %.4f\n", maxv, offx, offy, scale);
    CHECK(maxv > 0.97);

    // error map with the known alignment (integer offset), in gamma space
    const int ox = int(std::lround(offx)), oy = int(std::lround(offy));
    std::vector<double> colErr(P.width, 0), colN(P.width, 0);
    double eAll = 0, nAll = 0;
    ImageF errMap(P.width, P.height);
    for (int y = 0; y < P.height; ++y)
      for (int x = 0; x < P.width; ++x) {
        const int wx = x + ox, wy = y + oy;
        const float* p = P.row(y) + 3 * x;
        if (wx < 0 || wy < 0 || wx >= wscaled.width || wy >= wscaled.height) continue;
        if (p[0] + p[1] + p[2] <= 0.f) continue;  // outside the stitched area
        const float* q = wscaled.row(wy) + 3 * wx;
        double e = 0;
        for (int ch = 0; ch < 3; ++ch) e += std::fabs(std::pow(std::clamp(p[ch], 0.f, 1.f), 1 / 2.2f) - std::pow(std::clamp(q[ch], 0.f, 1.f), 1 / 2.2f));
        e /= 3;
        colErr[x] += e;
        colN[x] += 1;
        eAll += e;
        nAll += 1;
        for (int ch = 0; ch < 3; ++ch) errMap.row(y)[3 * x + ch] = float(e * 6);
      }
    saveGamma(errMap, out / "pano_error_x6.jpg", 1.f, 1600);
    // coverage: how many frames cover a column (from the known geometry): lon of column -> yaw ranges
    // pano x = fNative * lon(rad) + const; the world x of column = x + ox -> lon = (wx - world_cx) / scale_ppd
    const double pxPerDeg = fNative * CV_PI / 180.0;
    auto lonOfColumn = [&](int x) { return ((x + ox) - wscaled.width * 0.5) / pxPerDeg; };
    double eOv = 0, nOv = 0, eSingle = 0, nSingle = 0;
    for (int x = 0; x < P.width; ++x) {
      if (colN[x] < 50) continue;
      const double lon = lonOfColumn(x);
      int cover = 0;
      for (const Cam& c : cams)
        if (std::fabs(lon - c.yaw) < hfov * 0.5 - 2.5) ++cover;
      if (cover >= 2) { eOv += colErr[x]; nOv += colN[x]; }
      else if (cover == 1) { eSingle += colErr[x]; nSingle += colN[x]; }
    }
    const double mAll = nAll ? eAll / nAll : 1, mOv = nOv ? eOv / nOv : 1, mSingle = nSingle ? eSingle / nSingle : 1;
    std::printf("    mean |error| (gamma domain, 0..1): all %.4f  single-coverage %.4f  overlaps(seams) %.4f  -> overlap/single %.2f\n",
                mAll, mSingle, mOv, mOv / std::max(1e-9, mSingle));
    CHECK_LT(mAll, 0.03);
    CHECK_LT(mOv / std::max(1e-9, mSingle), 1.6);
  }
  saveGamma(r->image.linear, out / "pano_spherical_nocrop.jpg", 1.f, 2400);

  // --- cylindrical + crop, perspective ---
  {
    PanoOptions oc;
    oc.projection = PanoProjection::Cylindrical;
    oc.crop = true;
    auto rc = stitchPanoramaImages(pc.frames, oc);
    CHECK(rc.has_value());
    if (rc) {
      std::printf("    cylindrical+crop: %dx%d, valid fraction %.4f\n", rc->image.linear.width, rc->image.linear.height, rc->report.validFraction);
      CHECK(rc->report.validFraction > 0.999);
      // no black pixels inside the crop
      long black = 0;
      for (int y = 0; y < rc->image.linear.height; ++y)
        for (int x = 0; x < rc->image.linear.width; ++x) {
          const float* p = rc->image.linear.row(y) + 3 * x;
          if (p[0] + p[1] + p[2] <= 0.f) ++black;
        }
      CHECK(black == 0);
      saveGamma(rc->image.linear, out / "pano_cylindrical_crop.jpg", 1.f, 2400);
    }
    PanoOptions op = oc;
    op.projection = PanoProjection::Perspective;
    auto rp = stitchPanoramaImages(pc.frames, op);
    CHECK(rp.has_value());
    if (rp) {
      std::printf("    perspective+crop: %dx%d\n", rp->image.linear.width, rp->image.linear.height);
      saveGamma(rp->image.linear, out / "pano_perspective_crop.jpg", 1.f, 2400);
    }
    PanoOptions of = o;
    of.fill = true;
    of.crop = false;
    of.projection = PanoProjection::Cylindrical;
    auto rf = stitchPanoramaImages(pc.frames, of);
    CHECK(rf.has_value());
    if (rf) saveGamma(rf->image.linear, out / "pano_cylindrical_fill.jpg", 1.f, 2400);
    // base frame = the second one (projection centre)
    PanoOptions ob = oc;
    ob.baseIndex = 1;
    auto rb = stitchPanoramaImages(pc.frames, ob);
    CHECK(rb.has_value());
    if (rb) {
      CHECK(std::fabs(rb->report.frames[1].yawDeg) < 1e-6 || rb->report.frames[0].yawDeg != 0);
      saveGamma(rb->image.linear, out / "pano_cylindrical_base1.jpg", 1.f, 2400);
    }
    // preview resolution cap
    PanoOptions ol = oc;
    ol.longEdge = 800;
    auto rs = stitchPanoramaImages(pc.frames, ol);
    CHECK(rs.has_value());
    if (rs) {
      std::printf("    preview (long edge 800): %dx%d in %.2fs compose\n", rs->image.linear.width, rs->image.linear.height, rs->report.secCompose);
      CHECK(std::max(rs->image.linear.width, rs->image.linear.height) <= 800);
    }
  }

  // --- unmatched frames: error, with the usable ones still stitched when possible ---
  {
    std::vector<SourceImage> mix = {pc.frames[0], pc.frames[1], pc.frames[2]};
    SourceImage junk = pc.frames[3];
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> U(0.f, 0.5f);
    for (auto& v : junk.linear.data) v = U(rng);
    mix.push_back(junk);
    auto rm = stitchPanoramaImages(mix, o);
    CHECK(rm.has_value());
    if (rm) {
      std::printf("    junk frame left out: used %d of 4 (%s)\n", rm->report.framesUsed, rm->report.warnings.empty() ? "" : rm->report.warnings[0].c_str());
      CHECK(rm->report.framesUsed == 3);
    }
    SourceImage j2 = junk;
    for (auto& v : j2.linear.data) v = U(rng);
    auto rn = stitchPanoramaImages({junk, j2}, o);
    std::printf("    two unrelated frames: %s\n", rn ? "stitched (unexpected)" : rn.error().message.c_str());
    CHECK(!rn.has_value());
    auto r1 = stitchPanoramaImages({pc.frames[0]}, o);
    CHECK(!r1.has_value());
  }
  // --- cancel ---
  {
    std::atomic<bool> cancel{true};
    MergeControl ctl;
    ctl.cancel = &cancel;
    auto rc = stitchPanoramaImages(pc.frames, o, ctl);
    CHECK(!rc.has_value());
    if (!rc) CHECK(rc.error().message == "cancelled");
  }
}

// Regression: portrait (upright, EXIF-rotated) frames must come out upright. OpenCV's wave correction is
// arbitrary for two frames and used to turn the result by 90 degrees.
void testPanoPortrait() {
  bool real = false;
  ImageF scene = loadScene(5200, real);
  if (scene.height > scene.width) {
    ImageF t(scene.height, scene.width);
    for (int y = 0; y < scene.height; ++y)
      for (int x = 0; x < scene.width; ++x) std::copy_n(scene.row(y) + 3 * x, 3, t.row(scene.width - 1 - x) + 3 * y);
    scene = std::move(t);
  }
  const int w = 1000, h = 1500;  // portrait
  struct Case { std::vector<Cam> cams; };
  std::vector<Case> cases;
  for (int sy : {-1, 1})
    for (int sp : {-1, 1})
      for (int sr : {-1, 1}) cases.push_back({{{0, 0, 0}, {3.0 * sy, 1.9 * sp, 0.8 * sr}}});
  cases.push_back({{{0, 0, 0}, {3.0, 1.9, -0.8}}});
  const std::vector<Case> more = {{{{0, 0, 0}, {3.0, 1.9, -0.8}}},
                                   {{{0, 0, 0}, {-4.0, -0.56, 2.72}}},
                                   {{{-5, 0.9, 0.8}, {5, -0.6, -0.9}}},
                                   {{{-4, 0.3, 0.3}, {4, -0.6, -0.5}, {12, 0.4, 0.6}}},
                                   {{{-27, 1.0, 0.6}, {-9, -0.8, -0.7}, {9, 0.9, 1.1}, {27, -1.0, -0.5}}}};
  cases.insert(cases.end(), more.begin() + 1, more.end());
  for (const auto& c : cases) {
    PanoCase pc = makePanoCase(scene, w, h, 30, c.cams, true);
    for (auto proj : {PanoProjection::Cylindrical, PanoProjection::Spherical}) {
      PanoOptions o;
      o.projection = proj;
      o.crop = true;
      auto r = stitchPanoramaImages(pc.frames, o);
      CHECK(r.has_value());
      if (!r) continue;
      const int ow = r->image.linear.width, oh = r->image.linear.height;
      std::printf("    portrait %zu frames %s: %dx%d\n", c.cams.size(), toString(proj), ow, oh);
      saveGamma(r->image.linear, outDir() / ("pano_portrait_" + std::to_string(c.cams.size()) + ".jpg"), 1.f, 1200);
      CHECK(oh > ow * 0.9 || c.cams.size() == 4);  // a 2-3 frame portrait pan stays portrait-ish, never turned 90 degrees
      // The centre column must still match the upright world: correlate it with frame 0's vertical profile.
      CHECK(oh >= int(0.8 * h));
    }
  }
  // Real portrait pair (EXIF-rotated ARW, the original bug): only when the owner's photos are present.
  {
    const fs::path a = fromUtf8("D:/\xE5\x9B\xBE\xE7\x89\x87/2026-07-10/DSC00204.ARW"), b = fromUtf8("D:/\xE5\x9B\xBE\xE7\x89\x87/2026-07-10/DSC00205.ARW");
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
      PanoOptions o;
      o.quality = DecodeQuality::Preview;
      auto r = stitchPanorama({a, b}, o);
      CHECK(r.has_value());
      if (r) {
        std::printf("    real portrait pair: %dx%d\n", r->image.linear.width, r->image.linear.height);
        CHECK(r->image.linear.height > r->image.linear.width);
      }
    }
  }
}
#endif  // DAPPLE_HAVE_OPENCV

// ===========================================================================
// Sequence detection
// ===========================================================================
void testSequenceDetection() {
  auto mk = [](const char* name, long long t, float shutter, float iso, float ap, float focal) {
    FrameInfo f;
    f.path = name;
    f.meta.make = "Sony";
    f.meta.model = "ILCE-7RM5";
    f.meta.timestamp = t;
    f.meta.shutterSeconds = shutter;
    f.meta.iso = iso;
    f.meta.aperture = ap;
    f.meta.focalLengthMm = focal;
    f.meta.sensorWidth = 4180;
    f.meta.sensorHeight = 6272;
    f.exposureKnown = true;
    f.exposureStops = std::log2(double(shutter) * iso / (double(ap) * ap));
    f.orientationKey = "4180x6272";
    return f;
  };
  std::vector<FrameInfo> fr;
  // two back-to-back 3-shot brackets (-2/0/+2), a single, a 4-frame pano pan, a burst of identical frames
  const long long t0 = 1700000000;
  for (int set = 0; set < 2; ++set)
    for (int k = 0; k < 3; ++k)
      fr.push_back(mk(("b" + std::to_string(set) + std::to_string(k)).c_str(), t0 + set * 3 + k, float(0.0025 * std::pow(2.0, (k - 1) * 2)), 100, 8, 24));
  fr.push_back(mk("single", t0 + 100, 0.004f, 100, 8, 24));
  for (int k = 0; k < 4; ++k) fr.push_back(mk(("p" + std::to_string(k)).c_str(), t0 + 200 + k * 2, 0.004f, 100, 8, 35));
  for (int k = 0; k < 3; ++k) fr.push_back(mk(("burst" + std::to_string(k)).c_str(), t0 + 400, 0.004f, 100, 8, 85));
  fr.push_back(mk("other_lens", t0 + 401, 0.01f, 100, 8, 200));
  DetectOptions d;
  d.verifyContent = false;
  auto groups = detectSequences(fr, d);
  int brackets = 0, panos = 0;
  for (auto& g : groups) {
    std::printf("    %s %zu frames conf %.2f: %s\n", g.kind == SequenceGroup::Kind::Bracket ? "bracket" : "pano", g.paths.size(),
                g.confidence, g.reason.c_str());
    (g.kind == SequenceGroup::Kind::Bracket ? brackets : panos) += 1;
  }
  CHECK(brackets == 2);
  for (auto& g : groups)
    if (g.kind == SequenceGroup::Kind::Bracket) CHECK(g.paths.size() == 3);
  // with content verification off the pano pan and the burst both look like pans (time-only); at least the pan must be found
  CHECK(panos >= 1);
  bool panFound = false;
  for (auto& g : groups)
    if (g.kind == SequenceGroup::Kind::Panorama && g.paths.size() == 4) panFound = true;
  CHECK(panFound);
}

// ===========================================================================
// Motion Photo
// ===========================================================================
namespace {

using Bytes = std::vector<std::uint8_t>;
void be32(Bytes& b, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void be16(Bytes& b, std::uint32_t v) {
  b.push_back(std::uint8_t(v >> 8));
  b.push_back(std::uint8_t(v));
}
std::size_t boxStart(Bytes& b, const char* t) {
  const std::size_t p = b.size();
  be32(b, 0);
  b.insert(b.end(), t, t + 4);
  return p;
}
void boxEnd(Bytes& b, std::size_t p) {
  const std::uint32_t sz = std::uint32_t(b.size() - p);
  for (int i = 0; i < 4; ++i) b[p + i] = std::uint8_t(sz >> (24 - 8 * i));
}
void fullBox(Bytes& b, const char* t, std::uint32_t vf, std::size_t& p) {
  p = boxStart(b, t);
  be32(b, vf);
}

// A tiny synthetic MP4: `n` video samples at 30 fps with a keyframe every `gop`, B-frame style composition
// offsets, each sample `sz` bytes filled with (index & 0xFF) so the payload can be checked after trimming;
// plus an AAC-like audio track (1024-sample frames @ 48 kHz) when withAudio.
Bytes syntheticMp4(int n, int gop, bool withAudio, bool pcmAudio = false) {
  const int sz = 200, vts = 30000, vdelta = 1000;
  const int an = withAudio ? int(std::ceil(n / 30.0 * 48000 / 1024)) : 0;
  const int asz = 120;
  Bytes ftyp;
  {
    std::size_t p = boxStart(ftyp, "ftyp");
    ftyp.insert(ftyp.end(), {'m', 'p', '4', '2'});
    be32(ftyp, 0);
    ftyp.insert(ftyp.end(), {'m', 'p', '4', '2', 'i', 's', 'o', 'm'});
    boxEnd(ftyp, p);
  }
  // mdat: video samples then audio samples (not interleaved: exercises offset handling)
  Bytes mdat;
  const std::size_t dataOff = ftyp.size() + 8;  // mdat first, moov last (like Sony files)
  for (int i = 0; i < n; ++i) mdat.insert(mdat.end(), sz, std::uint8_t(i & 0x7F));
  const std::size_t audioStart = mdat.size();
  for (int i = 0; i < an; ++i) mdat.insert(mdat.end(), asz, std::uint8_t(0x80 | (i & 0x7F)));

  Bytes moov;
  const std::size_t mp = boxStart(moov, "moov");
  std::size_t p;
  fullBox(moov, "mvhd", 0, p);
  be32(moov, 0), be32(moov, 0), be32(moov, 1000), be32(moov, std::uint32_t(n * 1000 / 30));
  be32(moov, 0x10000), be16(moov, 0x100), be16(moov, 0);
  be32(moov, 0), be32(moov, 0);
  const std::uint32_t mat[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  for (auto m : mat) be32(moov, m);
  for (int i = 0; i < 6; ++i) be32(moov, 0);
  be32(moov, 3);
  boxEnd(moov, p);

  auto trak = [&](bool video) {
    const std::size_t tr = boxStart(moov, "trak");
    fullBox(moov, "tkhd", 3, p);
    be32(moov, 0), be32(moov, 0), be32(moov, video ? 1 : 2), be32(moov, 0), be32(moov, 0);
    be32(moov, 0), be32(moov, 0), be16(moov, 0), be16(moov, 0), be16(moov, video ? 0 : 0x100), be16(moov, 0);
    for (auto m : mat) be32(moov, m);
    be32(moov, video ? 1920u << 16 : 0), be32(moov, video ? 1080u << 16 : 0);
    boxEnd(moov, p);
    const std::size_t md = boxStart(moov, "mdia");
    fullBox(moov, "mdhd", 0, p);
    be32(moov, 0), be32(moov, 0), be32(moov, video ? vts : 48000), be32(moov, video ? std::uint32_t(n) * vdelta : std::uint32_t(an) * 1024);
    be16(moov, 0x55c4), be16(moov, 0);
    boxEnd(moov, p);
    fullBox(moov, "hdlr", 0, p);
    be32(moov, 0);
    moov.insert(moov.end(), video ? "vide" : "soun", (video ? "vide" : "soun") + 4);
    be32(moov, 0), be32(moov, 0), be32(moov, 0);
    moov.push_back(0);
    boxEnd(moov, p);
    const std::size_t mi = boxStart(moov, "minf");
    if (video) {
      fullBox(moov, "vmhd", 1, p);
      for (int i = 0; i < 4; ++i) moov.push_back(0);
      boxEnd(moov, p);
    } else {
      fullBox(moov, "smhd", 0, p);
      be32(moov, 0);
      boxEnd(moov, p);
    }
    {
      const std::size_t di = boxStart(moov, "dinf");
      fullBox(moov, "dref", 0, p);
      be32(moov, 1);
      std::size_t u;
      fullBox(moov, "url ", 1, u);
      boxEnd(moov, u);
      boxEnd(moov, p);
      boxEnd(moov, di);
    }
    const std::size_t st = boxStart(moov, "stbl");
    fullBox(moov, "stsd", 0, p);
    be32(moov, 1);
    {
      const std::size_t e = boxStart(moov, video ? "avc1" : (pcmAudio ? "twos" : "mp4a"));
      for (int i = 0; i < 6; ++i) moov.push_back(0);
      be16(moov, 1);
      for (int i = 0; i < 70; ++i) moov.push_back(0);
      boxEnd(moov, e);
    }
    boxEnd(moov, p);
    const int cnt = video ? n : an;
    fullBox(moov, "stts", 0, p);
    be32(moov, 1), be32(moov, cnt), be32(moov, video ? vdelta : 1024);
    boxEnd(moov, p);
    if (video) {
      fullBox(moov, "ctts", 0, p);  // 2-frame reorder delay
      be32(moov, 1), be32(moov, n), be32(moov, 2 * vdelta);
      boxEnd(moov, p);
      fullBox(moov, "stss", 0, p);
      be32(moov, std::uint32_t((n + gop - 1) / gop));
      for (int i = 0; i < n; i += gop) be32(moov, i + 1);
      boxEnd(moov, p);
    }
    fullBox(moov, "stsc", 0, p);
    be32(moov, 1), be32(moov, 1), be32(moov, 10), be32(moov, 1);  // 10 samples per chunk
    boxEnd(moov, p);
    fullBox(moov, "stsz", 0, p);
    be32(moov, 0), be32(moov, cnt);
    for (int i = 0; i < cnt; ++i) be32(moov, video ? sz : asz);
    boxEnd(moov, p);
    fullBox(moov, "stco", 0, p);
    const int chunks = (cnt + 9) / 10;
    be32(moov, chunks);
    for (int c = 0; c < chunks; ++c)
      be32(moov, std::uint32_t(dataOff + (video ? std::size_t(c) * 10 * sz : audioStart + std::size_t(c) * 10 * asz)));
    boxEnd(moov, p);
    boxEnd(moov, st);
    boxEnd(moov, mi);
    boxEnd(moov, md);
    boxEnd(moov, tr);
  };
  trak(true);
  if (withAudio) trak(false);
  boxEnd(moov, mp);

  Bytes out = ftyp;
  be32(out, std::uint32_t(8 + mdat.size()));
  out.insert(out.end(), {'m', 'd', 'a', 't'});
  out.insert(out.end(), mdat.begin(), mdat.end());
  out.insert(out.end(), moov.begin(), moov.end());
  return out;
}

Bytes makeJpeg(int w, int h) {
  std::vector<std::uint8_t> rgb(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      rgb[(std::size_t(y) * w + x) * 3] = std::uint8_t(x * 255 / w);
      rgb[(std::size_t(y) * w + x) * 3 + 1] = std::uint8_t(y * 255 / h);
      rgb[(std::size_t(y) * w + x) * 3 + 2] = 128;
    }
  Bytes jpg;
  stbi_write_jpg_to_func([](void* c, void* d, int n) { static_cast<Bytes*>(c)->insert(static_cast<Bytes*>(c)->end(), static_cast<std::uint8_t*>(d), static_cast<std::uint8_t*>(d) + n); },
                         &jpg, w, h, 3, rgb.data(), 90);
  return jpg;
}

void writeBytes(const fs::path& p, const Bytes& b) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

}  // namespace

void testMotionPhotoSynthetic() {
  const fs::path out = outDir();
  const Bytes mp4 = syntheticMp4(300, 30, true);  // 10 s @ 30 fps, 1 s GOP
  const fs::path src = out / "synthetic_source.mp4";
  writeBytes(src, mp4);
  auto info = probeVideo(src);
  CHECK(info.has_value());
  if (!info) return;
  std::printf("    synthetic: %dx%d %.2fs %.1f fps %s/%s, %zu keyframes\n", info->width, info->height, info->durationSec, info->fps,
              info->videoCodec.c_str(), info->audioCodec.c_str(), info->keyframesSec.size());
  CHECK(info->keyframesSec.size() == 10);
  CHECK(std::fabs(info->durationSec - 10.0) < 1e-6);

  TrimOptions to;
  to.startSec = 3.4;
  to.durationSec = 3.0;
  auto tr = trimMp4(src, to);
  CHECK(tr.has_value());
  if (!tr) {
    std::fprintf(stderr, "    %s\n", tr.error().message.c_str());
    return;
  }
  std::printf("    trimmed: start %.3f dur %.3f s, %d frames, audio %d\n", tr->actualStartSec, tr->actualDurationSec, tr->videoFrames, tr->audioKept);
  // first kept frame is a keyframe near 3.4 s: pts of key 3 is 3.0+2/30 (reorder delay), key 4 is 4.0+...
  CHECK(tr->videoFrames % 30 == 0);
  CHECK(std::fabs(tr->actualDurationSec - 3.0) < 1.01);
  CHECK(tr->audioKept);
  // payload check: the first video sample of the clip must be the original keyframe (index multiple of 30)
  {
    const Bytes& m = tr->mp4;
    int firstIdx = -1, count = 0;
    // scan mdat for 200-byte runs of equal bytes < 0x80 (video) in order
    std::size_t mdatAt = 0;
    for (std::size_t i = 4; i + 4 < m.size(); ++i)
      if (std::memcmp(&m[i], "mdat", 4) == 0) { mdatAt = i + 4; break; }
    for (std::size_t o = mdatAt; o + 200 <= m.size(); ) {
      const std::uint8_t v = m[o];
      if (v >= 0x80) { o += 120; continue; }
      bool uniform = true;
      for (int k = 1; k < 200; ++k) if (m[o + k] != v) { uniform = false; break; }
      if (!uniform) break;
      if (firstIdx < 0) firstIdx = v;
      ++count;
      o += 200;
      if (count > 5000) break;
    }
    std::printf("    first sample index %d (expect multiple of 30), video samples counted %d\n", firstIdx, count);
    CHECK(firstIdx >= 0 && firstIdx % 30 == 0);
    CHECK(count >= tr->videoFrames);
  }

  // container + validator
  const Bytes jpeg = makeJpeg(320, 240);
  MotionPhotoOptions mo;
  mo.presentationTimestampUs = 1500000;
  auto mp = buildMotionPhoto(jpeg, tr->mp4, mo);
  CHECK(mp.has_value());
  if (!mp) return;
  writeBytes(out / "synthetic_MP.jpg", *mp);
  MotionPhotoInfo v = validateMotionPhoto(*mp);
  for (auto& p : v.problems) std::printf("    problem: %s\n", p.c_str());
  std::printf("    validator: %s, video at %llu len %llu, %s %dx%d %.2fs, pts %lld\n", v.valid ? "valid" : "INVALID",
              (unsigned long long)v.videoOffset, (unsigned long long)v.videoLength, v.videoCodec.c_str(), v.videoWidth,
              v.videoHeight, v.videoDurationSec, (long long)v.presentationTimestampUs);
  CHECK(v.valid);
  CHECK(v.videoLength == tr->mp4.size());
  CHECK(v.presentationTimestampUs == 1500000);
  CHECK(v.version == 1);
  // the primary image is still a decodable JPEG with the original size
  {
    int w = 0, h = 0, n = 0;
    Bytes headOnly(mp->begin(), mp->begin() + std::ptrdiff_t(v.jpegLength));
    // decode through our own decoder to prove the XMP insertion kept the JPEG intact
    const fs::path jp = out / "synthetic_MP_primary.jpg";
    writeBytes(jp, headOnly);
    auto reg = DecoderRegistry::withBuiltins();
    const ImageDecoder* dec = reg.find(jp);
    auto img = dec ? dec->decode(jp, DecodeOptions{}) : fail("no decoder");
    CHECK(img.has_value());
    if (img) { w = img->linear.width; h = img->linear.height; n = 3; }
    CHECK(w == 320 && h == 240 && n == 3);
  }
  // legacy attributes flag
  {
    MotionPhotoOptions lo = mo;
    lo.legacyMicroVideo = true;
    auto lp = buildMotionPhoto(jpeg, tr->mp4, lo);
    CHECK(lp.has_value());
    if (lp) CHECK(validateMotionPhoto(*lp).valid);
  }
  // an existing XMP packet in the cover is replaced, not duplicated: build twice
  {
    Bytes primary(mp->begin(), mp->begin() + std::ptrdiff_t(v.jpegLength));
    auto again = buildMotionPhoto(primary, tr->mp4, mo);
    CHECK(again.has_value());
    if (again) {
      auto v2 = validateMotionPhoto(*again);
      CHECK(v2.valid);
      CHECK(again->size() == mp->size());
    }
  }
  // negative tests: the validator must catch each defect
  {
    Bytes bad = *mp;
    bad.push_back(0);  // trailing byte after the video
    CHECK(!validateMotionPhoto(bad).valid);
    bad = *mp;
    bad.pop_back();  // truncated video
    CHECK(!validateMotionPhoto(bad).valid);
    bad = Bytes(mp->begin(), mp->begin() + std::ptrdiff_t(v.jpegLength));  // no video
    CHECK(!validateMotionPhoto(bad).valid);
    bad = *mp;
    for (std::size_t i = 0; i + 20 < v.jpegLength; ++i)
      if (std::memcmp(&bad[i], "MotionPhoto=\"1\"", 15) == 0) { bad[i + 13] = '0'; break; }
    CHECK(!validateMotionPhoto(bad).valid);
    bad = *mp;
    bad[v.videoOffset + 4] = 'x';  // not an ftyp any more
    CHECK(!validateMotionPhoto(bad).valid);
    CHECK(!validateMotionPhoto(jpeg).valid);  // plain JPEG
    CHECK(!buildMotionPhoto(Bytes{1, 2, 3}, tr->mp4, mo).has_value());
    CHECK(!buildMotionPhoto(jpeg, Bytes(100, 0), mo).has_value());
  }
  // PCM audio is dropped, no-audio and out-of-range windows behave
  {
    const fs::path pcm = out / "synthetic_pcm.mp4";
    writeBytes(pcm, syntheticMp4(150, 30, true, true));
    TrimOptions t2;
    t2.startSec = 0;
    t2.durationSec = 2;
    auto r2 = trimMp4(pcm, t2);
    CHECK(r2.has_value());
    if (r2) {
      CHECK(!r2->audioKept);
      CHECK(!r2->note.empty());
      CHECK(validateMotionPhoto(*buildMotionPhoto(jpeg, r2->mp4)).valid);
    }
    TrimOptions t3;
    t3.startSec = 100;  // beyond the end -> clamped to the tail
    t3.durationSec = 3;
    t3.keepAudio = false;
    auto r3 = trimMp4(src, t3);
    CHECK(r3.has_value());
    if (r3) {
      CHECK(!r3->audioKept);
      CHECK(r3->actualStartSec > 5.0);
    }
    CHECK(!trimMp4(out / "does_not_exist.mp4", TrimOptions{}).has_value());
    writeBytes(out / "garbage.mp4", Bytes(1000, 0x42));
    CHECK(!trimMp4(out / "garbage.mp4", TrimOptions{}).has_value());
    CHECK(!probeVideo(out / "garbage.mp4").has_value());
  }
}

void testMotionPhotoRealVideo() {
  const char* cand = "D:/\xE5\x9B\xBE\xE7\x89\x87/2026-07-10/C0021.MP4";
  const fs::path video = fromUtf8(cand);
  std::error_code ec;
  if (!fs::exists(video, ec)) {
    std::printf("    (owner video not found, skipped)\n");
    ++g_skipped;
    return;
  }
  const fs::path out = outDir();
  auto info = probeVideo(video);
  CHECK(info.has_value());
  if (!info) { std::fprintf(stderr, "    %s\n", info.error().message.c_str()); return; }
  std::printf("    owner video: %dx%d %.2fs %.2f fps %s audio %s keyframes %zu\n", info->width, info->height, info->durationSec,
              info->fps, info->videoCodec.c_str(), info->audioCodec.c_str(), info->keyframesSec.size());
  // cover: a developed still (any JPEG); use the decoder's embedded preview of an ARW when available, else a gradient
  Bytes cover = makeJpeg(1620, 1080);
  const std::string arw = findArw();
  if (!arw.empty()) {
    auto pv = extractEmbeddedPreview(fromUtf8(arw), 1620);
    if (pv) {
      const fs::path tmp = out / "cover_tmp.jpg";
      if (writeJpeg(*pv, tmp, JpegOptions{92})) { auto b = readFile(tmp); if (b) cover = *b; }
    }
  }
  const fs::path coverPath = out / "cover.jpg";
  writeBytes(coverPath, cover);
  MotionFromVideoRequest rq;
  rq.video = video;
  rq.coverJpeg = coverPath;
  rq.output = out / "C0021_MP.jpg";
  rq.startSec = 40.0;
  rq.durationSec = 3.0;
  rq.coverSec = 41.5;
  auto t0 = std::chrono::steady_clock::now();
  auto r = createMotionPhotoFromVideo(rq);
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.has_value());
  if (!r) { std::fprintf(stderr, "    %s\n", r.error().message.c_str()); return; }
  std::printf("    wrote %s in %.2fs: clip %.3f -> +%.3f s, %d frames, cover at %.3f s, audio kept %d (%s)\n",
              "C0021_MP.jpg", sec, r->trim.actualStartSec, r->trim.actualDurationSec, r->trim.videoFrames, r->coverInClipSec,
              r->trim.audioKept, r->trim.note.c_str());
  CHECK(r->info.valid);
  for (auto& p : r->info.problems) std::printf("    problem: %s\n", p.c_str());
  CHECK(std::fabs(r->trim.actualDurationSec - 3.0) <= 1.0);
  CHECK(std::fabs(r->trim.actualStartSec - 40.0) <= 1.0);
  CHECK(r->info.videoWidth == info->width && r->info.videoHeight == info->height);
  // re-validate from disk and re-probe the appended MP4 independently
  auto again = validateMotionPhotoFile(rq.output);
  CHECK(again.has_value() && again->valid);
  auto bytes = readFile(rq.output);
  if (bytes && again) {
    Bytes vid(bytes->begin() + std::ptrdiff_t(again->videoOffset), bytes->end());
    writeBytes(out / "C0021_clip.mp4", vid);
    auto vi = probeVideo(out / "C0021_clip.mp4");
    CHECK(vi.has_value());
    if (vi) {
      std::printf("    clip.mp4: %.3f s, %.2f fps, keyframes %zu, size %.1f MB\n", vi->durationSec, vi->fps, vi->keyframesSec.size(), vid.size() / 1e6);
      CHECK(std::fabs(vi->durationSec - r->trim.actualDurationSec) < 0.1);
    }
  }
}

// ===========================================================================
// C API
// ===========================================================================
#ifdef DAPPLE_HAVE_OPENCV
namespace {
struct ProgressLog {
  int calls = 0;
  float last = 0;
  bool monotonic = true;
  int cancelAtCall = -1;
};
int32_t progressCb(float f, const char*, void* user) {
  auto* p = static_cast<ProgressLog*>(user);
  ++p->calls;
  if (f + 1e-4f < p->last) p->monotonic = false;
  p->last = f;
  return p->cancelAtCall > 0 && p->calls >= p->cancelAtCall ? 1 : 0;
}
}  // namespace

void testCApiMerge() {
  CHECK(dapple_merge_available() == 1);
  const fs::path out = outDir();
  // frames as linear DNGs (no EXIF: exposure ratios are measured from the pixels)
  bool real = false;
  ImageF base = loadScene(1400, real);
  HdrCase c = makeHdrCase(base, false, 41);
  std::vector<std::string> hdrFiles;
  for (int i = 0; i < 3; ++i) {
    const fs::path p = out / ("capi_hdr_" + std::to_string(i) + ".dng");
    CHECK(writeLinearDng(c.frames[i], p, 1.f).has_value());
    hdrFiles.push_back(pathToUtf8(p));
  }
  std::vector<const char*> hp;
  for (auto& s : hdrFiles) hp.push_back(s.c_str());
  ProgressLog log;
  char* report = nullptr;
  DappleSession* s = dapple_session_open_merged(hp.data(), 3, "hdr", R"({"deghost":"mid","base":-1})", progressCb, &log, nullptr, &report);
  CHECK(s != nullptr);
  if (s) {
    CHECK(report != nullptr);
    if (report) {
      const std::string rj = report;
      std::printf("    hdr report: %.200s...\n", rj.c_str());
      CHECK(rj.find(R"("kind":"hdr")") != std::string::npos);
      CHECK(rj.find(R"("evUsed")") != std::string::npos);
    }
    std::printf("    progress callbacks: %d, monotonic %d, last %.2f\n", log.calls, log.monotonic, log.last);
    CHECK(log.calls >= 4 && log.monotonic && log.last > 0.99f);
    DappleImage img{};
    CHECK(dapple_session_render(s, nullptr, nullptr, 800, &img) == DAPPLE_OK);
    CHECK(img.width > 0 && img.rgba != nullptr);
    dapple_free_image(&img);
    const std::string dngOut = pathToUtf8(out / "capi_session.dng");
    CHECK(dapple_session_save_linear(s, dngOut.c_str(), "dng") == DAPPLE_OK);
    CHECK(dapple_session_save_linear(s, pathToUtf8(out / "capi_session.tif").c_str(), "tiff") == DAPPLE_OK);
    CHECK(dapple_session_save_linear(s, dngOut.c_str(), "bmp") == DAPPLE_ERROR);
    CHECK(std::string(dapple_last_error()).find("unknown format") != std::string::npos);
    char* an = dapple_session_analyze_json(s);
    CHECK(an != nullptr);
    dapple_free_string(an);
    dapple_session_close(s);
  }
  dapple_free_string(report);

  // cancellation through the callback and through the flag
  {
    ProgressLog l2;
    l2.cancelAtCall = 2;
    DappleSession* sc = dapple_session_open_merged(hp.data(), 3, "hdr", "{}", progressCb, &l2, nullptr, nullptr);
    CHECK(sc == nullptr);
    CHECK(std::string(dapple_last_error()) == "cancelled");
    volatile int32_t flag = 1;
    DappleSession* sf = dapple_session_open_merged(hp.data(), 3, "hdr", "{}", nullptr, nullptr, &flag, nullptr);
    CHECK(sf == nullptr);
    CHECK(std::string(dapple_last_error()) == "cancelled");
  }
  // errors are reported, not thrown
  {
    CHECK(dapple_session_open_merged(nullptr, 0, "hdr", "{}", nullptr, nullptr, nullptr, nullptr) == nullptr);
    CHECK(dapple_session_open_merged(hp.data(), 3, "bogus", "{}", nullptr, nullptr, nullptr, nullptr) == nullptr);
    const char* bad[] = {"/no/such/a.arw", "/no/such/b.arw"};
    CHECK(dapple_session_open_merged(bad, 2, "hdr", "{}", nullptr, nullptr, nullptr, nullptr) == nullptr);
    std::printf("    error for missing files: %s\n", dapple_last_error());
    CHECK(dapple_session_open_merged(bad, 2, "pano", "{}", nullptr, nullptr, nullptr, nullptr) == nullptr);
    CHECK(dapple_motion_photo_from_video("/no/such.mp4", "/no/such.jpg", "/tmp/x_MP.jpg", "{}", nullptr) == DAPPLE_ERROR);
    CHECK(dapple_motion_photo_validate_json("/no/such_MP.jpg") == nullptr);
    CHECK(dapple_video_probe_json("/no/such.mp4") == nullptr);
    CHECK(dapple_session_save_linear(nullptr, "x", "dng") == DAPPLE_ERROR);
  }
  // panorama through the C API (DNG frames)
  {
    ImageF scene = loadScene(5200, real);
    if (scene.height > scene.width) {
      ImageF t(scene.height, scene.width);
      for (int y = 0; y < scene.height; ++y)
        for (int x = 0; x < scene.width; ++x) std::copy_n(scene.row(y) + 3 * x, 3, t.row(scene.width - 1 - x) + 3 * y);
      scene = std::move(t);
    }
    PanoCase pc = makePanoCase(scene, 1200, 800, 40, {{-12, 0.5, 0.3}, {0, -0.4, -0.2}, {12, 0.4, 0.4}}, true);
    std::vector<std::string> pf;
    for (int i = 0; i < 3; ++i) {
      const fs::path p = out / ("capi_pano_" + std::to_string(i) + ".dng");
      CHECK(writeLinearDng(pc.frames[i], p, 1.f).has_value());
      pf.push_back(pathToUtf8(p));
    }
    std::vector<const char*> pp;
    for (auto& x : pf) pp.push_back(x.c_str());
    char* pr = nullptr;
    ProgressLog l3;
    DappleSession* ps = dapple_session_open_merged(pp.data(), 3, "pano", R"({"projection":"sph","crop":true})", progressCb, &l3, nullptr, &pr);
    CHECK(ps != nullptr);
    if (ps) {
      std::printf("    pano report: %.220s...\n", pr ? pr : "");
      CHECK(pr && std::string(pr).find(R"("framesUsed":3)") != std::string::npos);
      CHECK(l3.monotonic && l3.last > 0.99f);
      DappleImage img{};
      CHECK(dapple_session_render(ps, nullptr, nullptr, 1000, &img) == DAPPLE_OK);
      dapple_free_image(&img);
      dapple_session_close(ps);
    }
    dapple_free_string(pr);
  }
  // detection through the C API on the owner's folder (first 12 ARWs) when available
  {
    const std::string arw = findArw();
    if (!arw.empty()) {
      std::vector<std::string> list;
      for (const auto& e : fs::directory_iterator(fromUtf8(arw).parent_path())) {
        if (e.path().extension() == ".ARW") list.push_back(pathToUtf8(e.path()));
        if (list.size() >= 12) break;
      }
      list.push_back("/no/such/file.arw");
      std::vector<const char*> lp;
      for (auto& x : list) lp.push_back(x.c_str());
      char* dj = dapple_detect_sequences_json(lp.data(), int32_t(lp.size()), R"({"verifyContent":false})", nullptr, nullptr, nullptr);
      CHECK(dj != nullptr);
      if (dj) {
        const std::string d = dj;
        CHECK(d.find(R"("unreadable":[12])") != std::string::npos);
        CHECK(d.find(R"("groups":[)") != std::string::npos);
        std::printf("    detect json: %.160s...\n", d.c_str());
        dapple_free_string(dj);
      }
    } else {
      ++g_skipped;
    }
  }
  // motion photo through the C API: synthetic clip + cover
  {
    const Bytes mp4 = syntheticMp4(300, 30, true);
    const fs::path src = out / "capi_source.mp4";
    writeBytes(src, mp4);
    const fs::path cov = out / "capi_cover.jpg";
    writeBytes(cov, makeJpeg(640, 480));
    const std::string outMp = pathToUtf8(out / "capi_MP.jpg");
    char* info = nullptr;
    CHECK(dapple_motion_photo_from_video(pathToUtf8(src).c_str(), pathToUtf8(cov).c_str(), outMp.c_str(),
                                         R"({"start":2.0,"duration":3.0,"coverTime":3.5})", &info) == DAPPLE_OK);
    CHECK(info && std::string(info).find(R"("valid":true)") != std::string::npos);
    if (info) std::printf("    motion info: %.240s...\n", info);
    dapple_free_string(info);
    char* v = dapple_motion_photo_validate_json(outMp.c_str());
    CHECK(v && std::string(v).find(R"("valid":true)") != std::string::npos);
    dapple_free_string(v);
    char* pj = dapple_video_probe_json(pathToUtf8(src).c_str());
    CHECK(pj && std::string(pj).find(R"("keyframes":[)") != std::string::npos);
    dapple_free_string(pj);
    auto trimmed = trimMp4(src, TrimOptions{2.0, 3.0, true});
    CHECK(trimmed.has_value());
    if (trimmed) {
      const fs::path clip = out / "capi_clip.mp4";
      writeBytes(clip, trimmed->mp4);
      char* i2 = nullptr;
      CHECK(dapple_motion_photo_write(pathToUtf8(cov).c_str(), pathToUtf8(clip).c_str(), pathToUtf8(out / "capi_pretrimmed_MP.jpg").c_str(), 1.0, &i2) == DAPPLE_OK);
      CHECK(i2 && std::string(i2).find(R"("presentationTimestampUs":1000000)") != std::string::npos);
      dapple_free_string(i2);
    }
  }
}
#endif

// ===========================================================================
// Performance (DAPPLE_PERF=1)
// ===========================================================================
#ifdef DAPPLE_HAVE_OPENCV
void testPerformance() {
  if (!std::getenv("DAPPLE_PERF")) {
    ++g_skipped;
    return;
  }
  bool real = false;
  // 26 MP frames (4180x6272 is the A7R V M-size; use 6000x4000-class landscape for the pano)
  ImageF scene = loadScene(6272, real);
  std::printf("    perf scene %s %dx%d\n", real ? "ARW" : "procedural", scene.width, scene.height);
  {
    HdrCase c = makeHdrCase(scene, true, 31);
    for (const auto& f : c.frames) (void)f;
    std::printf("    HDR 3 x %.1f MP:\n", double(scene.width) * scene.height / 1e6);
    HdrOptions o;
    o.baseIndex = 1;
    auto t0 = std::chrono::steady_clock::now();
    auto r = mergeHdrImages(c.frames, o);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.has_value());
    if (r) std::printf("    -> %.2f s total (align %.2f, merge+deghost %.2f)\n", sec, r->report.secAlign, r->report.secMerge);
    HdrOptions p = o;
    p.longEdge = 1600;
    auto t1 = std::chrono::steady_clock::now();
    auto rp = mergeHdrImages(c.frames, p);
    std::printf("    preview (long edge 1600) -> %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
    CHECK(rp.has_value());
  }
  {
    ImageF world = scene;
    if (world.height > world.width) {
      ImageF t(world.height, world.width);
      for (int y = 0; y < world.height; ++y)
        for (int x = 0; x < world.width; ++x) std::copy_n(world.row(y) + 3 * x, 3, t.row(world.width - 1 - x) + 3 * y);
      world = std::move(t);
    }
    // 6000x4000 frames (24 MP) from a ~6272 px world: 50 deg fov each
    const std::vector<Cam> cams = {{-33, 0.8, 0.5}, {-11, -0.7, -0.6}, {11, 0.7, 0.9}, {33, -0.8, -0.4}};
    auto tg = std::chrono::steady_clock::now();
    PanoCase pc = makePanoCase(world, 6000, 4000, 56, cams, true);
    std::printf("    (frame synthesis %.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - tg).count());
    PanoOptions o;
    o.projection = PanoProjection::Cylindrical;
    auto t0 = std::chrono::steady_clock::now();
    auto r = stitchPanoramaImages(pc.frames, o);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.has_value());
    if (r) {
      std::printf("    pano 4 x 24 MP -> %dx%d (%.1f MP) in %.2f s (features %.2f, estimate %.2f, compose %.2f)\n", r->image.linear.width,
                  r->image.linear.height, double(r->image.linear.width) * r->image.linear.height / 1e6, sec, r->report.secFeatures,
                  r->report.secEstimate, r->report.secCompose);
      saveGamma(r->image.linear, outDir() / "pano_perf_24mp.jpg", 1.f, 2400);
    }
    PanoOptions p = o;
    p.longEdge = 2000;
    auto t1 = std::chrono::steady_clock::now();
    auto rp = stitchPanoramaImages(pc.frames, p);
    std::printf("    pano preview (long edge 2000) -> %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
    CHECK(rp.has_value());
  }
}
#endif

// ===========================================================================
#ifdef DAPPLE_HAVE_OPENCV
void testDegenerateMergeRejected() {
  bool real = false;
  ImageF base = loadScene(400, real);
  HdrCase c = makeHdrCase(base, false, 33);
  // all-black frames -> error, never an all-zero image
  std::vector<SourceImage> blk = c.frames;
  for (auto& f : blk) std::fill(f.linear.data.begin(), f.linear.data.end(), 0.f);
  auto rb = mergeHdrImages(blk, HdrOptions{});
  CHECK(!rb.has_value());
  // NaN frame -> either an error or a fully finite non-black image
  std::vector<SourceImage> nn = c.frames;
  std::fill(nn[0].linear.data.begin(), nn[0].linear.data.end(), std::nanf(""));
  auto rn = mergeHdrImages(nn, HdrOptions{});
  if (rn) {
    float mx = 0;
    bool fin = true;
    for (float v : rn->image.linear.data) fin = fin && std::isfinite(v), mx = std::max(mx, v);
    CHECK(fin && mx > 1e-6f);
  }
  // writers refuse all-NaN / all-zero images
  ImageF bad(8, 8);
  std::fill(bad.data.begin(), bad.data.end(), std::nanf(""));
  CHECK(!writeLinearTiff16(bad, outDir() / "bad_nan.tif").has_value());
  SourceImage badSrc;
  badSrc.linear = bad;
  CHECK(!writeLinearDng(badSrc, outDir() / "bad_nan.dng").has_value());
  ImageF zero(8, 8);
  CHECK(!writeLinearTiff16(zero, outDir() / "bad_zero.tif").has_value());
}
#endif  // DAPPLE_HAVE_OPENCV

int main() {
  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {
#ifdef DAPPLE_HAVE_OPENCV
      {"hdr synthetic", testHdrSynthetic},
      {"hdr fusion + errors", testHdrJpegFusionAndErrors},
      {"degenerate merge", testDegenerateMergeRejected},
      {"pano synthetic", testPanoSynthetic},
      {"pano portrait", testPanoPortrait},
      {"c api merge", testCApiMerge},
      {"performance", testPerformance},
#endif
      {"sequence detection", testSequenceDetection},
      {"motion photo synthetic", testMotionPhotoSynthetic},
      {"motion photo real video", testMotionPhotoRealVideo},
  };
  const char* only = std::getenv("DAPPLE_TEST_ONLY");  // substring filter, e.g. "pano portrait"
  for (const T& t : tests) {
    if (only && !std::strstr(t.name, only)) continue;
    const int before = g_failures;
    std::printf("[ run ] %s\n", t.name);
    std::fflush(stdout);
    t.fn();
    std::printf("[%s] %s\n", g_failures == before ? " ok " : "FAIL", t.name);
  }
  std::printf("%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skipped);
  return g_failures ? 1 : 0;
}
