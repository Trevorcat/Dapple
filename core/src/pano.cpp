// Panorama stitching on developed-linear float frames using OpenCV's
// stitching detail pipeline (Apache-2.0): AKAZE features -> pairwise matching
// -> homography + ray bundle adjustment -> wave correction -> projection
// (spherical / cylindrical / plane) -> graph-cut seams -> multi-band blending.
//
// Estimation runs on ~0.6 MP gamma-encoded copies; the blend runs on the
// linear float data (encoded to 13-bit gamma only inside the multi-band
// blender, whose pyramids are 16-bit integer) at full or reduced resolution.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

#include "dapple/merge.h"
#include "file_util.h"
#include "merge_internal.h"

#ifdef DAPPLE_HAVE_OPENCV
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/stitching.hpp>
#include <opencv2/stitching/detail/autocalib.hpp>
#include <opencv2/stitching/detail/blenders.hpp>
#include <opencv2/stitching/detail/camera.hpp>
#include <opencv2/stitching/detail/exposure_compensate.hpp>
#include <opencv2/stitching/detail/matchers.hpp>
#include <opencv2/stitching/detail/motion_estimators.hpp>
#include <opencv2/stitching/detail/seam_finders.hpp>
#include <opencv2/stitching/detail/util.hpp>
#include <opencv2/stitching/detail/warpers.hpp>
#endif

namespace dapple {

using namespace mergeutil;

const char* toString(PanoProjection p) {
  switch (p) {
    case PanoProjection::Spherical: return "sph";
    case PanoProjection::Cylindrical: return "cyl";
    case PanoProjection::Perspective: return "persp";
  }
  return "cyl";
}

PanoProjection panoProjectionFromString(const std::string& s, PanoProjection fallback) {
  if (s == "sph" || s == "spherical") return PanoProjection::Spherical;
  if (s == "cyl" || s == "cylindrical") return PanoProjection::Cylindrical;
  if (s == "persp" || s == "perspective" || s == "plane") return PanoProjection::Perspective;
  return fallback;
}

#ifndef DAPPLE_HAVE_OPENCV

Result<PanoResult> stitchPanoramaImages(std::vector<SourceImage>, const PanoOptions&, const MergeControl&) {
  return fail("Panorama stitching is not available: built without OpenCV (DAPPLE_WITH_OPENCV=OFF)");
}

#else

namespace {

constexpr float kEncMax = 4.f;      // linear value mapped to the top of the blender range
constexpr float kEncRange = 8000.f; // 13-bit: blender pyramids add up to ~3x this and must stay below 32767

inline short encodeBlend(float x) {
  const float t = std::clamp(x / kEncMax, 0.f, 1.f);
  return short(std::pow(t, 1.f / 2.2f) * kEncRange + 0.5f);
}
inline float decodeBlend(short v) {
  const float t = std::clamp(float(v) / kEncRange, 0.f, 1.f);
  return std::pow(t, 2.2f) * kEncMax;
}

cv::Mat gamma8(const ImageF& im, double scale) {
  const int w = std::max(8, int(std::lround(im.width * scale))), h = std::max(8, int(std::lround(im.height * scale)));
  cv::Mat small;
  cv::resize(asMat(im), small, cv::Size(w, h), 0, 0, cv::INTER_AREA);
  cv::Mat out(h, w, CV_8UC3);
  for (int y = 0; y < h; ++y) {
    const float* s = small.ptr<float>(y);
    std::uint8_t* d = out.ptr<std::uint8_t>(y);
    for (int i = 0; i < w * 3; ++i)
      d[i] = std::uint8_t(std::pow(std::clamp(s[i], 0.f, 1.f), 1.f / 2.2f) * 255.f + 0.5f);
  }
  return out;
}

cv::Ptr<cv::WarperCreator> makeWarper(PanoProjection p) {
  switch (p) {
    case PanoProjection::Spherical: return cv::makePtr<cv::SphericalWarper>();
    case PanoProjection::Cylindrical: return cv::makePtr<cv::CylindricalWarper>();
    default: return cv::makePtr<cv::PlaneWarper>();
  }
}

// Largest axis-aligned rectangle of non-zero cells (maximal rectangle in a binary matrix).
cv::Rect largestRect(const cv::Mat& bin) {
  const int w = bin.cols, h = bin.rows;
  std::vector<int> heights(w, 0);
  long best = 0;
  cv::Rect bestRect;
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* row = bin.ptr<std::uint8_t>(y);
    for (int x = 0; x < w; ++x) heights[x] = row[x] ? heights[x] + 1 : 0;
    std::vector<int> st;
    for (int x = 0; x <= w; ++x) {
      const int cur = x == w ? 0 : heights[x];
      while (!st.empty() && heights[st.back()] >= cur) {
        const int hh = heights[st.back()];
        st.pop_back();
        const int left = st.empty() ? 0 : st.back() + 1;
        const long area = long(hh) * (x - left);
        if (area > best) {
          best = area;
          bestRect = cv::Rect(left, y - hh + 1, x - left, hh);
        }
      }
      st.push_back(x);
    }
  }
  return bestRect;
}

void rotationAngles(const cv::Mat& R, double& yaw, double& pitch, double& roll) {
  const double r00 = R.at<double>(0, 0), r01 = R.at<double>(0, 1), r02 = R.at<double>(0, 2);
  const double r10 = R.at<double>(1, 0), r11 = R.at<double>(1, 1), r12 = R.at<double>(1, 2);
  const double r22 = R.at<double>(2, 2);
  (void)r01;
  (void)r00;
  yaw = std::atan2(r02, r22) * 180.0 / CV_PI;
  pitch = -std::asin(std::clamp(r12, -1.0, 1.0)) * 180.0 / CV_PI;
  roll = std::atan2(r10, r11) * 180.0 / CV_PI;
}

}  // namespace

