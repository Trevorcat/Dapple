// AI masks, the pure part: plane builders (polygon rasteriser, face features from the 478-point mesh, person part splitting, depth, tap
// accumulation, sky prior), the id grammar and the cache file format. No runtime, no model: unit-tested on the desktop with fixed inputs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "ai_masks.h"

namespace dapple::aimask {

// ---- topology: MediaPipe face_mesh_connections.py, FACEMESH_* sets chained into rings (see docs/legal/ai-models.md for the licence) ------
const int kFaceOval[36] = {10, 109, 67, 103, 54, 21, 162, 127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148,
                           152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454, 356, 389, 251, 284, 332, 297, 338};
const int kLipsOuter[20] = {0, 37, 39, 40, 185, 61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267};
const int kLipsInner[20] = {13, 82, 81, 80, 191, 78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308, 415, 310, 311, 312};
const int kEyeLeft[16] = {249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466, 263};
const int kEyeRight[16] = {7, 33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163};
const int kIrisLeft[4] = {474, 475, 476, 477};
const int kIrisRight[4] = {469, 470, 471, 472};
// eyebrow edges: the upper chain runs outer -> inner, the lower chain outer -> inner (so upper + reversed lower is a closed ring)
const int kBrowRightUpper[5] = {70, 63, 105, 66, 107};
const int kBrowRightLower[5] = {46, 53, 52, 65, 55};
const int kBrowLeftUpper[5] = {300, 293, 334, 296, 336};
const int kBrowLeftLower[5] = {276, 283, 282, 295, 285};

namespace {
inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline std::uint8_t to8(float v) { return std::uint8_t(std::lround(clamp01(v) * 255.f)); }
inline float sstep(float a, float b, float x) {
  const float t = clamp01((x - a) / (b - a));
  return t * t * (3.f - 2.f * t);
}

// Separable gaussian on a float buffer (zero outside).
void gaussBlur(std::vector<float>& buf, int w, int h, float sigma) {
  if (sigma < 0.3f) return;
  const int R = std::max(1, int(std::ceil(3.f * sigma)));
  std::vector<float> k(std::size_t(2 * R + 1));
  float sum = 0;
  for (int i = -R; i <= R; ++i) sum += k[std::size_t(i + R)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
  for (float& v : k) v /= sum;
  std::vector<float> tmp(buf.size(), 0.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float a = 0;
      for (int i = -R; i <= R; ++i) {
        const int xx = x + i;
        if (xx >= 0 && xx < w) a += buf[std::size_t(y) * w + xx] * k[std::size_t(i + R)];
      }
      tmp[std::size_t(y) * w + x] = a;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float a = 0;
      for (int i = -R; i <= R; ++i) {
        const int yy = y + i;
        if (yy >= 0 && yy < h) a += tmp[std::size_t(yy) * w + x] * k[std::size_t(i + R)];
      }
      buf[std::size_t(y) * w + x] = a;
    }
}
}  // namespace

Plane polygonPlane(int w, int h, const std::vector<std::vector<Pt>>& rings, float softSigma) {
  Plane p;
  p.w = w, p.h = h;
  p.px.assign(std::size_t(w) * h, 0);
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
  for (const auto& r : rings)
    for (const Pt& q : r) minx = std::min(minx, q.x), maxx = std::max(maxx, q.x), miny = std::min(miny, q.y), maxy = std::max(maxy, q.y);
  if (!(maxx > minx) || !(maxy > miny)) return p;
  const int pad = int(std::ceil(3.f * softSigma)) + 2;
  const int bx0 = std::max(0, int(std::floor(minx)) - pad), by0 = std::max(0, int(std::floor(miny)) - pad);
  const int bx1 = std::min(w, int(std::ceil(maxx)) + pad), by1 = std::min(h, int(std::ceil(maxy)) + pad);
  if (bx1 <= bx0 || by1 <= by0) return p;
  const int bw = bx1 - bx0, bh = by1 - by0;
  std::vector<float> cov(std::size_t(bw) * bh, 0.f);
  constexpr int SS = 4;
  std::vector<float> xs;
  for (int sy = by0 * SS; sy < by1 * SS; ++sy) {
    const float yc = (float(sy) + 0.5f) / SS;
    if (yc < miny || yc > maxy) continue;
    xs.clear();
    for (const auto& r : rings) {
      const std::size_t n = r.size();
      for (std::size_t i = 0; i < n; ++i) {
        const Pt &a = r[i], &b = r[(i + 1) % n];
        if ((a.y <= yc) != (b.y <= yc)) xs.push_back(a.x + (yc - a.y) / (b.y - a.y) * (b.x - a.x));
      }
    }
    std::sort(xs.begin(), xs.end());
    float* row = cov.data() + std::size_t(sy / SS - by0) * bw;
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
      const float xa = std::clamp(xs[i], float(bx0), float(bx1)), xb = std::clamp(xs[i + 1], float(bx0), float(bx1));
      if (xb <= xa) continue;
      const int ia = int(std::floor(xa)), ib = int(std::floor(xb));
      if (ia == ib) {
        row[ia - bx0] += (xb - xa) / SS;
      } else {
        row[ia - bx0] += (float(ia + 1) - xa) / SS;
        for (int x = ia + 1; x < ib; ++x) row[x - bx0] += 1.f / SS;
        if (ib < bx1) row[ib - bx0] += (xb - float(ib)) / SS;
      }
    }
  }
  gaussBlur(cov, bw, bh, softSigma);
  for (int y = 0; y < bh; ++y)
    for (int x = 0; x < bw; ++x) p.px[std::size_t(by0 + y) * w + bx0 + x] = to8(cov[std::size_t(y) * bw + x]);
  return p;
}

Plane resizePlane(const std::uint8_t* src, int sw, int sh, int dw, int dh) {
  Plane p;
  p.w = dw, p.h = dh;
  p.px.resize(std::size_t(dw) * dh);
  for (int y = 0; y < dh; ++y) {
    const float fy = std::clamp((float(y) + 0.5f) * float(sh) / float(dh) - 0.5f, 0.f, float(sh - 1));
    const int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
    const float ty = fy - float(y0);
    for (int x = 0; x < dw; ++x) {
      const float fx = std::clamp((float(x) + 0.5f) * float(sw) / float(dw) - 0.5f, 0.f, float(sw - 1));
      const int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
      const float tx = fx - float(x0);
      const float a = src[std::size_t(y0) * sw + x0] + (src[std::size_t(y0) * sw + x1] - src[std::size_t(y0) * sw + x0]) * tx;
      const float b = src[std::size_t(y1) * sw + x0] + (src[std::size_t(y1) * sw + x1] - src[std::size_t(y1) * sw + x0]) * tx;
      p.px[std::size_t(y) * dw + x] = std::uint8_t(std::lround(a + (b - a) * ty));
    }
  }
  return p;
}

Plane resizePlaneF(const float* src, int sw, int sh, int dw, int dh, float lo, float hi) {
  Plane p;
  p.w = dw, p.h = dh;
  p.px.resize(std::size_t(dw) * dh);
  const float inv = 1.f / std::max(1e-12f, hi - lo);
  for (int y = 0; y < dh; ++y) {
    const float fy = std::clamp((float(y) + 0.5f) * float(sh) / float(dh) - 0.5f, 0.f, float(sh - 1));
    const int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
    const float ty = fy - float(y0);
    for (int x = 0; x < dw; ++x) {
      const float fx = std::clamp((float(x) + 0.5f) * float(sw) / float(dw) - 0.5f, 0.f, float(sw - 1));
      const int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
      const float tx = fx - float(x0);
      const float a = src[std::size_t(y0) * sw + x0] + (src[std::size_t(y0) * sw + x1] - src[std::size_t(y0) * sw + x0]) * tx;
      const float b = src[std::size_t(y1) * sw + x0] + (src[std::size_t(y1) * sw + x1] - src[std::size_t(y1) * sw + x0]) * tx;
      p.px[std::size_t(y) * dw + x] = to8((a + (b - a) * ty - lo) * inv);
    }
  }
  return p;
}

void planeMul(Plane& a, const Plane& b) {
  if (a.px.size() != b.px.size()) return;
  for (std::size_t i = 0; i < a.px.size(); ++i) a.px[i] = std::uint8_t((unsigned(a.px[i]) * b.px[i] + 127) / 255);
}
void planeSub(Plane& a, const Plane& b) {
  if (a.px.size() != b.px.size()) return;
  for (std::size_t i = 0; i < a.px.size(); ++i) a.px[i] = std::uint8_t((unsigned(a.px[i]) * (255u - b.px[i]) + 127) / 255);
}
void planeMax(Plane& a, const Plane& b) {
  if (a.px.size() != b.px.size()) return;
  for (std::size_t i = 0; i < a.px.size(); ++i) a.px[i] = std::max(a.px[i], b.px[i]);
}
void planeInvert(Plane& a) {
  for (auto& v : a.px) v = std::uint8_t(255 - v);
}
double planeSum(const Plane& a) {
  double s = 0;
  for (auto v : a.px) s += v;
  return s / 255.0;
}

// ---- face features ------------------------------------------------------------------------------------------------------------------------
namespace {
template <std::size_t N>
std::vector<Pt> ring(const FaceMesh& m, const int (&idx)[N]) {
  std::vector<Pt> r;
  r.reserve(N);
  for (int i : idx) r.push_back({m.xy[i][0], m.xy[i][1]});
  return r;
}
std::vector<Pt> browRing(const FaceMesh& m, const int (&up)[5], const int (&lo)[5]) {
  std::vector<Pt> r;
  for (int i : up) r.push_back({m.xy[i][0], m.xy[i][1]});
  for (int i = 4; i >= 0; --i) r.push_back({m.xy[lo[i]][0], m.xy[lo[i]][1]});
  return r;
}
std::vector<Pt> irisRing(const FaceMesh& m, const int (&idx)[4]) {
  float cx = 0, cy = 0;
  for (int i : idx) cx += m.xy[i][0], cy += m.xy[i][1];
  cx /= 4, cy /= 4;
  float r = 0;
  for (int i : idx) r += std::hypot(m.xy[i][0] - cx, m.xy[i][1] - cy);
  r /= 4;
  std::vector<Pt> c;
  for (int k = 0; k < 28; ++k) {
    const float a = 6.2831853f * float(k) / 28.f;
    c.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
  }
  return c;
}
}  // namespace

// Under-eye crescents (docs/engine/pipeline-architecture.md 43.4): below each lower lid, from the outer to the inner corner, starting 3.5 % of the
// inter-ocular distance below the lid (lashes) and reaching 3.5 + 24 % at its deepest point (t = 0.6 of the lid's arc length, toward the inner
// corner where dark circles sit), soft edge sigma 3.5 %, minus the lid opening. "Down" = perpendicular to the iris-to-iris line, toward the mouth.
Plane underEyePlane(const FaceMesh& m, int w, int h, const Plane& eyes) {
  static const int kLidR[9] = {33, 7, 163, 144, 145, 153, 154, 155, 133};
  static const int kLidL[9] = {263, 249, 390, 373, 374, 380, 381, 382, 362};
  const float ex = m.xy[473][0] - m.xy[468][0], ey = m.xy[473][1] - m.xy[468][1];
  const float iod = std::hypot(ex, ey);
  if (!(iod > 2.f)) return Plane{};
  float dx = ey / iod, dy = -ex / iod;
  const float mx = 0.5f * (m.xy[13][0] + m.xy[14][0]) - 0.5f * (m.xy[468][0] + m.xy[473][0]);
  const float my = 0.5f * (m.xy[13][1] + m.xy[14][1]) - 0.5f * (m.xy[468][1] + m.xy[473][1]);
  if (dx * mx + dy * my < 0.f) dx = -dx, dy = -dy;
  const float peakExp = std::log(0.5f) / std::log(0.6f);
  std::vector<std::vector<Pt>> rings;
  for (const int* lid : {kLidR, kLidL}) {
    float cum[9] = {0};
    for (int i = 1; i < 9; ++i) cum[i] = cum[i - 1] + std::hypot(m.xy[lid[i]][0] - m.xy[lid[i - 1]][0], m.xy[lid[i]][1] - m.xy[lid[i - 1]][1]);
    if (!(cum[8] > 0.f)) continue;
    std::vector<Pt> up, lo;
    for (int k = 0; k < 24; ++k) {
      const float t = float(k) / 23.f, at = t * cum[8];
      int i = 0;
      while (i < 7 && cum[i + 1] < at) ++i;
      const float seg = std::max(cum[i + 1] - cum[i], 1e-6f), u = std::clamp((at - cum[i]) / seg, 0.f, 1.f);
      const float px = m.xy[lid[i]][0] + (m.xy[lid[i + 1]][0] - m.xy[lid[i]][0]) * u;
      const float py = m.xy[lid[i]][1] + (m.xy[lid[i + 1]][1] - m.xy[lid[i]][1]) * u;
      const float s = std::pow(std::max(0.f, std::sin(3.14159265f * std::pow(t, peakExp))), 0.7f);
      const float d0 = 0.035f * iod, d1 = (0.035f + 0.24f * s) * iod;
      up.push_back({px + dx * d0, py + dy * d0});
      lo.push_back({px + dx * d1, py + dy * d1});
    }
    std::vector<Pt> r = up;
    for (int k = 23; k >= 0; --k) r.push_back(lo[std::size_t(k)]);
    rings.push_back(std::move(r));
  }
  if (rings.empty()) return Plane{};
  Plane p = polygonPlane(w, h, rings, std::max(0.6f, 0.035f * iod));
  if (!eyes.empty() && eyes.w == p.w && eyes.h == p.h) planeSub(p, eyes);
  return p;
}

bool faceFeatures(const FaceMesh& m, int w, int h, const std::uint8_t* rgb, int stride, FaceFeatures& out) {
  out = FaceFeatures{};
  if (w < 8 || h < 8) return false;
  for (int i = 0; i < 478; ++i)
    if (!std::isfinite(m.xy[i][0]) || !std::isfinite(m.xy[i][1])) return false;
  const float dx = m.xy[454][0] - m.xy[234][0], dy = m.xy[454][1] - m.xy[234][1], dz = m.z[454] - m.z[234];
  const float faceW = std::hypot(dx, dy);
  const float dist3 = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(faceW > 4.f)) return false;
  out.yawDeg = std::asin(std::clamp(dz / std::max(dist3, 1e-6f), -1.f, 1.f)) * 57.29578f;
  out.frontal = std::fabs(out.yawDeg) < kFrontalYawDeg;
  out.face = polygonPlane(w, h, {ring(m, kFaceOval)}, std::max(0.8f, 0.012f * faceW));
  const float gap = std::hypot(m.xy[13][0] - m.xy[14][0], m.xy[13][1] - m.xy[14][1]);
  const float width = std::max(1e-3f, std::hypot(m.xy[78][0] - m.xy[308][0], m.xy[78][1] - m.xy[308][1]));
  out.mouthOpen = gap / width;
  out.iod = std::hypot(m.xy[473][0] - m.xy[468][0], m.xy[473][1] - m.xy[468][1]);
  if (!out.frontal) return true;
  const float sEye = std::max(0.6f, 0.006f * faceW), sMouth = std::max(0.6f, 0.008f * faceW);
  out.eyes = polygonPlane(w, h, {ring(m, kEyeRight), ring(m, kEyeLeft)}, sEye);
  Plane iris = polygonPlane(w, h, {irisRing(m, kIrisRight), irisRing(m, kIrisLeft)}, sEye);
  planeMul(iris, out.eyes);  // the iris is seen through the lid opening only
  out.sclera = out.eyes;
  planeSub(out.sclera, iris);
  out.iris = std::move(iris);
  out.lips = polygonPlane(w, h, {ring(m, kLipsOuter), ring(m, kLipsInner)}, sMouth);
  out.undereye = underEyePlane(m, w, h, out.eyes);
  {  // the mesh brow edges (upper and lower chain) are only a hair apart: grow the band to the width of a brow (2 % of the face width each side)
    const float grow = 0.02f * faceW;
    std::vector<std::vector<Pt>> rings = {browRing(m, kBrowRightUpper, kBrowRightLower), browRing(m, kBrowLeftUpper, kBrowLeftLower)};
    for (auto& r : rings) {
      float cx = 0, cy = 0;
      for (const Pt& q : r) cx += q.x, cy += q.y;
      cx /= float(r.size()), cy /= float(r.size());
      // push every point away from the centre line of the band: along the band's normal at that point
      std::vector<Pt> o = r;
      const std::size_t n = r.size();
      for (std::size_t i = 0; i < n; ++i) {
        const Pt &a0 = r[(i + n - 1) % n], &a1 = r[(i + 1) % n];
        float tx = a1.x - a0.x, ty = a1.y - a0.y;
        const float tl = std::max(1e-3f, std::hypot(tx, ty));
        tx /= tl, ty /= tl;
        float nx = -ty, ny = tx;
        if (nx * (r[i].x - cx) + ny * (r[i].y - cy) < 0) nx = -nx, ny = -ny;  // outward
        o[i] = {r[i].x + nx * grow, r[i].y + ny * grow};
      }
      r = o;
    }
    out.brows = polygonPlane(w, h, rings, std::max(0.6f, 0.008f * faceW));
  }
  if (out.mouthOpen >= kMouthOpen) {
    Plane inner = polygonPlane(w, h, {ring(m, kLipsInner)}, std::max(0.5f, 0.004f * faceW));
    if (rgb) {
      // keep the bright, unsaturated pixels of the opening (teeth); tongue and the dark mouth interior drop out
      std::vector<float> Y;
      std::vector<std::size_t> at;
      for (std::size_t i = 0; i < inner.px.size(); ++i)
        if (inner.px[i] > 127) at.push_back(i);
      if (at.size() < 12) return true;
      Y.reserve(at.size());
      for (std::size_t i : at) {
        const std::uint8_t* p = rgb + std::size_t(i / std::size_t(w)) * std::size_t(stride) + (i % std::size_t(w)) * 3;
        Y.push_back(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]);
      }
      std::vector<float> sorted = Y;
      std::sort(sorted.begin(), sorted.end());
      const float yhi = sorted[sorted.size() * 9 / 10];
      if (yhi < 90.f) return true;  // a dark opening: no teeth visible
      Plane t = inner;
      std::size_t keep = 0;
      for (std::size_t k = 0; k < at.size(); ++k) {
        const std::size_t i = at[k];
        const std::uint8_t* p = rgb + std::size_t(i / std::size_t(w)) * std::size_t(stride) + (i % std::size_t(w)) * 3;
        const float mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        const float sat = (mx - mn) / std::max(1.f, mx);
        const float v = sstep(0.55f * yhi, 0.8f * yhi, Y[k]) * (1.f - sstep(0.30f, 0.55f, sat));
        t.px[i] = to8(float(inner.px[i]) / 255.f * v);
        if (v > 0.5f) ++keep;
      }
      for (std::size_t i = 0; i < t.px.size(); ++i)
        if (inner.px[i] <= 127) t.px[i] = 0;
      if (float(keep) < 0.08f * float(at.size())) return true;  // no teeth among the pixels
      out.teeth = std::move(t);
    } else {
      out.teeth = std::move(inner);
    }
    out.open = true;
  }
  return true;
}

