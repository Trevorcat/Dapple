// HDR bracket merge in linear light.
//
// RAW frames are linear, so no camera response curve is needed: every frame is
// normalised by its exposure ratio to the base frame (EXIF, refined from the
// pixels), aligned to the base, and averaged with weights that favour pixels
// that are neither clipped nor noise-floor limited (inverse-variance weights
// with a smooth highlight gate). Moving content is detected against the base
// frame and replaced by base-frame pixels (deghost). JPEG / display-referred
// inputs use Mertens exposure fusion instead.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "dapple/merge.h"
#include "file_util.h"
#include "merge_internal.h"

#ifdef DAPPLE_HAVE_OPENCV
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/video/tracking.hpp>
#endif

namespace dapple {

using namespace mergeutil;

const char* toString(Deghost d) {
  switch (d) {
    case Deghost::Off: return "off";
    case Deghost::Low: return "low";
    case Deghost::Mid: return "mid";
    case Deghost::High: return "high";
  }
  return "mid";
}

Deghost deghostFromString(const std::string& s, Deghost fallback) {
  if (s == "off") return Deghost::Off;
  if (s == "low") return Deghost::Low;
  if (s == "mid" || s == "medium") return Deghost::Mid;
  if (s == "high") return Deghost::High;
  return fallback;
}

bool mergeAvailable() {
#ifdef DAPPLE_HAVE_OPENCV
  return true;
#else
  return false;
#endif
}

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Noise model in normalised sensor units (1.0 = clipping): var = gain * (kReadRel + v) * isoScale.
constexpr float kGain = 2e-5f;
constexpr float kReadRel = 1e-3f;

struct ExifExposure {
  bool known = false;
  double stops = 0;
};

ExifExposure exifExposure(const ShotMetadata& m) {
  ExifExposure e;
  if (m.shutterSeconds > 0 && m.iso > 0 && m.aperture > 0) {
    e.known = true;
    e.stops = std::log2(double(m.shutterSeconds) * m.iso / (double(m.aperture) * m.aperture));
  }
  return e;
}

float lumaOf(const float* p) { return color::proPhotoLuma(p); }

// Median of luminance ratio frame/base over mid-tone pixels that are valid in both.
bool estimateRatio(const ImageF& base, const ImageF& frame, const std::vector<std::uint8_t>* valid, double& ratio,
                   std::size_t& samples) {
  std::vector<float> r;
  const int step = std::max(1, int(std::sqrt(double(base.width) * base.height / 400000.0)));
  for (int y = step / 2; y < base.height; y += step) {
    const float* b = base.row(y);
    const float* f = frame.row(y);
    for (int x = step / 2; x < base.width; x += step) {
      if (valid && !(*valid)[std::size_t(y) * base.width + x]) continue;
      const float mb = std::max({b[3 * x], b[3 * x + 1], b[3 * x + 2]});
      const float mf = std::max({f[3 * x], f[3 * x + 1], f[3 * x + 2]});
      if (mb < 0.02f || mb > 0.85f || mf < 0.02f || mf > 0.85f) continue;
      const float yb = lumaOf(b + 3 * x), yf = lumaOf(f + 3 * x);
      if (yb < 0.004f || yf < 0.004f) continue;
      r.push_back(yf / yb);
    }
  }
  samples = r.size();
  if (r.size() < 1500) return false;
  std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
  ratio = r[r.size() / 2];
  return ratio > 0;
}

double medianLuma(const ImageF& im) {
  std::vector<float> v;
  const int step = std::max(1, int(std::sqrt(double(im.width) * im.height / 100000.0)));
  for (int y = step / 2; y < im.height; y += step)
    for (int x = step / 2; x < im.width; x += step) {
      const float* p = im.row(y) + 3 * x;
      if (std::max({p[0], p[1], p[2]}) < 0.95f) v.push_back(lumaOf(p));
    }
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

// ---------------------------------------------------------------------------
// Alignment (OpenCV)
// ---------------------------------------------------------------------------
#ifdef DAPPLE_HAVE_OPENCV

struct AlignOut {
  bool ok = false;
  cv::Mat H;  // 3x3 CV_64F, full resolution, base -> frame
  int method = 0;
  double score = 0;
  std::string why;
};

struct RegImage {
  cv::Mat gray32;  // gamma-encoded, exposure-normalised luma in [0,1]
  cv::Mat gray8;
  cv::Mat valid8;  // 255 where neither clipped nor noise-floor limited
  double sx = 1, sy = 1;
};

RegImage makeReg(const ImageF& im, int regLong, double gain) {
  RegImage r;
  const double s = std::min(1.0, double(regLong) / std::max(im.width, im.height));
  const int rw = std::max(16, int(std::lround(im.width * s))), rh = std::max(16, int(std::lround(im.height * s)));
  r.sx = double(rw) / im.width;
  r.sy = double(rh) / im.height;
  cv::Mat small;
  cv::resize(asMat(im), small, cv::Size(rw, rh), 0, 0, cv::INTER_AREA);
  r.gray32.create(rh, rw, CV_32F);
  r.gray8.create(rh, rw, CV_8U);
  r.valid8.create(rh, rw, CV_8U);
  for (int y = 0; y < rh; ++y) {
    const float* p = small.ptr<float>(y);
    float* g = r.gray32.ptr<float>(y);
    std::uint8_t* g8 = r.gray8.ptr<std::uint8_t>(y);
    std::uint8_t* v8 = r.valid8.ptr<std::uint8_t>(y);
    for (int x = 0; x < rw; ++x) {
      const float* q = p + 3 * x;
      const float mx = std::max({q[0], q[1], q[2]});
      const float yl = lumaOf(q);
      const float n = std::clamp(float(yl * gain), 0.f, 1.f);
      const float e = std::pow(n, 1.f / 2.2f);
      g[x] = e;
      g8[x] = std::uint8_t(e * 255.f + 0.5f);
      v8[x] = (mx < 0.9f && yl > 0.003f && yl * gain < 0.97) ? 255 : 0;
    }
  }
  return r;
}

cv::Matx33d scaleMat(const RegImage& r) {
  return cv::Matx33d(r.sx, 0, 0.5 * r.sx - 0.5, 0, r.sy, 0.5 * r.sy - 0.5, 0, 0, 1);
}

double cornerShift(const cv::Matx33d& H, int w, int h) {
  const cv::Point2d c[4] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
  double sum = 0;
  for (const auto& p : c) {
    const double X = H(0, 0) * p.x + H(0, 1) * p.y + H(0, 2);
    const double Y = H(1, 0) * p.x + H(1, 1) * p.y + H(1, 2);
    const double Z = H(2, 0) * p.x + H(2, 1) * p.y + H(2, 2);
    sum += std::hypot(X / Z - p.x, Y / Z - p.y);
  }
  return sum / 4;
}

bool sane(const cv::Matx33d& H, int w, int h) {
  if (!std::isfinite(H(0, 0)) || std::fabs(H(2, 2)) < 1e-9) return false;
  const cv::Matx33d N = H * (1.0 / H(2, 2));
  if (std::fabs(N(0, 0) - 1) > 0.25 || std::fabs(N(1, 1) - 1) > 0.25) return false;
  if (std::fabs(N(2, 0)) * w > 0.4 || std::fabs(N(2, 1)) * h > 0.4) return false;
  return cornerShift(N, w, h) < 0.25 * std::max(w, h);
}

// Zero-mean normalised cross correlation over pixels valid in both images.
double nccScore(const cv::Mat& a32, const cv::Mat& b32, const cv::Mat& validA, const cv::Mat& H, int rw, int rh) {
  cv::Mat bw, vw;
  cv::warpPerspective(b32, bw, H, cv::Size(rw, rh), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, 0);
  cv::warpPerspective(validA, vw, H, cv::Size(rw, rh), cv::INTER_NEAREST | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, 0);
  cv::Mat inFrame = cv::Mat::ones(rh, rw, CV_8U) * 255, fw;
  cv::warpPerspective(inFrame, fw, H, cv::Size(rw, rh), cv::INTER_NEAREST | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, 0);
  cv::Mat mask = fw & validA;
  const cv::Scalar ma = cv::mean(a32, mask), mb = cv::mean(bw, mask);
  cv::Mat da = a32 - ma[0], db = bw - mb[0];
  const double num = da.dot(db) - 0;  // restricted below
  (void)num;
  cv::Mat dam, dbm;
  da.copyTo(dam, mask);
  db.copyTo(dbm, mask);
  const double d = std::sqrt(dam.dot(dam) * dbm.dot(dbm));
  return d > 1e-9 ? dam.dot(dbm) / d : 0;
}

bool eccRefine(const RegImage& base, const RegImage& frame, const cv::Mat& maskIn, cv::Mat& H, bool coarseLevels,
               double& cc) {
  const cv::Mat tmpl = base.gray32, input = frame.gray32;
  std::vector<double> scales = coarseLevels ? std::vector<double>{0.25, 0.5, 1.0} : std::vector<double>{1.0};
  cv::Mat Hcur = H.clone();
  for (std::size_t i = 0; i < scales.size(); ++i) {
    const double s = scales[i];
    cv::Mat t, f, m;
    if (s < 1.0) {
      cv::resize(tmpl, t, cv::Size(), s, s, cv::INTER_AREA);
      cv::resize(input, f, cv::Size(), s, s, cv::INTER_AREA);
      cv::resize(maskIn, m, cv::Size(), s, s, cv::INTER_NEAREST);
      cv::erode(m, m, cv::Mat());
    } else {
      t = tmpl;
      f = input;
      m = maskIn;
    }
    cv::Matx33d S(s, 0, 0, 0, s, 0, 0, 0, 1);
    cv::Matx33d Hc = cv::Matx33d(Hcur) ;
    cv::Matx33d Hl = S * Hc * S.inv();
    cv::Mat Hf;
    cv::Mat(Hl).convertTo(Hf, CV_32F);
    try {
      const cv::TermCriteria crit(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, s < 1.0 ? 60 : 40, 1e-5);
      if (std::getenv("DAPPLE_ECC_AFFINE")) {
        cv::Mat A = Hf(cv::Rect(0, 0, 3, 2)).clone();
        cc = cv::findTransformECC(t, f, A, cv::MOTION_AFFINE, crit, m, 5);
        Hf = cv::Mat::eye(3, 3, CV_32F);
        A.copyTo(Hf(cv::Rect(0, 0, 3, 2)));
      } else {
        cc = cv::findTransformECC(t, f, Hf, cv::MOTION_HOMOGRAPHY, crit, m, 5);
      }
    } catch (const cv::Exception&) {
      return false;
    }
    cv::Mat Hd;
    Hf.convertTo(Hd, CV_64F);
    cv::Matx33d back = S.inv() * cv::Matx33d(Hd) * S;
    Hcur = cv::Mat(back).clone();
    if (std::fabs(back(2, 2)) > 1e-12) Hcur = Hcur / back(2, 2);
  }
  H = Hcur;
  return true;
}

// Registration pyramids of the base frame, shared by all frames of one merge.
struct BaseReg {
  bool ready = false;
  RegImage feat, ecc;
  bool sameLevel = false;
};

AlignOut alignFrame(const ImageF& base, const ImageF& frame, double kRel, BaseReg& cache) {
  AlignOut out;
  const int featLong = 1600;  // keypoints are found at ~1.6k px
  const int eccLong = 3000;   // the correlation refinement runs at up to ~3k px (sub-pixel accuracy at full resolution)
  if (!cache.ready) {
    cache.feat = makeReg(base, featLong, 1.0);
    cache.sameLevel = std::max(base.width, base.height) <= featLong;
    cache.ecc = cache.sameLevel ? cache.feat : makeReg(base, eccLong, 1.0);
    cache.ready = true;
  }
  const RegImage& rb = cache.feat;
  const RegImage rf = makeReg(frame, featLong, 1.0 / kRel);
  const int rw = rb.gray8.cols, rh = rb.gray8.rows;

  // 1) features
  cv::Matx33d Hreg = cv::Matx33d::eye();
  bool haveFeat = false;
  int inliers = 0;
  try {
    auto akaze = cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB, 0, 3, 0.0006f, 4, 4);
    std::vector<cv::KeyPoint> kb, kf;
    cv::Mat db, df;
    akaze->detectAndCompute(rb.gray8, cv::noArray(), kb, db);
    akaze->detectAndCompute(rf.gray8, cv::noArray(), kf, df);
    if (kb.size() >= 20 && kf.size() >= 20) {
      cv::BFMatcher matcher(cv::NORM_HAMMING);
      std::vector<std::vector<cv::DMatch>> knn;
      matcher.knnMatch(db, df, knn, 2);
      std::vector<cv::Point2f> pb, pf;
      for (const auto& m : knn) {
        if (m.size() == 2 && m[0].distance < 0.8f * m[1].distance) {
          pb.push_back(kb[m[0].queryIdx].pt);
          pf.push_back(kf[m[0].trainIdx].pt);
        }
      }
      if (pb.size() >= 15) {
        std::vector<std::uint8_t> inl;
        cv::Mat Hm = cv::findHomography(pb, pf, cv::RANSAC, 2.5, inl, 3000, 0.995);
        if (!Hm.empty()) {
          inliers = cv::countNonZero(inl);
          cv::Matx33d Hc(Hm);
          if (inliers >= 15 && inliers >= 0.2 * double(pb.size()) && sane(Hc, rw, rh)) {
            Hreg = Hc;
            haveFeat = true;
          }
        }
      }
    }
  } catch (const cv::Exception& e) {
    out.why = std::string("feature alignment failed: ") + e.what();
  }

  // Feature homography in full-resolution coordinates -> ECC level.
  const cv::Matx33d SbF = scaleMat(rb), SfF = scaleMat(rf);
  const cv::Matx33d HfullFeat = SfF.inv() * Hreg * SbF;
  const RegImage& eb = cache.ecc;
  const RegImage ef = cache.sameLevel ? rf : makeReg(frame, eccLong, 1.0 / kRel);
  const cv::Matx33d SbE = scaleMat(eb), SfE = scaleMat(ef);
  const int ew = eb.gray8.cols, eh = eb.gray8.rows;
  cv::Mat H0 = cv::Mat(SfE * HfullFeat * SbE.inv()).clone();

  // 2) ECC refinement; mask = frame valid AND base valid (mapped into the frame)
  cv::Mat baseValidInFrame;
  cv::warpPerspective(eb.valid8, baseValidInFrame, H0, cv::Size(ew, eh), cv::INTER_NEAREST, cv::BORDER_CONSTANT, 0);
  cv::Mat mask = ef.valid8 & baseValidInFrame;
  cv::erode(mask, mask, cv::Mat(), cv::Point(-1, -1), 2);
  cv::Mat Hecc = H0.clone();
  double cc = 0;
  bool eccOk = false;
  if (cv::countNonZero(mask) > 0.05 * ew * eh) eccOk = eccRefine(eb, ef, mask, Hecc, !haveFeat, cc);
  if (eccOk && !sane(cv::Matx33d(Hecc), ew, eh)) eccOk = false;

  cv::Mat Hbest;
  double scoreFeat = 0, scoreEcc = 0;
  if (haveFeat) scoreFeat = nccScore(eb.gray32, ef.gray32, eb.valid8, H0, ew, eh);
  if (eccOk) scoreEcc = nccScore(eb.gray32, ef.gray32, eb.valid8, Hecc, ew, eh);
  if (eccOk && (!haveFeat || scoreEcc >= scoreFeat - 0.005)) {
    Hbest = Hecc;
    out.method = haveFeat ? 3 : 1;
    out.score = scoreEcc;
  } else if (haveFeat) {
    Hbest = H0;
    out.method = 2;
    out.score = scoreFeat;
  } else {
    out.why = out.why.empty() ? "no reliable features or correlation" : out.why;
    return out;
  }
  if (out.score < 0.3) {
    out.why = "alignment correlation too low (" + std::to_string(out.score) + ")";
    return out;
  }
  const cv::Matx33d Hfull = SfE.inv() * cv::Matx33d(Hbest) * SbE;
  out.H = cv::Mat(Hfull).clone();
  out.ok = true;
  return out;
}

// Warp `frame` into the base geometry. valid[i] = 1 where the source covered the pixel.
void warpToBase(const ImageF& frame, const cv::Mat& H, ImageF& dst, std::vector<std::uint8_t>& valid) {
  const int w = frame.width, h = frame.height;
  dst = ImageF(w, h);
  cv::Mat dstView = asMat(dst);
  cv::warpPerspective(asMat(frame), dstView, H, cv::Size(w, h), cv::INTER_LANCZOS4 | cv::WARP_INVERSE_MAP,
                      cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
  cv::Mat ones = cv::Mat::ones(h, w, CV_8U) * 255, vm;
  cv::warpPerspective(ones, vm, H, cv::Size(w, h), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, 0);
  cv::erode(vm, vm, cv::Mat(), cv::Point(-1, -1), 2);
  valid.assign(std::size_t(w) * h, 0);
  parallelRows(h, [&](int y) {
    const std::uint8_t* v = vm.ptr<std::uint8_t>(y);
    std::uint8_t* d = valid.data() + std::size_t(y) * w;
    float* px = dst.row(y);
    for (int x = 0; x < w; ++x) {
      d[x] = v[x] == 255;
      for (int c = 0; c < 3; ++c) px[3 * x + c] = std::max(0.f, px[3 * x + c]);
    }
  });
}

#endif  // DAPPLE_HAVE_OPENCV

// ---------------------------------------------------------------------------
// Ghost detection at reduced resolution
// ---------------------------------------------------------------------------
struct GhostParams {
  float nsig, tolRel, tolGrad;
  int dilate;
};

GhostParams ghostParams(Deghost d) {
  switch (d) {
    case Deghost::Low: return {6.f, 0.25f, 1.5f, 1};
    case Deghost::Mid: return {4.5f, 0.12f, 1.2f, 2};
    case Deghost::High: return {3.5f, 0.07f, 1.0f, 3};
    default: return {0, 0, 0, 0};
  }
}

// Low-resolution analysis planes of a frame: mean luma (divided by k) and max-channel peak.
struct Analysis {
  int w = 0, h = 0, f = 1;
  std::vector<float> luma, peak;
};

Analysis analyse(const ImageF& im, int f, double k) {
  Analysis a;
  a.f = f;
  a.w = (im.width + f - 1) / f;
  a.h = (im.height + f - 1) / f;
  a.luma.assign(std::size_t(a.w) * a.h, 0.f);
  a.peak.assign(std::size_t(a.w) * a.h, 0.f);
  parallelRows(a.h, [&](int by) {
    for (int bx = 0; bx < a.w; ++bx) {
      double sum = 0;
      float pk = 0;
      int n = 0;
      for (int y = by * f; y < std::min(im.height, (by + 1) * f); ++y) {
        const float* row = im.row(y);
        for (int x = bx * f; x < std::min(im.width, (bx + 1) * f); ++x) {
          sum += lumaOf(row + 3 * x);
          pk = std::max(pk, std::max({row[3 * x], row[3 * x + 1], row[3 * x + 2]}));
          ++n;
        }
      }
      a.luma[std::size_t(by) * a.w + bx] = float(sum / std::max(1, n) / k);
      a.peak[std::size_t(by) * a.w + bx] = pk;
    }
  });
  return a;
}

// Ghost weight in [0,1] per analysis cell: 1 = the frame disagrees with the base here.
std::vector<float> detectGhost(const Analysis& base, const Analysis& fr, double k, float isoScale, const GhostParams& gp,
                               const std::vector<std::uint8_t>* validLow) {
  const int w = base.w, h = base.h;
  std::vector<float> g(std::size_t(w) * h, 0.f);
  const float binScale = 1.f / float(base.f * base.f);
  parallelRows(h, [&](int y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      if (validLow && !(*validLow)[i]) continue;
      if (base.peak[i] > 0.9f || fr.peak[i] > 0.9f) continue;  // clipped: cannot compare
      const float lb = base.luma[i], lf = fr.luma[i];
      // gradient of the base luma (px of misregistration tolerance)
      const float gx = base.luma[y * w + std::min(w - 1, x + 1)] - base.luma[y * w + std::max(0, x - 1)];
      const float gy = base.luma[std::min(h - 1, y + 1) * w + x] - base.luma[std::max(0, y - 1) * w + x];
      const float grad = 0.5f * std::sqrt(gx * gx + gy * gy);
      const float varB = kGain * isoScale * (kReadRel + lb) * binScale;
      const float varF = kGain * isoScale * (kReadRel + lf * float(k)) * binScale / float(k * k);
      const float tol = gp.nsig * std::sqrt(varB + varF) + gp.tolRel * 0.5f * (lb + lf) + gp.tolGrad * grad + 1e-4f;
      const float d = std::fabs(lf - lb) / tol;
      g[i] = smooth01(0.9f, 1.4f, d);
    }
  });
  // dilate (max filter), then soften
  if (gp.dilate > 0) {
    std::vector<float> t(g.size());
    const int r = gp.dilate;
    parallelRows(h, [&](int y) {
      for (int x = 0; x < w; ++x) {
        float m = 0;
        for (int dx = -r; dx <= r; ++dx) m = std::max(m, g[std::size_t(y) * w + std::clamp(x + dx, 0, w - 1)]);
        t[std::size_t(y) * w + x] = m;
      }
    });
    parallelRows(h, [&](int y) {
      for (int x = 0; x < w; ++x) {
        float m = 0;
        for (int dy = -r; dy <= r; ++dy) m = std::max(m, t[std::size_t(std::clamp(y + dy, 0, h - 1)) * w + x]);
        g[std::size_t(y) * w + x] = m;
      }
    });
    boxBlur(g, w, h, 1);
  }
  return g;
}

float sampleLow(const std::vector<float>& plane, int w, int h, int f, int x, int y) {
  const float fx = (x + 0.5f) / f - 0.5f, fy = (y + 0.5f) / f - 0.5f;
  const int x0 = std::clamp(int(std::floor(fx)), 0, w - 1), y0 = std::clamp(int(std::floor(fy)), 0, h - 1);
  const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
  const float tx = std::clamp(fx - x0, 0.f, 1.f), ty = std::clamp(fy - y0, 0.f, 1.f);
  const float a = plane[std::size_t(y0) * w + x0] * (1 - tx) + plane[std::size_t(y0) * w + x1] * tx;
  const float b = plane[std::size_t(y1) * w + x0] * (1 - tx) + plane[std::size_t(y1) * w + x1] * tx;
  return a * (1 - ty) + b * ty;
}

// ---------------------------------------------------------------------------
// Merge engine
// ---------------------------------------------------------------------------
using FrameLoader = std::function<Result<SourceImage>(int index)>;

Result<void> fitToBase(SourceImage& s, int w, int h) {
  if (s.linear.width == w && s.linear.height == h) return {};
  const double ar1 = double(s.linear.width) / s.linear.height, ar2 = double(w) / h;
  if (std::fabs(ar1 / ar2 - 1.0) > 0.01) return fail("Frames have different sizes / aspect ratios");
  ImageF out(w, h);
  // Area resize through resizeToLongEdge keeps exact scale; fall back to nearest for upscales.
  if (s.linear.width > w) {
    ImageF r = resizeToLongEdge(s.linear, std::max(w, h));
    if (r.width == w && r.height == h) {
      s.linear = std::move(r);
      return {};
    }
  }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int sx = std::min(s.linear.width - 1, int((x + 0.5) * s.linear.width / w));
      const int sy = std::min(s.linear.height - 1, int((y + 0.5) * s.linear.height / h));
      std::copy_n(s.linear.row(sy) + 3 * sx, 3, out.row(y) + 3 * x);
    }
  s.linear = std::move(out);
  return {};
}

