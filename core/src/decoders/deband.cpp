#include "deband.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "parallel.h"

namespace dapple {

namespace {
constexpr int kMinSide = 24;
constexpr float kBlockPerLongEdge = 150.f;  // blocks along the long edge
constexpr float kCellJoin = 1.5f;           // coarse smoothing: neighbouring cells within this many codes are averaged
constexpr float kPixelJoin = 2.5f;          // joint-bilateral: coarse cells within this many codes of the pixel count
}  // namespace

Deband8::Deband8(const std::uint8_t* rgb, int w, int h) : px_(rgb), w_(w) {
  if (!rgb || std::min(w, h) < kMinSide) return;
  f_ = std::max(2, int(std::lround(double(std::max(w, h)) / kBlockPerLongEdge)));
  gate_.assign(std::size_t(w) * h, 0);
  // 1. flatness: the largest 3x3 range over the three channels (integer codes: 0, 1 -> 1; 2 -> 2/3; 3 and more -> 0)
  static const std::uint8_t kGateOfRange[3] = {255, 255, 170};
  std::atomic<bool> any{false};
  parallelRows(h, [&](int y) {
    thread_local std::vector<std::uint8_t> vmin, vmax;
    vmin.resize(std::size_t(w) * 3);
    vmax.resize(std::size_t(w) * 3);
    const int ya = std::max(0, y - 1), yb = std::min(h - 1, y + 1);
    for (int i = 0; i < w * 3; ++i) {
      std::uint8_t lo = 255, hi = 0;
      for (int yy = ya; yy <= yb; ++yy) {
        const std::uint8_t v = px_[std::size_t(yy) * w * 3 + i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      vmin[i] = lo;
      vmax[i] = hi;
    }
    std::uint8_t* g = gate_.data() + std::size_t(y) * w;
    bool rowAny = false;
    for (int x = 0; x < w; ++x) {
      const int xa = std::max(0, x - 1), xb = std::min(w - 1, x + 1);
      int range = 0;
      for (int c = 0; c < 3; ++c) {
        int lo = 255, hi = 0;
        for (int xx = xa; xx <= xb; ++xx) {
          lo = std::min<int>(lo, vmin[std::size_t(xx) * 3 + c]);
          hi = std::max<int>(hi, vmax[std::size_t(xx) * 3 + c]);
        }
        range = std::max(range, hi - lo);
      }
      g[x] = range < 3 ? kGateOfRange[range] : 0;
      rowAny |= g[x] != 0;
    }
    if (rowAny) any.store(true, std::memory_order_relaxed);
  });
  if (!any.load()) return;

  // 2. coarse grid: flatness-weighted block means
  cw_ = (w + f_ - 1) / f_;
  ch_ = (h + f_ - 1) / f_;
  grid_.assign(std::size_t(cw_) * ch_ * 3, -1.f);
  parallelFor(ch_, [&](int by) {
    const int y0 = by * f_, y1 = std::min(h, y0 + f_);
    for (int bx = 0; bx < cw_; ++bx) {
      const int x0 = bx * f_, x1 = std::min(w, x0 + f_);
      double sw = 0, s[3] = {0, 0, 0};
      for (int y = y0; y < y1; ++y) {
        const std::uint8_t* g = gate_.data() + std::size_t(y) * w;
        const std::uint8_t* p = px_ + std::size_t(y) * w * 3;
        for (int x = x0; x < x1; ++x) {
          if (!g[x]) continue;
          const double k = g[x] * (1.0 / 255.0);
          sw += k;
          for (int c = 0; c < 3; ++c) s[c] += k * p[3 * x + c];
        }
      }
      if (sw < 0.25 * double(x1 - x0) * double(y1 - y0)) continue;
      float* o = grid_.data() + (std::size_t(by) * cw_ + bx) * 3;
      for (int c = 0; c < 3; ++c) o[c] = float(s[c] / sw);
    }
  });
  // two passes of the edge-limited 3x3 average over the cells
  std::vector<float> tmp(grid_.size());
  for (int it = 0; it < 2; ++it) {
    parallelFor(ch_, [&](int by) {
      for (int bx = 0; bx < cw_; ++bx) {
        const float* c0 = grid_.data() + (std::size_t(by) * cw_ + bx) * 3;
        float* o = tmp.data() + (std::size_t(by) * cw_ + bx) * 3;
        if (c0[0] < 0.f) {
          o[0] = o[1] = o[2] = -1.f;
          continue;
        }
        for (int c = 0; c < 3; ++c) {
          float acc = 0.f;
          int n = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int yy = by + dy, xx = bx + dx;
              if (yy < 0 || yy >= ch_ || xx < 0 || xx >= cw_) continue;
              const float v = grid_[(std::size_t(yy) * cw_ + xx) * 3 + c];
              if (v < 0.f || std::fabs(v - c0[c]) > kCellJoin) continue;
              acc += v;
              ++n;
            }
          o[c] = acc / float(n);
        }
      }
    });
    grid_.swap(tmp);
  }
  active_ = true;
}

void Deband8::pixel(int x, int y, float out[3]) const {
  const std::uint8_t* p = px_ + (std::size_t(y) * w_ + x) * 3;
  const float I[3] = {float(p[0]), float(p[1]), float(p[2])};
  out[0] = I[0], out[1] = I[1], out[2] = I[2];
  if (!active_) return;
  const int g = gate_[std::size_t(y) * w_ + x];
  if (!g) return;
  const float fx = (float(x) + 0.5f) / float(f_) - 0.5f, fy = (float(y) + 0.5f) / float(f_) - 0.5f;
  const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
  const float tx = fx - float(ix), ty = fy - float(iy);
  const float wx[2] = {1.f - tx, tx}, wy[2] = {1.f - ty, ty};
  for (int c = 0; c < 3; ++c) {
    float acc = 0.f, ws = 0.f;
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) {
        const int cx = std::clamp(ix + i, 0, cw_ - 1), cy = std::clamp(iy + j, 0, ch_ - 1);
        const float v = grid_[(std::size_t(cy) * cw_ + cx) * 3 + c];
        if (v < 0.f || std::fabs(v - I[c]) > kPixelJoin) continue;
        const float k = wx[i] * wy[j];
        acc += k * v;
        ws += k;
      }
    if (ws < 0.5f) continue;
    const float S = acc / ws, dev = std::fabs(S - I[c]);
    const float wd = dev <= 1.f ? 1.f : (dev >= 2.f ? 0.f : 2.f - dev);
    out[c] = I[c] + (S - I[c]) * (float(g) * (1.f / 255.f) * wd);
  }
}

std::vector<float> debandCodes(const std::uint8_t* rgb, int w, int h) {
  std::vector<float> o(std::size_t(w) * h * 3);
  const Deband8 d(rgb, w, h);
  parallelRows(h, [&](int y) {
    for (int x = 0; x < w; ++x) d.pixel(x, y, o.data() + (std::size_t(y) * w + x) * 3);
  });
  return o;
}

}  // namespace dapple