// ---- person parts ---------------------------------------------------------------------------------------------------------------------------
// The network sometimes answers "hair" or "skin" on a few blobs of the background. Connected foreground components (argmax != background) smaller than
// 10 % of the largest one are dropped (multi-person photos keep every person of comparable size); a 2 cell margin keeps the soft edges.
static void dropIslands(std::vector<std::uint8_t>& prob, int S) {
  const std::size_t n = std::size_t(S) * S;
  std::vector<int> label(n, 0);
  std::vector<std::size_t> size(1, 0), stack;
  for (std::size_t i = 0; i < n; ++i) {
    if (label[i]) continue;
    int best = 0, bi = 0;
    for (int c = 0; c < 6; ++c)
      if (prob[n * c + i] > best) best = prob[n * c + i], bi = c;
    if (bi == 0) continue;
    const int id = int(size.size());
    size.push_back(0);
    label[i] = id;
    stack.assign(1, i);
    while (!stack.empty()) {
      const std::size_t k = stack.back();
      stack.pop_back();
      ++size[std::size_t(id)];
      const int x = int(k % std::size_t(S)), y = int(k / std::size_t(S));
      const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
      for (auto& q : nb) {
        if (q[0] < 0 || q[1] < 0 || q[0] >= S || q[1] >= S) continue;
        const std::size_t j = std::size_t(q[1]) * S + q[0];
        if (label[j]) continue;
        int b2 = 0, c2 = 0;
        for (int c = 0; c < 6; ++c)
          if (prob[n * c + j] > b2) b2 = prob[n * c + j], c2 = c;
        if (c2 == 0) continue;
        label[j] = id;
        stack.push_back(j);
      }
    }
  }
  std::size_t biggest = 0;
  for (std::size_t v : size) biggest = std::max(biggest, v);
  if (biggest == 0) return;
  std::vector<std::uint8_t> keep(n, 0);
  for (std::size_t i = 0; i < n; ++i)
    if (label[i] && size[std::size_t(label[i])] * 10 >= biggest) keep[i] = 1;
  std::vector<std::uint8_t> grown(n, 0);
  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      bool k = false;
      for (int dy = -2; dy <= 2 && !k; ++dy)
        for (int dx = -2; dx <= 2 && !k; ++dx) {
          const int xx = x + dx, yy = y + dy;
          if (xx >= 0 && yy >= 0 && xx < S && yy < S && keep[std::size_t(yy) * S + xx]) k = true;
        }
      grown[std::size_t(y) * S + x] = k;
    }
  for (std::size_t i = 0; i < n; ++i)
    if (!grown[i]) {
      for (int c = 1; c < 6; ++c) prob[n * c + i] = 0;
      prob[i] = 255;
    }
}

