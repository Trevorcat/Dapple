#pragma once
// Shared helpers for hdr.cpp / pano.cpp / sequence.cpp. Internal.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "color.h"
#include "dapple/image.h"
#include "dapple/merge.h"
#include "parallel.h"

#ifdef DAPPLE_HAVE_OPENCV
#include <opencv2/core.hpp>
#endif

namespace dapple::mergeutil {

inline double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

inline float smooth01(float e0, float e1, float x) {
  float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

// Running-sum box blur of a planar float plane (clamped borders). In place via tmp.
inline void boxBlur(std::vector<float>& plane, int w, int h, int radius) {
  if (radius <= 0 || w <= 0 || h <= 0) return;
  std::vector<float> tmp(plane.size());
  const float inv = 1.f / float(2 * radius + 1);
  parallelRows(h, [&](int y) {
    const float* s = plane.data() + std::size_t(y) * w;
    float* d = tmp.data() + std::size_t(y) * w;
    double acc = 0;
    for (int i = -radius; i <= radius; ++i) acc += s[std::clamp(i, 0, w - 1)];
    for (int x = 0; x < w; ++x) {
      d[x] = float(acc) * inv;
      acc += s[std::min(w - 1, x + radius + 1)] - s[std::max(0, x - radius)];
    }
  });
  // vertical pass: column chunks run in parallel, rows stream through an accumulator row
  const int chunk = 256;
  const int chunks = (w + chunk - 1) / chunk;
  parallelFor(chunks, [&](int c) {
    const int x0 = c * chunk, x1 = std::min(w, x0 + chunk), n = x1 - x0;
    std::vector<double> acc(n, 0.0);
    for (int i = -radius; i <= radius; ++i) {
      const float* s = tmp.data() + std::size_t(std::clamp(i, 0, h - 1)) * w + x0;
      for (int x = 0; x < n; ++x) acc[x] += s[x];
    }
    for (int y = 0; y < h; ++y) {
      float* d = plane.data() + std::size_t(y) * w + x0;
      for (int x = 0; x < n; ++x) d[x] = float(acc[x]) * inv;
      const float* sa = tmp.data() + std::size_t(std::min(h - 1, y + radius + 1)) * w + x0;
      const float* sr = tmp.data() + std::size_t(std::max(0, y - radius)) * w + x0;
      for (int x = 0; x < n; ++x) acc[x] += sa[x] - sr[x];
    }
  });
}

#ifdef DAPPLE_HAVE_OPENCV
// Zero-copy views (the Mat must not outlive the image).
inline cv::Mat asMat(ImageF& im) { return cv::Mat(im.height, im.width, CV_32FC3, im.data.data()); }
inline cv::Mat asMat(const ImageF& im) {
  return cv::Mat(im.height, im.width, CV_32FC3, const_cast<float*>(im.data.data()));
}
inline ImageF fromMat(const cv::Mat& m) {
  CV_Assert(m.type() == CV_32FC3);
  ImageF out(m.cols, m.rows);
  for (int y = 0; y < m.rows; ++y) std::copy_n(m.ptr<float>(y), std::size_t(m.cols) * 3, out.row(y));
  return out;
}
#endif

}  // namespace dapple::mergeutil