Result<PanoResult> stitchPanoramaImages(std::vector<SourceImage> frames, const PanoOptions& opt,
                                        const MergeControl& ctl) {
  using clk = std::chrono::steady_clock;
  try {
    const int n = int(frames.size());
    if (n < 2 || n > 20) return fail("Panorama needs 2 to 20 frames");
    for (const auto& f : frames)
      if (f.linear.empty()) return fail("Empty frame in panorama input");

    PanoResult result;
    PanoReport& rep = result.report;
    rep.projection = opt.projection;

    // ---------------- features ----------------
    auto t0 = clk::now();
    ctl.report(0.02f, "features");
    const double regPixels = 0.6e6;
    std::vector<double> regScale(n);
    std::vector<cv::Mat> regImgs(n);
    std::vector<cv::detail::ImageFeatures> features(n);
    auto finder = cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB, 0, 3, 0.0002f, 4, 4);
    for (int i = 0; i < n; ++i) {
      if (ctl.cancelled()) return fail("cancelled");
      const double px = double(frames[i].linear.width) * frames[i].linear.height;
      regScale[i] = std::min(1.0, std::sqrt(regPixels / px));
      regImgs[i] = gamma8(frames[i].linear, regScale[i]);
      {
        // Local contrast equalisation helps dark / low-contrast frames produce enough keypoints.
        cv::Mat gray, eq;
        cv::cvtColor(regImgs[i], gray, cv::COLOR_RGB2GRAY);
        cv::createCLAHE(3.0, cv::Size(8, 8))->apply(gray, eq);
        cv::detail::computeImageFeatures(finder, eq, features[i]);
      }
      features[i].img_idx = i;
      ctl.report(0.02f + 0.18f * (i + 1) / n, "features");
    }
    rep.secFeatures = secondsSince(t0);

    // ---------------- matching ----------------
    auto t1 = clk::now();
    ctl.report(0.22f, "match");
    std::vector<cv::detail::MatchesInfo> pairwise;
    {
      // 4th/5th parameters: OpenCV zeroes the confidence of pairs above 3.0 ("too close images"); with
      // strongly overlapping hand-held frames that would drop valid neighbours, so only reject absurd values.
      cv::detail::BestOf2NearestMatcher matcher(false, float(opt.matchConfidence), 6, 6, 1000.0);
      matcher(features, pairwise);
      matcher.collectGarbage();
    }
    if (ctl.cancelled()) return fail("cancelled");
    if (std::getenv("DAPPLE_PANO_DEBUG")) {
      for (int i = 0; i < n; ++i) std::fprintf(stderr, "  feat[%d]=%zu\n", i, features[i].keypoints.size());
      for (const auto& mi : pairwise)
        if (mi.src_img_idx < mi.dst_img_idx && mi.src_img_idx >= 0)
          std::fprintf(stderr, "  pair %d-%d matches %zu inliers %d conf %.2f\n", mi.src_img_idx, mi.dst_img_idx, mi.matches.size(), mi.num_inliers, mi.confidence);
    }
    std::vector<int> keep = cv::detail::leaveBiggestComponent(features, pairwise, 1.0f);
    if (keep.size() < 2)
      return fail("Panorama: the frames do not overlap enough to be stitched (no matching features)");
    if (int(keep.size()) < n)
      rep.warnings.push_back(std::to_string(n - int(keep.size())) + " frame(s) did not match the others and were left out");
    const int m = int(keep.size());
    rep.usedIndices = keep;
    rep.framesUsed = m;

    // ---------------- camera estimation ----------------
    ctl.report(0.3f, "estimate");
    std::vector<cv::detail::CameraParams> cameras;
    {
      cv::detail::HomographyBasedEstimator estimator;
      if (!estimator(features, pairwise, cameras)) return fail("Panorama: camera estimation failed");
      for (auto& c : cameras) {
        cv::Mat R;
        c.R.convertTo(R, CV_32F);
        c.R = R;
      }
      cv::detail::BundleAdjusterRay adjuster;
      adjuster.setConfThresh(1.0);
      if (!adjuster(features, pairwise, cameras)) return fail("Panorama: bundle adjustment failed");
    }
    std::vector<double> focals;
    for (const auto& c : cameras) focals.push_back(c.focal);
    std::sort(focals.begin(), focals.end());
    // The spherical/cylindrical scale must be a single focal; use the median (as OpenCV's Stitcher does).
    const double warpedScaleReg = focals.size() % 2 ? focals[focals.size() / 2]
                                                      : 0.5 * (focals[focals.size() / 2 - 1] + focals[focals.size() / 2]);

    // reference frame / wave correction
    {
      std::vector<cv::Mat> rmats;
      for (const auto& c : cameras) rmats.push_back(c.R.clone());
      int baseLocal = -1;
      if (opt.baseIndex >= 0) {
        for (int j = 0; j < m; ++j)
          if (keep[j] == opt.baseIndex) baseLocal = j;
        if (baseLocal < 0) rep.warnings.push_back("base frame was not part of the stitched set; using automatic centre");
      }
      if (baseLocal >= 0) {
        const cv::Mat Rb = rmats[baseLocal].t();
        for (auto& r : rmats) r = r * Rb;
      } else {
        // pan direction: spread of optical axes along x (yaw) versus y (pitch)
        double sx = 0, sy = 0, mx = 0, my = 0;
        for (const auto& r : rmats) {
          cv::Mat rr;
          r.convertTo(rr, CV_64F);
          mx += rr.at<double>(2, 0);  // x-component of the optical axis (row 2 of R^T... column of R^T z)
          my += rr.at<double>(2, 1);
        }
        mx /= m;
        my /= m;
        for (const auto& r : rmats) {
          cv::Mat rr;
          r.convertTo(rr, CV_64F);
          sx += std::pow(rr.at<double>(2, 0) - mx, 2);
          sy += std::pow(rr.at<double>(2, 1) - my, 2);
        }
        // Frames are upright (decoders apply the EXIF / RAW orientation), so the result must keep the cameras'
        // "up". OpenCV's waveCorrect infers up as the plane normal of the cameras' x axes, which is arbitrary for
        // two frames (or a short, nearly straight pan) and can turn a portrait panorama by 90 degrees. Keep its
        // result only when it agrees with the cameras' own up; otherwise level to the mean camera up.
        std::vector<cv::Mat> corrected;
        for (const auto& r : rmats) corrected.push_back(r.clone());
        if (m >= 3) cv::detail::waveCorrect(corrected, sx >= sy ? cv::detail::WAVE_CORRECT_HORIZ : cv::detail::WAVE_CORRECT_VERT);
        auto meanUpAlongWorldY = [](const std::vector<cv::Mat>& rs) {
          cv::Vec3d y(0, 0, 0);
          for (const auto& r : rs) {
            cv::Mat rr;
            r.convertTo(rr, CV_64F);
            y += cv::Vec3d(rr.at<double>(0, 1), rr.at<double>(1, 1), rr.at<double>(2, 1));
          }
          const double len = cv::norm(y);
          return len > 0 ? y[1] / len : 0.0;  // cos(angle between the cameras' mean y axis and world y)
        };
        if (m >= 3 && meanUpAlongWorldY(corrected) > 0.87) {
          rmats = corrected;
        } else {
          cv::Vec3d ay(0, 0, 0), az(0, 0, 0);
          for (const auto& r : rmats) {
            cv::Mat rr;
            r.convertTo(rr, CV_64F);
            ay += cv::Vec3d(rr.at<double>(0, 1), rr.at<double>(1, 1), rr.at<double>(2, 1));
            az += cv::Vec3d(rr.at<double>(0, 2), rr.at<double>(1, 2), rr.at<double>(2, 2));
          }
          cv::Vec3d gy = ay * (1.0 / cv::norm(ay));
          cv::Vec3d gz = az - gy * gy.dot(az);
          gz = gz * (1.0 / cv::norm(gz));
          cv::Vec3d gx = gy.cross(gz);
          cv::Mat W = (cv::Mat_<double>(3, 3) << gx[0], gx[1], gx[2], gy[0], gy[1], gy[2], gz[0], gz[1], gz[2]);
          for (auto& r : rmats) {
            cv::Mat rr, out;
            r.convertTo(rr, CV_64F);
            cv::Mat(W * rr).convertTo(out, CV_32F);
            r = out;
          }
        }
      }
      for (int j = 0; j < m; ++j) cameras[j].R = rmats[j];
    }
    rep.secEstimate = secondsSince(t1);

    // report
    {
      cv::Mat R0;
      cameras[0].R.convertTo(R0, CV_64F);
      for (int j = 0; j < m; ++j) {
        PanoFrameReport fr;
        fr.path = "frame" + std::to_string(keep[j]);
        fr.focalPx = cameras[j].focal;
        cv::Mat Rj, rel;
        cameras[j].R.convertTo(Rj, CV_64F);
        rel = Rj * R0.t();
        rotationAngles(rel, fr.yawDeg, fr.pitchDeg, fr.rollDeg);
        for (const auto& mi : pairwise)
          if (mi.src_img_idx == j || mi.dst_img_idx == j) fr.inliers = std::max(fr.inliers, mi.num_inliers);
        rep.frames.push_back(fr);
      }
    }

    // ---------------- geometry of the canvas ----------------
    auto t2 = clk::now();
    ctl.report(0.4f, "warp");
    // Scale relationships. "native" = full-resolution input pixels. Registration pixels are
    // regScale[keep[j]] * native (identical for all frames only if sizes match; we use per-frame factors).
    std::vector<double> sizeFactor(m);  // native -> registration
    for (int j = 0; j < m; ++j) sizeFactor[j] = regScale[keep[j]];
    const double nativeFocal = warpedScaleReg / sizeFactor[0];  // focal in native pixels of frame 0's scale
    auto creator = makeWarper(opt.projection);

    // Canvas at native resolution (for output-size planning).
    auto cameraK = [&](int j, double toNative /*registration -> target scale*/) {
      cv::Mat K;
      cameras[j].K().convertTo(K, CV_32F);
      K.at<float>(0, 0) *= float(toNative);
      K.at<float>(0, 2) *= float(toNative);
      K.at<float>(1, 1) *= float(toNative);
      K.at<float>(1, 2) *= float(toNative);
      return K;
    };
    auto canvasAt = [&](double scalePerNative, std::vector<cv::Point>& corners, std::vector<cv::Size>& sizes) {
      // scalePerNative: pixels of the target image per native pixel
      auto warper = creator->create(float(nativeFocal * scalePerNative));
      corners.resize(m);
      sizes.resize(m);
      for (int j = 0; j < m; ++j) {
        const SourceImage& f = frames[keep[j]];
        cv::Mat K = cameraK(j, scalePerNative / sizeFactor[j]);
        cv::Mat R;
        cameras[j].R.convertTo(R, CV_32F);
        const cv::Rect roi = warper->warpRoi(cv::Size(int(std::lround(f.linear.width * scalePerNative)),
                                                     int(std::lround(f.linear.height * scalePerNative))),
                                            K, R);
        corners[j] = roi.tl();
        sizes[j] = roi.size();
      }
      return cv::detail::resultRoi(corners, sizes);
    };

    std::vector<cv::Point> cornersNative;
    std::vector<cv::Size> sizesNative;
    const cv::Rect nativeRoi = canvasAt(1.0, cornersNative, sizesNative);
    const double nativeArea = double(nativeRoi.width) * nativeRoi.height;
    if (nativeRoi.width <= 0 || nativeRoi.height <= 0 || nativeArea > 3.0e9)
      return fail("Panorama: this projection cannot represent the field of view of these frames; try a different projection");
    // output scale relative to native
    double outScale = 1.0;
    if (opt.longEdge > 0) outScale = std::min(1.0, double(opt.longEdge) / std::max(nativeRoi.width, nativeRoi.height));
    constexpr double kMaxCanvasPixels = 160e6;  // memory guard: 160 MP canvas ~ 6 GB of blender state
    if (nativeArea * outScale * outScale > kMaxCanvasPixels) {
      outScale = std::sqrt(kMaxCanvasPixels / nativeArea);
      rep.warnings.push_back("canvas limited to 160 MP to stay within memory");
    }

    // ---------------- seams at low resolution ----------------
    ctl.report(0.45f, "seams");
    std::vector<cv::Mat> seamMasks(m);
    std::vector<cv::Point> seamCorners(m);
    std::vector<double> gains(m, 1.0);
    {
      const double seamPixels = 0.12e6;
      const double nativeMp = double(frames[keep[0]].linear.width) * frames[keep[0]].linear.height;
      const double seamScale = std::min(1.0, std::sqrt(seamPixels / nativeMp));  // per native pixel
      auto warper = creator->create(float(nativeFocal * seamScale));
      std::vector<cv::UMat> imgsW(m), imgsWf(m), masksW(m);
      std::vector<cv::Point> corners(m);
      for (int j = 0; j < m; ++j) {
        if (ctl.cancelled()) return fail("cancelled");
        const SourceImage& f = frames[keep[j]];
        cv::Mat small8 = gamma8(f.linear, seamScale);
        cv::Mat K = cameraK(j, seamScale / sizeFactor[j]);
        cv::Mat R;
        cameras[j].R.convertTo(R, CV_32F);
        cv::Mat mask(small8.size(), CV_8U, cv::Scalar(255));
        cv::Mat wi, wm;
        corners[j] = warper->warp(small8, K, R, cv::INTER_LINEAR, cv::BORDER_REFLECT, wi);
        warper->warp(mask, K, R, cv::INTER_NEAREST, cv::BORDER_CONSTANT, wm);
        wi.copyTo(imgsW[j]);
        wm.copyTo(masksW[j]);
      }
      if (opt.exposureCompensation) {
        cv::detail::GainCompensator comp;
        std::vector<std::pair<cv::UMat, uchar>> pm;
        for (int j = 0; j < m; ++j) pm.emplace_back(masksW[j], uchar(255));
        comp.feed(corners, imgsW, pm);
        std::vector<double> g = comp.gains();
        if (int(g.size()) == m)
          for (int j = 0; j < m; ++j) gains[j] = std::pow(std::max(0.05, g[j]), 2.2);  // gamma-domain gain -> linear
      }
      for (int j = 0; j < m; ++j) imgsW[j].convertTo(imgsWf[j], CV_32F);
      cv::detail::GraphCutSeamFinder seamFinder(cv::detail::GraphCutSeamFinderBase::COST_COLOR);
      seamFinder.find(imgsWf, corners, masksW);
      for (int j = 0; j < m; ++j) {
        masksW[j].copyTo(seamMasks[j]);
        seamCorners[j] = corners[j];
      }
    }
    if (ctl.cancelled()) return fail("cancelled");

    // ---------------- compose at output resolution ----------------
    ctl.report(0.55f, "blend");
    std::vector<cv::Point> corners;
    std::vector<cv::Size> sizes;
    const cv::Rect dstRoi = canvasAt(outScale, corners, sizes);
    if (dstRoi.width <= 0 || dstRoi.height <= 0) return fail("Panorama: empty canvas");
    rep.canvasWidth = dstRoi.width;
    rep.canvasHeight = dstRoi.height;

    cv::Ptr<cv::detail::Blender> blender = cv::detail::Blender::createDefault(cv::detail::Blender::MULTI_BAND, false);
    {
      const float blendWidth = float(std::sqrt(double(dstRoi.width) * dstRoi.height) * 5.0 / 100.0);
      auto* mb = dynamic_cast<cv::detail::MultiBandBlender*>(blender.get());
      mb->setNumBands(std::max(1, int(std::ceil(std::log(std::max(2.f, blendWidth)) / std::log(2.0)) - 1.0)));
    }
    blender->prepare(dstRoi);
    auto warperOut = creator->create(float(nativeFocal * outScale));
    for (int j = 0; j < m; ++j) {
      if (ctl.cancelled()) return fail("cancelled");
      const SourceImage& f = frames[keep[j]];
      ImageF scaled;
      const ImageF* src = &f.linear;
      if (outScale < 0.999) {
        scaled = resizeToLongEdge(f.linear, std::max(1, int(std::lround(std::max(f.linear.width, f.linear.height) * outScale))));
        src = &scaled;
      }
      cv::Mat K = cameraK(j, double(src->width) / (f.linear.width * sizeFactor[j]));
      cv::Mat R;
      cameras[j].R.convertTo(R, CV_32F);
      cv::Mat warped, wmask;
      cv::Mat inMask(src->height, src->width, CV_8U, cv::Scalar(255));
      const cv::Point corner = warperOut->warp(asMat(*src), K, R, cv::INTER_LINEAR, cv::BORDER_REFLECT, warped);
      warperOut->warp(inMask, K, R, cv::INTER_NEAREST, cv::BORDER_CONSTANT, wmask);
      // seam mask (low-res) -> output resolution
      cv::Mat dil, seamUp;
      cv::dilate(seamMasks[j], dil, cv::Mat());
      cv::resize(dil, seamUp, wmask.size(), 0, 0, cv::INTER_LINEAR_EXACT);
      wmask = seamUp & wmask;
      // gain, encode to the blender's 16-bit integer range
      cv::Mat w16(warped.size(), CV_16SC3);
      const float g = float(gains[j]);
      parallelRows(warped.rows, [&](int y) {
        const float* s = warped.ptr<float>(y);
        short* d = w16.ptr<short>(y);
        for (int i = 0; i < warped.cols * 3; ++i) d[i] = encodeBlend(s[i] * g);
      });
      blender->feed(w16, wmask, corner);
      ctl.report(0.55f + 0.3f * (j + 1) / m, "blend");
    }
    cv::Mat res16, resMask;
    blender->blend(res16, resMask);
    ctl.report(0.9f, "crop");

    // ---------------- decode, crop / fill ----------------
    cv::Mat valid = resMask > 0;  // 255 where covered
    cv::Rect cropRect(0, 0, res16.cols, res16.rows);
    if (opt.crop) {
      const int longEdge = std::max(res16.cols, res16.rows);
      const int d = std::max(1, longEdge / 1400);
      cv::Mat small;
      cv::resize(valid, small, cv::Size(std::max(1, res16.cols / d), std::max(1, res16.rows / d)), 0, 0, cv::INTER_AREA);
      cv::Mat full = small == 255;
      cv::Rect r = largestRect(full);
      if (r.width < 8 || r.height < 8) {
        rep.warnings.push_back("could not find a valid crop rectangle; output is uncropped");
      } else {
        cropRect = cv::Rect(r.x * d, r.y * d, std::min(res16.cols - r.x * d, r.width * d),
                            std::min(res16.rows - r.y * d, r.height * d));
        // The rectangle consists of fully valid cells; shave one pixel against interpolation at its border.
        cropRect.x += 1;
        cropRect.y += 1;
        cropRect.width = std::max(1, cropRect.width - 2);
        cropRect.height = std::max(1, cropRect.height - 2);
      }
    }
    rep.cropX = cropRect.x;
    rep.cropY = cropRect.y;
    ImageF out(cropRect.width, cropRect.height);
    parallelRows(cropRect.height, [&](int y) {
      const short* s = res16.ptr<short>(y + cropRect.y) + 3 * cropRect.x;
      const std::uint8_t* v = valid.ptr<std::uint8_t>(y + cropRect.y) + cropRect.x;
      float* d = out.row(y);
      for (int x = 0; x < cropRect.width; ++x)
        for (int c = 0; c < 3; ++c) d[3 * x + c] = v[x] ? decodeBlend(s[3 * x + c]) : 0.f;
    });
    {
      cv::Mat vcrop = valid(cropRect);
      rep.validFraction = double(cv::countNonZero(vcrop)) / (double(cropRect.width) * cropRect.height);
      if (!opt.crop && opt.fill && rep.validFraction < 0.999) {
        // Telea inpaint on a reduced-resolution gamma copy, upsampled into the holes only.
        const double sc = std::min(1.0, 1600.0 / std::max(out.width, out.height));
        cv::Mat small8 = gamma8(out, sc), smallValid, hole, inp;
        cv::resize(vcrop, smallValid, small8.size(), 0, 0, cv::INTER_AREA);
        hole = smallValid < 250;
        cv::dilate(hole, hole, cv::Mat(), cv::Point(-1, -1), 2);
        cv::inpaint(small8, hole, inp, 5, cv::INPAINT_TELEA);
        cv::Mat up;
        cv::resize(inp, up, cv::Size(out.width, out.height), 0, 0, cv::INTER_LINEAR);
        parallelRows(out.height, [&](int y) {
          const std::uint8_t* v = vcrop.ptr<std::uint8_t>(y);
          const std::uint8_t* u = up.ptr<std::uint8_t>(y);
          float* d = out.row(y);
          for (int x = 0; x < out.width; ++x)
            if (!v[x])
              for (int c = 0; c < 3; ++c) d[3 * x + c] = std::pow(u[3 * x + c] / 255.f, 2.2f);
        });
        rep.warnings.push_back("edges filled by inpainting (approximate content)");
      }
    }
    rep.secCompose = secondsSince(t2);

    result.image.linear = std::move(out);
    result.image.meta = frames[keep[0]].meta;
    result.image.meta.sensorWidth = result.image.linear.width;
    result.image.meta.sensorHeight = result.image.linear.height;
    result.image.sceneReferred = frames[keep[0]].sceneReferred;
    ctl.report(1.f, "done");
    return result;
  } catch (const cv::Exception& e) {
    return fail(std::string("Panorama failed: ") + e.what());
  } catch (const std::exception& e) {
    return fail(std::string("Panorama failed: ") + e.what());
  }
}