PartPlanes splitParts(const std::uint8_t* probIn, int S, int w, int h) {
  PartPlanes p;
  const std::size_t n = std::size_t(S) * S;
  std::vector<std::uint8_t> probCopy(probIn, probIn + n * 6);
  dropIslands(probCopy, S);
  const std::uint8_t* prob = probCopy.data();
  p.hair = resizePlane(prob + n * kPartHair, S, S, w, h);
  p.bodySkin = resizePlane(prob + n * kPartBodySkin, S, S, w, h);
  p.faceSkin = resizePlane(prob + n * kPartFaceSkin, S, S, w, h);
  p.clothes = resizePlane(prob + n * kPartClothes, S, S, w, h);
  p.background = resizePlane(prob + n * kPartBackground, S, S, w, h);
  p.person = p.background;
  planeInvert(p.person);
  return p;
}
std::vector<std::uint8_t> probFromClassMap(const std::vector<std::uint8_t>& classMap, int S) {
  const std::size_t n = std::size_t(S) * S;
  std::vector<std::uint8_t> prob(n * 6, 0);
  for (std::size_t i = 0; i < n && i < classMap.size(); ++i) prob[n * std::min<std::size_t>(classMap[i], 5) + i] = 255;
  return prob;
}

// ---- depth ------------------------------------------------------------------------------------------------------------------------------------
Plane depthFarPlane(const float* d, int dw, int dh, int w, int h) {
  const std::size_t n = std::size_t(dw) * dh;
  std::vector<float> s;
  const std::size_t step = std::max<std::size_t>(1, n / 65536);
  for (std::size_t i = 0; i < n; i += step)
    if (std::isfinite(d[i])) s.push_back(d[i]);
  if (s.empty()) return Plane{};
  const std::size_t k0 = s.size() * 2 / 100, k1 = std::min(s.size() - 1, s.size() * 98 / 100);
  std::nth_element(s.begin(), s.begin() + long(k0), s.end());
  const float lo = s[k0];
  std::nth_element(s.begin(), s.begin() + long(k1), s.end());
  const float hi = s[k1];
  const float span = std::max(1e-9f, hi - lo);
  std::vector<float> far(n);
  for (std::size_t i = 0; i < n; ++i) far[i] = 1.f - clamp01((d[i] - lo) / span);
  return resizePlaneF(far.data(), dw, dh, w, h, 0.f, 1.f);
}