// A merge must never hand back an empty or non-finite image: NaN/Inf are cleared and an all-zero result is an error.
static Result<void> validateMerged(ImageF& im) {
  float mx = 0.f;
  std::size_t bad = 0;
  for (float& v : im.data) {
    if (!std::isfinite(v)) { v = 0.f; ++bad; }
    else mx = std::max(mx, v);
  }
  if (mx <= 1e-6f) return fail(bad ? "Merge produced invalid (non-finite) pixels" : "Merge produced an empty (black) image");
  return {};
}

Result<HdrResult> mergeEngine(int count, int baseIdx, const std::vector<std::string>& names, const FrameLoader& load,
                              const HdrOptions& opt, const MergeControl& ctl) {
  using clk = std::chrono::steady_clock;
  HdrResult res;
  HdrReport& rep = res.report;
  rep.baseIndex = baseIdx;
  rep.frames.resize(count);
  for (int i = 0; i < count; ++i) rep.frames[i].path = names[i];

  auto t0 = clk::now();
  ctl.report(0.0f, "load");
  auto baseR = load(baseIdx);
  if (!baseR) return std::unexpected(baseR.error());
  SourceImage base = std::move(*baseR);
  if (base.linear.empty()) return fail("Empty base frame");
  const int W = base.linear.width, H = base.linear.height;
  rep.secLoad += secondsSince(t0);

  const bool sceneRef = base.sceneReferred;
  const ExifExposure baseExif = exifExposure(base.meta);
  const int framesTotal = count;
  auto stage = [&](int done, float within, const char* name) {
    ctl.report((done + within) / float(framesTotal + 1), name);
  };

  if (!sceneRef) {
    // ---------------- Mertens exposure fusion for display-referred inputs ----------------
#ifndef DAPPLE_HAVE_OPENCV
    return fail("Exposure fusion of JPEG frames needs OpenCV (this build has none)");
#else
    rep.exposureFusion = true;
    rep.warnings.push_back("display-referred inputs: exposure fusion (no deghost, result is display-referred)");
    std::vector<cv::Mat> stack;
    auto toGamma = [&](const ImageF& im) {
      cv::Mat m(im.height, im.width, CV_32FC3);
      parallelRows(im.height, [&](int y) {
        const float* s = im.row(y);
        float* d = m.ptr<float>(y);
        for (int i = 0; i < im.width * 3; ++i) d[i] = 255.f * std::pow(s[i] > 0.f ? std::min(s[i], 1.f) : 0.f, 1.f / 2.2f);  // OpenCV's Mertens divides every depth by 255
      });
      return m;
    };
    stack.resize(count);
    stack[baseIdx] = toGamma(base.linear);
    ImageF baseKeep = std::move(base.linear);
    SourceImage baseMeta;
    baseMeta.meta = base.meta;
    baseMeta.sceneReferred = false;
    double secAlign = 0;
    for (int i = 0; i < count; ++i) {
      if (i == baseIdx) continue;
      if (ctl.cancelled()) return fail("cancelled");
      stage(i, 0.f, "load");
      auto r = load(i);
      if (!r) return std::unexpected(r.error());
      if (auto f = fitToBase(*r, W, H); !f) return std::unexpected(f.error());
      if (opt.align) {
        auto ta = clk::now();
        stage(i, 0.4f, "align");
        const double kr = std::max(1e-3, medianLuma(r->linear) / std::max(1e-6, medianLuma(baseKeep)));
        BaseReg breg;
        AlignOut a = alignFrame(baseKeep, r->linear, kr, breg);
        if (!a.ok) return fail("Could not align frame " + std::to_string(i) + ": " + a.why);
        ImageF warped;
        std::vector<std::uint8_t> valid;
        warpToBase(r->linear, a.H, warped, valid);
        // fill uncovered border with the base so fusion has no black frame
        parallelRows(H, [&](int y) {
          for (int x = 0; x < W; ++x)
            if (!valid[std::size_t(y) * W + x]) std::copy_n(baseKeep.row(y) + 3 * x, 3, warped.row(y) + 3 * x);
        });
        r->linear = std::move(warped);
        auto& fr = rep.frames[i];
        fr.aligned = true;
        fr.alignMethod = a.method;
        fr.alignScore = a.score;
        fr.shiftPx = cornerShift(cv::Matx33d(a.H), W, H);
        secAlign += secondsSince(ta);
      }
      stack[i] = toGamma(r->linear);
    }
    rep.secAlign = secAlign;
    auto tm = clk::now();
    stage(count - 1, 0.7f, "merge");
    cv::Mat fused;
    cv::createMergeMertens(1.f, 1.f, 1.f)->process(stack, fused);
    stack.clear();
    ImageF out(W, H);
    parallelRows(H, [&](int y) {
      const float* s = fused.ptr<float>(y);
      float* d = out.row(y);
      for (int i = 0; i < W * 3; ++i) d[i] = std::pow(std::max(0.f, s[i]), 2.2f);
    });
    if (auto v = validateMerged(out); !v) return std::unexpected(v.error());
    res.image.linear = std::move(out);
    res.image.meta = baseMeta.meta;
    res.image.sceneReferred = false;
    rep.secMerge = secondsSince(tm);
    ctl.report(1.f, "done");
    return res;
#endif
  }

  // ---------------- Linear merge ----------------
  ImageF num(W, H);
  std::vector<float> den(std::size_t(W) * H, 0.f);
  const int af = std::max(1, int(std::ceil(std::sqrt(double(W) * H / 6.0e6))));  // ghost analysis factor
  const GhostParams gp = ghostParams(opt.deghost);
  const float baseIso = std::max(1.f, base.meta.iso / 100.f);

  auto accumulate = [&](const ImageF& fr, const std::vector<std::uint8_t>* valid, double k, const std::vector<float>* ghost,
                        int gw, int gh) {
    const float kk = float(k), invk = 1.f / kk, k2 = kk * kk;
    parallelRows(H, [&](int y) {
      const float* s = fr.row(y);
      float* n = num.row(y);
      float* d = den.data() + std::size_t(y) * W;
      for (int x = 0; x < W; ++x) {
        if (valid && !(*valid)[std::size_t(y) * W + x]) continue;
        const float* p = s + 3 * x;
        const float mx = std::max({p[0], p[1], p[2]});
        const float vm = std::max(0.f, (p[0] + p[1] + p[2]) * (1.f / 3.f));
        float w = smooth01(0.95f, 0.78f, mx) * k2 / (kReadRel + vm);
        if (ghost) w *= 1.f - std::clamp(sampleLow(*ghost, gw, gh, af, x, y), 0.f, 1.f);
        if (w <= 0.f) continue;
        n[3 * x] += w * p[0] * invk;
        n[3 * x + 1] += w * p[1] * invk;
        n[3 * x + 2] += w * p[2] * invk;
        d[x] += w;
      }
    });
  };
  // Even clipped pixels keep a tiny weight so a fully clipped column still resolves to the base value.
  auto addFloor = [&](const ImageF& fr, double k) {
    const float invk = 1.f / float(k);
    parallelRows(H, [&](int y) {
      const float* s = fr.row(y);
      float* n = num.row(y);
      float* d = den.data() + std::size_t(y) * W;
      for (int x = 0; x < W; ++x) {
        const float wf = 1e-6f;
        for (int c = 0; c < 3; ++c) n[3 * x + c] += wf * s[3 * x + c] * invk;
        d[x] += wf;
      }
    });
  };

  Analysis baseAn;
  const bool wantGhost = opt.deghost != Deghost::Off;
  if (wantGhost) baseAn = analyse(base.linear, af, 1.0);
  accumulate(base.linear, nullptr, 1.0, nullptr, 0, 0);
  addFloor(base.linear, 1.0);

#ifdef DAPPLE_HAVE_OPENCV
  BaseReg baseReg;
#endif

  double secAlign = 0, secMerge = 0;
  int done = 0;
  for (int i = 0; i < count; ++i) {
    HdrFrameReport& fr = rep.frames[i];
    if (i == baseIdx) {
      fr.evUsed = 0;
      fr.evExif = 0;
      continue;
    }
    if (ctl.cancelled()) return fail("cancelled");
    stage(done, 0.0f, "load");
    auto tl = clk::now();
    auto r = load(i);
    if (!r) return std::unexpected(r.error());
    if (auto f = fitToBase(*r, W, H); !f) return std::unexpected(f.error());
    rep.secLoad += secondsSince(tl);

    const ExifExposure fe = exifExposure(r->meta);
    double kExif = kNaN;
    if (fe.known && baseExif.known) kExif = std::exp2(fe.stops - baseExif.stops);
    fr.evExif = std::isfinite(kExif) ? std::log2(kExif) : kNaN;
    double k0 = std::isfinite(kExif) ? kExif : 0;
    if (k0 <= 0) {
      const double mb = medianLuma(base.linear), mf = medianLuma(r->linear);
      k0 = (mb > 1e-5 && mf > 1e-5) ? mf / mb : 1.0;
    }

    // ---- align ----
    auto ta = clk::now();
    ImageF aligned;
    const ImageF* use = &r->linear;
    std::vector<std::uint8_t> valid;
    const std::vector<std::uint8_t>* validPtr = nullptr;
    stage(done, 0.25f, "align");
#ifdef DAPPLE_HAVE_OPENCV
    if (opt.align) {
      AlignOut a = alignFrame(base.linear, r->linear, k0, baseReg);
      if (!a.ok) return fail("Could not align frame " + std::to_string(i) + " (" + names[i] + "): " + a.why);
      fr.alignMethod = a.method;
      fr.alignScore = a.score;
      fr.shiftPx = cornerShift(cv::Matx33d(a.H), W, H);
      if (fr.shiftPx > 0.02) {
        warpToBase(r->linear, a.H, aligned, valid);
        r->linear = ImageF();  // release the unwarped copy
        use = &aligned;
        validPtr = &valid;
        fr.aligned = true;
      }
    }
#else
    if (opt.align && i == (baseIdx == 0 ? 1 : 0)) rep.warnings.push_back("built without OpenCV: frames were not aligned");
#endif
    secAlign += secondsSince(ta);

    // ---- exposure ratio ----
    stage(done, 0.5f, "merge");
    auto tm = clk::now();
    double kData = 0;
    std::size_t nSamp = 0;
    const bool dataOk = estimateRatio(base.linear, *use, validPtr, kData, nSamp);
    double k = 0;
    if (dataOk && std::isfinite(kExif)) {
      if (std::fabs(std::log2(kData / kExif)) > 0.35) {
        rep.warnings.push_back("frame " + std::to_string(i) + ": measured exposure ratio differs from EXIF by " +
                               std::to_string(std::log2(kData / kExif)) + " EV; using the measured one");
        k = kData;
        fr.evFromData = true;
      } else {
        k = kData;  // measured value is more accurate than the nominal one
      }
    } else if (dataOk) {
      k = kData;
      fr.evFromData = true;
    } else if (std::isfinite(kExif)) {
      k = kExif;
      rep.warnings.push_back("frame " + std::to_string(i) + ": too few usable pixels to measure exposure, using EXIF");
    } else {
      return fail("Cannot determine the exposure difference of frame " + std::to_string(i));
    }
    fr.evUsed = std::log2(k);

    // ---- deghost ----
    std::vector<float> ghost;
    int gw = 0, gh = 0;
    if (wantGhost) {
      stage(done, 0.6f, "deghost");
      Analysis an = analyse(*use, af, k);
      std::vector<std::uint8_t> validLow;
      if (validPtr) {
        validLow.assign(std::size_t(an.w) * an.h, 1);
        for (int y = 0; y < H; ++y)
          for (int x = 0; x < W; ++x)
            if (!(*validPtr)[std::size_t(y) * W + x]) validLow[std::size_t(y / af) * an.w + x / af] = 0;
      }
      ghost = detectGhost(baseAn, an, k, std::max(baseIso, std::max(1.f, r->meta.iso / 100.f)), gp,
                          validPtr ? &validLow : nullptr);
      gw = an.w;
      gh = an.h;
      double sum = 0;
      for (float g : ghost) sum += g;
      fr.ghostFraction = ghost.empty() ? 0 : sum / ghost.size();
    }
    stage(done, 0.8f, "merge");
    accumulate(*use, validPtr, k, wantGhost ? &ghost : nullptr, gw, gh);
    secMerge += secondsSince(tm);
    ++done;
  }

  // ---- normalise ----
  auto tf = clk::now();
  ImageF& out = num;
  double maxV = 0;
  std::vector<double> rowMax(H, 0.0);
  parallelRows(H, [&](int y) {
    float* n = out.row(y);
    const float* d = den.data() + std::size_t(y) * W;
    double m = 0;
    for (int x = 0; x < W; ++x) {
      const float inv = d[x] > 0 ? 1.f / d[x] : 0.f;
      for (int c = 0; c < 3; ++c) {
        n[3 * x + c] *= inv;
        m = std::max<double>(m, n[3 * x + c]);
      }
    }
    rowMax[y] = m;
  });
  for (double m : rowMax) maxV = std::max(maxV, m);
  rep.maxValue = maxV;
  secMerge += secondsSince(tf);
  rep.secAlign = secAlign;
  rep.secMerge = secMerge;

  if (auto v = validateMerged(out); !v) return std::unexpected(v.error());
  res.image.linear = std::move(out);
  res.image.meta = base.meta;
  res.image.sceneReferred = true;
  ctl.report(1.f, "done");
  return res;
}