#endif  // DAPPLE_HAVE_OPENCV

Result<PanoResult> stitchPanorama(const std::vector<std::filesystem::path>& files, const PanoOptions& options,
                                  const MergeControl& control) {
  try {
    if (files.size() < 2 || files.size() > 20) return fail("Panorama needs 2 to 20 frames");
    static const DecoderRegistry registry = DecoderRegistry::withBuiltins();
    std::vector<SourceImage> frames;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (control.cancelled()) return fail("cancelled");
      const ImageDecoder* dec = registry.find(files[i]);
      if (!dec) return fail("Unsupported file type: " + pathToUtf8(files[i]));
      auto src = dec->decode(files[i], DecodeOptions{options.quality});
      if (!src) return fail(pathToUtf8(files[i]) + ": " + src.error().message);
      frames.push_back(std::move(*src));
      control.report(0.02f * float(i + 1) / files.size(), "load");
    }
    const double loadSec = secondsSince(t0);
    auto r = stitchPanoramaImages(std::move(frames), options, control);
    if (r) {
      r->report.secLoad = loadSec;
      for (std::size_t j = 0; j < r->report.frames.size() && j < r->report.usedIndices.size(); ++j)
        r->report.frames[j].path = pathToUtf8(files[r->report.usedIndices[j]]);
    }
    return r;
  } catch (const std::exception& e) {
    return fail(std::string("Panorama failed: ") + e.what());
  }
}

}  // namespace dapple