// ---- taps -------------------------------------------------------------------------------------------------------------------------------------
Plane accumulateTaps(const std::vector<TapMask>& taps, int w, int h) {
  Plane acc;
  acc.w = w, acc.h = h;
  acc.px.assign(std::size_t(w) * h, 0);
  for (const TapMask& t : taps) {
    if (t.mask.w != w || t.mask.h != h) continue;
    if (t.add)
      planeMax(acc, t.mask);
    else
      planeSub(acc, t.mask);
  }
  return acc;
}

// ---- ids ----------------------------------------------------------------------------------------------------------------------------------------
std::string tapSpec(const std::vector<TapPoint>& taps) {
  std::string s;
  for (const TapPoint& t : taps) {
    char b[40];
    std::snprintf(b, sizeof b, "%c%d,%d;", t.add ? '+' : '-', int(std::lround(std::clamp(t.x, 0.f, 1.f) * 999.f)), int(std::lround(std::clamp(t.y, 0.f, 1.f) * 999.f)));
    s += b;
  }
  if (!s.empty()) s.pop_back();
  return s;
}
bool parseTapSpec(const std::string& s, std::vector<TapPoint>& out) {
  out.clear();
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '+' && s[i] != '-') return false;
    TapPoint t;
    t.add = s[i] == '+';
    int x = 0, y = 0, used = 0;
    if (std::sscanf(s.c_str() + i + 1, "%d,%d%n", &x, &y, &used) != 2 || x < 0 || y < 0 || x > 999 || y > 999) return false;
    t.x = float(x) / 999.f, t.y = float(y) / 999.f;
    out.push_back(t);
    i += std::size_t(1 + used);
    if (i < s.size()) {
      if (s[i] != ';') return false;
      ++i;
    }
    if (int(out.size()) > kMaxTaps) return false;
  }
  return !out.empty();
}
std::string makeId(const std::string& photo, const std::string& key) { return "ai:" + photo + ":" + key; }