int pickBase(const std::vector<ExifExposure>& ex, int requested, int count) {
  if (requested >= 0 && requested < count) return requested;
  std::vector<int> order(count);
  std::iota(order.begin(), order.end(), 0);
  const bool all = std::all_of(ex.begin(), ex.end(), [](const ExifExposure& e) { return e.known; });
  if (all) std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ex[a].stops < ex[b].stops; });
  return order[(count - 1) / 2];
}

}  // namespace

Result<HdrResult> mergeHdr(const std::vector<std::filesystem::path>& files, const HdrOptions& options,
                           const MergeControl& control) {
  try {
    const int n = int(files.size());
    if (n < 2 || n > 9) return fail("HDR merge needs 2 to 9 frames");
    static const DecoderRegistry registry = DecoderRegistry::withBuiltins();
    std::vector<ExifExposure> ex(n);
    std::vector<std::string> names(n);
    for (int i = 0; i < n; ++i) {
      names[i] = pathToUtf8(files[i]);
      const ImageDecoder* dec = registry.find(files[i]);
      if (!dec) return fail("Unsupported file type: " + names[i]);
      auto meta = dec->probe(files[i]);
      if (!meta) return fail(names[i] + ": " + meta.error().message);
      ex[i] = exifExposure(*meta);
    }
    const int base = pickBase(ex, options.baseIndex, n);
    FrameLoader loader = [&](int i) -> Result<SourceImage> {
      const ImageDecoder* dec = registry.find(files[i]);
      auto src = dec->decode(files[i], DecodeOptions{options.quality});
      if (!src) return fail(names[i] + ": " + src.error().message);
      if (options.longEdge > 0) src->linear = resizeToLongEdge(src->linear, options.longEdge);
      return src;
    };
    return mergeEngine(n, base, names, loader, options, control);
  } catch (const std::exception& e) {
    return fail(std::string("HDR merge failed: ") + e.what());
  }
}

Result<HdrResult> mergeHdrImages(std::vector<SourceImage> frames, const HdrOptions& options,
                                 const MergeControl& control) {
  try {
    const int n = int(frames.size());
    if (n < 2 || n > 9) return fail("HDR merge needs 2 to 9 frames");
    std::vector<ExifExposure> ex(n);
    std::vector<std::string> names(n);
    for (int i = 0; i < n; ++i) {
      ex[i] = exifExposure(frames[i].meta);
      names[i] = "frame" + std::to_string(i);
    }
    const int base = pickBase(ex, options.baseIndex, n);
    FrameLoader loader = [&](int i) -> Result<SourceImage> {
      SourceImage s = std::move(frames[i]);
      if (options.longEdge > 0) s.linear = resizeToLongEdge(s.linear, options.longEdge);
      return s;
    };
    return mergeEngine(n, base, names, loader, options, control);
  } catch (const std::exception& e) {
    return fail(std::string("HDR merge failed: ") + e.what());
  }
}

}  // namespace dapple