ParsedId parseId(const std::string& id) {
  ParsedId p;
  if (id.size() < 4 + 16 + 1 || id.compare(0, 3, "ai:") != 0 || id[3 + 16] != ':') return p;
  p.photo = id.substr(3, 16);
  for (char c : p.photo)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return p;
  p.key = id.substr(3 + 16 + 1);
  const std::string& k = p.key;
  static const char* kPerson[] = {"hair", "faceskin", "bodyskin", "clothes", "person", "background"};
  for (const char* n : kPerson)
    if (k == n) p.group = "person";
  if (k == "depth") p.group = "depth";
  if (k == "sky") p.group = "sky";
  if (k.size() > 3 && k[0] == 'f' && k[1] >= '1' && k[1] <= '9' && k[2] == ':') {
    static const char* kFeat[] = {"face", "eyes", "iris", "sclera", "lips", "teeth", "brows", "undereye"};
    for (const char* n : kFeat)
      if (k.substr(3) == n) p.group = "faces", p.face = k[1] - '0', p.feature = n;
    if (p.face > kMaxFaces) p.group.clear();
  }
  if (k.compare(0, 4, "tap:") == 0 && parseTapSpec(k.substr(4), p.taps)) p.group = "tap";
  p.ok = !p.group.empty();
  return p;
}

// ---- cache format ----------------------------------------------------------------------------------------------------------------------------
// "DPL1" | u32 epoch | u32 n | n x { u8 keyLen, key, u16 w, u16 h, u32 packedLen, PackBits bytes } | u32 metaLen | meta | u32 fnv1a of everything before
namespace {
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(std::uint8_t(v >> (8 * i)));
}
void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
  o.push_back(std::uint8_t(v)), o.push_back(std::uint8_t(v >> 8));
}
std::uint32_t fnv(const std::uint8_t* p, std::size_t n) {
  std::uint32_t h = 2166136261u;
  for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
void packBits(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& o) {
  std::size_t i = 0;
  while (i < in.size()) {
    std::size_t run = 1;
    while (i + run < in.size() && in[i + run] == in[i] && run < 128) ++run;
    if (run >= 3) {
      o.push_back(std::uint8_t(257 - run));
      o.push_back(in[i]);
      i += run;
    } else {
      std::size_t lit = 0;
      while (i + lit < in.size() && lit < 128) {
        std::size_t r = 1;
        while (i + lit + r < in.size() && in[i + lit + r] == in[i + lit] && r < 3) ++r;
        if (r >= 3) break;
        ++lit;
      }
      if (lit == 0) lit = 1;
      o.push_back(std::uint8_t(lit - 1));
      o.insert(o.end(), in.begin() + long(i), in.begin() + long(i + lit));
      i += lit;
    }
  }
}
bool unpackBits(const std::uint8_t* p, std::size_t n, std::size_t outLen, std::vector<std::uint8_t>& out) {
  out.clear();
  out.reserve(outLen);
  std::size_t i = 0;
  while (i < n && out.size() < outLen) {
    const int c = p[i++];
    if (c < 128) {
      const std::size_t len = std::size_t(c) + 1;
      if (i + len > n) return false;
      out.insert(out.end(), p + i, p + i + len);
      i += len;
    } else if (c > 128) {
      if (i >= n) return false;
      out.insert(out.end(), std::size_t(257 - c), p[i++]);
    }
  }
  return out.size() == outLen && i == n;
}
}  // namespace

std::vector<std::uint8_t> packSet(const PlaneSet& s) {
  std::vector<std::uint8_t> o = {'D', 'P', 'L', '1'};
  put32(o, kEpoch);
  put32(o, std::uint32_t(s.entries.size()));
  for (const SetEntry& e : s.entries) {
    o.push_back(std::uint8_t(std::min<std::size_t>(255, e.key.size())));
    o.insert(o.end(), e.key.begin(), e.key.begin() + long(std::min<std::size_t>(255, e.key.size())));
    put16(o, std::uint32_t(e.plane.w));
    put16(o, std::uint32_t(e.plane.h));
    std::vector<std::uint8_t> pk;
    packBits(e.plane.px, pk);
    put32(o, std::uint32_t(pk.size()));
    o.insert(o.end(), pk.begin(), pk.end());
  }
  put32(o, std::uint32_t(s.meta.size()));
  o.insert(o.end(), s.meta.begin(), s.meta.end());
  put32(o, fnv(o.data(), o.size()));
  return o;
}

bool unpackSet(const std::uint8_t* d, std::size_t n, PlaneSet& out) {
  out = PlaneSet{};
  if (n < 20 || std::memcmp(d, "DPL1", 4) != 0) return false;
  std::uint32_t sum = 0;
  std::memcpy(&sum, d + n - 4, 4);
  if (sum != fnv(d, n - 4)) return false;
  std::size_t i = 4;
  auto get32 = [&](std::uint32_t& v) {
    if (i + 4 > n - 4) return false;
    std::memcpy(&v, d + i, 4);
    i += 4;
    return true;
  };
  std::uint32_t epoch = 0, cnt = 0;
  if (!get32(epoch) || epoch != std::uint32_t(kEpoch) || !get32(cnt) || cnt > 64) return false;
  for (std::uint32_t k = 0; k < cnt; ++k) {
    if (i >= n - 4) return false;
    const std::size_t kl = d[i++];
    if (i + kl + 4 > n - 4) return false;
    SetEntry e;
    e.key.assign(reinterpret_cast<const char*>(d + i), kl);
    i += kl;
    e.plane.w = d[i] | d[i + 1] << 8;
    e.plane.h = d[i + 2] | d[i + 3] << 8;
    i += 4;
    std::uint32_t pl = 0;
    if (!get32(pl) || i + pl > n - 4 || e.plane.w <= 0 || e.plane.h <= 0 || e.plane.w > 4096 || e.plane.h > 4096) return false;
    if (!unpackBits(d + i, pl, std::size_t(e.plane.w) * e.plane.h, e.plane.px)) return false;
    i += pl;
    out.entries.push_back(std::move(e));
  }
  std::uint32_t ml = 0;
  if (!get32(ml) || i + ml != n - 4) return false;
  out.meta.assign(reinterpret_cast<const char*>(d + i), ml);
  return true;
}

std::string photoIdOf(const float* lin, int w, int h) {
  std::uint64_t hs = 0xcbf29ce484222325ull ^ (std::uint64_t(w) << 32 | std::uint32_t(h));
  auto mix = [&](std::uint64_t v) {
    hs ^= v;
    hs *= 0x100000001b3ull;
    hs ^= hs >> 29;
  };
  for (int j = 0; j < 256; ++j)
    for (int i = 0; i < 256; ++i) {
      const int x = std::min(w - 1, (2 * i + 1) * w / 512), y = std::min(h - 1, (2 * j + 1) * h / 512);
      const float* p = lin + (std::size_t(y) * w + x) * 3;
      for (int c = 0; c < 3; ++c) mix(std::uint64_t(std::clamp(p[c], 0.f, 64.f) * 4096.f));
    }
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)hs);
  return b;
}

}  // namespace dapple::aimask
