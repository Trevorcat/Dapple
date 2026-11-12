#include "decoders/dng_opcodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "parallel.h"

namespace dapple::dng {

namespace {

struct Reader {
  const std::uint8_t* p;
  std::size_t n, pos = 0;
  bool ok = true;
  bool need(std::size_t k) {
    if (pos + k > n) ok = false;
    return ok;
  }
  std::uint32_t u32() {
    if (!need(4)) return 0;
    const std::uint32_t v = (std::uint32_t(p[pos]) << 24) | (std::uint32_t(p[pos + 1]) << 16) | (std::uint32_t(p[pos + 2]) << 8) | p[pos + 3];
    pos += 4;
    return v;
  }
  int i32() { return int(std::int32_t(u32())); }
  float f32() {
    const std::uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  double f64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[pos + i];
    pos += 8;
    double d;
    std::memcpy(&d, &v, 8);
    return d;
  }
};

const char* opName(std::uint32_t id) {
  switch (id) {
    case kWarpRectilinear: return "WarpRectilinear";
    case kWarpFisheye: return "WarpFisheye";
    case kFixVignetteRadial: return "FixVignetteRadial";
    case kFixBadPixelsConstant: return "FixBadPixelsConstant";
    case kFixBadPixelsList: return "FixBadPixelsList";
    case kTrimBounds: return "TrimBounds";
    case kMapTable: return "MapTable";
    case kMapPolynomial: return "MapPolynomial";
    case kGainMap: return "GainMap";
    case kDeltaPerRow: return "DeltaPerRow";
    case kDeltaPerColumn: return "DeltaPerColumn";
    case kScalePerRow: return "ScalePerRow";
    case kScalePerColumn: return "ScalePerColumn";
    case kWarpRectilinear2: return "WarpRectilinear2";
    default: return "Opcode?";
  }
}

// DNG 1.6 chapter 7: the opcode immediately after an optional WarpRectilinear2 is skipped when it is WarpRectilinear / WarpFisheye
// (a reader that understands Rectilinear2 prefers it). This reader does not implement Rectilinear2, so the rule never fires and the
// plain WarpRectilinear that writers add behind it is the one applied.

void note(std::vector<std::string>& v, const std::string& s) { v.push_back(s); }

std::string countedNames(const std::vector<std::string>& v) {
  std::string out;
  std::vector<std::pair<std::string, int>> c;
  for (auto& s : v) {
    auto it = std::find_if(c.begin(), c.end(), [&](auto& e) { return e.first == s; });
    if (it == c.end()) c.push_back({s, 1});
    else ++it->second;
  }
  for (auto& e : c) {
    if (!out.empty()) out += ", ";
    out += e.first;
    if (e.second > 1) out += " x" + std::to_string(e.second);
  }
  return out;
}

// farthest-pixel distance m of spec chapter 7 for an optical centre (cx, cy) in pixel coordinates of a w x h image
double farthest(double cx, double cy, int w, int h) {
  const double mx = std::max(std::fabs(0.0 - cx), std::fabs(double(w - 1) - cx));
  const double my = std::max(std::fabs(0.0 - cy), std::fabs(double(h - 1) - cy));
  return std::sqrt(mx * mx + my * my);
}

}  // namespace

OpcodeList parseOpcodeList(const void* data, std::size_t len) {
  OpcodeList out;
  if (!data || len < 4) {
    if (len != 0) out.error = "opcode list shorter than its count";
    return out;
  }
  Reader r{static_cast<const std::uint8_t*>(data), len};
  const std::uint32_t count = r.u32();
  for (std::uint32_t i = 0; i < count; ++i) {
    Opcode op;
    op.id = r.u32();
    op.version = r.u32();
    op.flags = r.u32();
    const std::uint32_t sz = r.u32();
    if (!r.ok || sz > r.n - r.pos) {
      out.error = "opcode " + std::to_string(i) + " truncated";
      return out;
    }
    op.params.assign(r.p + r.pos, r.p + r.pos + sz);
    r.pos += sz;
    out.ops.push_back(std::move(op));
  }
  return out;
}

bool parseGainMap(const Opcode& op, GainMap& g) {
  Reader r{op.params.data(), op.params.size()};
  g.top = r.i32(), g.left = r.i32(), g.bottom = r.i32(), g.right = r.i32();
  g.plane = r.i32(), g.planes = r.i32(), g.rowPitch = r.i32(), g.colPitch = r.i32();
  g.pointsV = r.i32(), g.pointsH = r.i32();
  g.spacingV = r.f64(), g.spacingH = r.f64(), g.originV = r.f64(), g.originH = r.f64();
  g.mapPlanes = r.i32();
  if (!r.ok || g.pointsV < 1 || g.pointsH < 1 || g.mapPlanes < 1 || g.planes < 1 || g.rowPitch < 1 || g.colPitch < 1 || g.plane < 0 ||
      g.bottom <= g.top || g.right <= g.left || g.pointsV > 4096 || g.pointsH > 4096 || g.mapPlanes > 8)
    return false;
  const std::size_t n = std::size_t(g.pointsV) * g.pointsH * g.mapPlanes;
  if (n * 4 > r.n - r.pos) return false;
  g.gain.resize(n);
  for (std::size_t i = 0; i < n; ++i) g.gain[i] = r.f32();
  return r.ok;
}

bool isIdentityGainList(const OpcodeList& list) {
  if (list.ops.empty()) return false;
  for (const Opcode& op : list.ops) {
    GainMap g;
    if (op.id != kGainMap || !parseGainMap(op, g)) return false;
    for (float v : g.gain)
      if (v != 1.f) return false;
  }
  return true;
}

float GainMap::at(int y, int x, int w, int h, int mp) const {
  mp = std::min(mp, mapPlanes - 1);
  auto idx = [&](double rel, double origin, double spacing, int pts, int& i0, int& i1, float& f) {
    double t = spacing > 0 ? (rel - origin) / spacing : 0.0;
    t = std::clamp(t, 0.0, double(pts - 1));
    i0 = int(std::floor(t));
    i1 = std::min(i0 + 1, pts - 1);
    f = float(t - i0);
  };
  int v0, v1, h0, h1;
  float fv, fh;
  idx(h > 1 ? double(y) / double(h - 1) : 0.0, originV, spacingV, pointsV, v0, v1, fv);
  idx(w > 1 ? double(x) / double(w - 1) : 0.0, originH, spacingH, pointsH, h0, h1, fh);
  auto g = [&](int v, int hh) { return gain[(std::size_t(v) * pointsH + hh) * mapPlanes + mp]; };
  const float a = g(v0, h0) + (g(v0, h1) - g(v0, h0)) * fh;
  const float b = g(v1, h0) + (g(v1, h1) - g(v1, h0)) * fh;
  return a + (b - a) * fv;
}

bool parseFixVignette(const Opcode& op, FixVignette& v) {
  Reader r{op.params.data(), op.params.size()};
  for (double& k : v.k) k = r.f64();
  v.cx = r.f64(), v.cy = r.f64();
  return r.ok;
}

float FixVignette::at(int x, int y, int w, int h) const {
  const double cxp = cx * double(w - 1), cyp = cy * double(h - 1), m = farthest(cxp, cyp, w, h);
  if (m <= 0) return 1.f;
  const double dx = double(x) - cxp, dy = double(y) - cyp, r2 = (dx * dx + dy * dy) / (m * m);
  return float(1.0 + r2 * (k[0] + r2 * (k[1] + r2 * (k[2] + r2 * (k[3] + r2 * k[4])))));
}

bool parseWarpRect(const Opcode& op, WarpRect& w, int imagePlanes) {
  Reader r{op.params.data(), op.params.size()};
  const int n = r.i32();
  if (!r.ok || n < 1 || n > 3 || (n != 1 && n != imagePlanes)) return false;
  w.sets = n;
  for (int i = 0; i < n; ++i) {
    for (int k = 0; k < 4; ++k) w.kr[i][k] = r.f64();
    for (int k = 0; k < 2; ++k) w.kt[i][k] = r.f64();
  }
  w.cx = r.f64(), w.cy = r.f64();
  return r.ok;
}

void WarpRect::source(int s, int w, int h, double x, double y, double& sx, double& sy) const {
  s = std::min(s, sets - 1);
  const double cxp = cx * double(w - 1), cyp = cy * double(h - 1), m = farthest(cxp, cyp, w, h);
  if (m <= 0) {
    sx = x, sy = y;
    return;
  }
  const double dx = (x - cxp) / m, dy = (y - cyp) / m, r2 = dx * dx + dy * dy;
  const double f = kr[s][0] + r2 * (kr[s][1] + r2 * (kr[s][2] + r2 * kr[s][3]));
  const double dxt = kt[s][0] * (2.0 * dx * dy) + kt[s][1] * (r2 + 2.0 * dx * dx);
  const double dyt = kt[s][1] * (2.0 * dx * dy) + kt[s][0] * (r2 + 2.0 * dy * dy);
  sx = cxp + m * (f * dx + dxt);
  sy = cyp + m * (f * dy + dyt);
}

std::string ApplyReport::summary() const {
  std::string s = countedNames(applied);
  if (!skipped.empty()) s += (s.empty() ? "" : "; ") + std::string("skipped: ") + countedNames(skipped);
  return s;
}

void applyMosaic(const OpcodeList& list, MosaicView& v, ApplyReport& rep) {
  if (!v.px || v.width <= 0 || v.height <= 0) return;
  auto blackAt = [&](int r, int c) { return v.black[std::size_t(r % v.bpr) * v.bpc + std::size_t(c % v.bpc)]; };
  // multiplies the lattice rows r0, r0 + rp, ... < r1 and columns c0, c0 + cp, ... < c1 (linearised value, black kept)
  auto scale = [&](int r0, int r1, int rp, int c0, int c1, int cp, auto&& gainFn) {
    parallelRows(r1 - r0, [&](int k) {
      if (k % rp != 0) return;
      const int r = r0 + k;
      std::uint16_t* row = v.px + (std::size_t(v.top) + r) * v.pitch + v.left;
      for (int c = c0; c < c1; c += cp) {
        const float b = blackAt(r, c), val = float(row[c]);
        row[c] = std::uint16_t(std::clamp(b + (val - b) * gainFn(r, c) + 0.5f, 0.f, v.maxValue));
      }
    });
  };
  for (const Opcode& op : list.ops) {
    if (op.id == kGainMap) {
      GainMap g;
      if (!parseGainMap(op, g) || g.plane != 0) {
        note(rep.skipped, "GainMap");
        continue;
      }
      // the lattice starts at (Top, Left); clamp its first row / column into the image keeping the pitch phase
      auto firstIn = [](int start, int pitch) { return start >= 0 ? start : start + (-start + pitch - 1) / pitch * pitch; };
      const int rs = firstIn(g.top, g.rowPitch), cs = firstIn(g.left, g.colPitch);
      const int r1 = std::min(g.bottom, v.height), c1 = std::min(g.right, v.width);
      if (rs < r1 && cs < c1) scale(rs, r1, g.rowPitch, cs, c1, g.colPitch, [&](int r, int c) { return g.at(r, c, v.width, v.height, 0); });
      rep.applied.push_back("GainMap");
    } else if (op.id == kFixVignetteRadial) {
      FixVignette f;
      if (!parseFixVignette(op, f)) {
        note(rep.skipped, "FixVignetteRadial");
        continue;
      }
      scale(0, v.height, 1, 0, v.width, 1, [&](int r, int c) { return f.at(c, r, v.width, v.height); });
      rep.applied.push_back("FixVignetteRadial");
    } else if (op.id == kWarpRectilinear || op.id == kWarpFisheye || op.id == kWarpRectilinear2) {
      note(rep.skipped, std::string(opName(op.id)) + " (needs the demosaiced image)");
    } else {
      note(rep.skipped, opName(op.id));
    }
  }
}

namespace {
inline void catmull(float t, float w[4]) {
  const float t2 = t * t, t3 = t2 * t;
  w[0] = -0.5f * t3 + t2 - 0.5f * t;
  w[1] = 1.5f * t3 - 2.5f * t2 + 1.f;
  w[2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
  w[3] = 0.5f * t3 - 0.5f * t2;
}
}  // namespace

ImageF warpRectilinear(const ImageF& src, const WarpRect& wp) {
  const int W = src.width, H = src.height;
  ImageF dst(W, H);
  if (W < 4 || H < 4) {
    dst.data = src.data;
    return dst;
  }
  parallelRows(H, [&](int y) {
    float* out = dst.row(y);
    for (int x = 0; x < W; ++x) {
      for (int c = 0; c < 3; ++c) {
        double sx, sy;
        wp.source(c, W, H, double(x), double(y), sx, sy);
        sx = std::clamp(sx, 0.0, double(W - 1));  // outside the source: replicate the edge (DefaultCrop removes it)
        sy = std::clamp(sy, 0.0, double(H - 1));
        const int ix = std::min(int(std::floor(sx)), W - 2), iy = std::min(int(std::floor(sy)), H - 2);
        const float fx = float(sx - ix), fy = float(sy - iy);
        if (fx < 1e-6f && fy < 1e-6f) {
          out[x * 3 + c] = src.data[(std::size_t(iy) * W + ix) * 3 + c];
          continue;
        }
        float wx[4], wy[4];
        catmull(fx, wx);
        catmull(fy, wy);
        float acc = 0.f, lo = 1e30f, hi = -1e30f;
        for (int j = 0; j < 4; ++j) {
          const int yy = std::clamp(iy - 1 + j, 0, H - 1);
          const float* row = src.data.data() + std::size_t(yy) * W * 3 + c;
          float rowAcc = 0.f;
          for (int i = 0; i < 4; ++i) {
            const int xx = std::clamp(ix - 1 + i, 0, W - 1);
            const float p = row[xx * 3];
            rowAcc += wx[i] * p;
            if (j >= 1 && j <= 2 && i >= 1 && i <= 2) lo = std::min(lo, p), hi = std::max(hi, p);
          }
          acc += wy[j] * rowAcc;
        }
        out[x * 3 + c] = std::clamp(acc, lo, hi);  // no overshoot / negative lobes: clamp to the 2x2 neighbourhood
      }
    }
  });
  return dst;
}

ImageF cropImage(const ImageF& src, int x0, int y0, int w, int h) {
  x0 = std::clamp(x0, 0, src.width - 1), y0 = std::clamp(y0, 0, src.height - 1);
  w = std::clamp(w, 1, src.width - x0), h = std::clamp(h, 1, src.height - y0);
  ImageF out(w, h);
  for (int y = 0; y < h; ++y) std::memcpy(out.row(y), src.row(y0 + y) + std::size_t(x0) * 3, std::size_t(w) * 3 * sizeof(float));
  return out;
}

void applyImage(const OpcodeList& list, ImageF& img, ApplyReport& rep) {
  if (img.empty()) return;
  for (const Opcode& op : list.ops) {
    if (op.id == kWarpRectilinear) {
      WarpRect w;
      if (!parseWarpRect(op, w)) {
        note(rep.skipped, "WarpRectilinear");
        continue;
      }
      img = warpRectilinear(img, w);
      rep.applied.push_back("WarpRectilinear");
    } else if (op.id == kFixVignetteRadial) {
      FixVignette f;
      if (!parseFixVignette(op, f)) {
        note(rep.skipped, "FixVignetteRadial");
        continue;
      }
      parallelRows(img.height, [&](int y) {
        float* r = img.row(y);
        for (int x = 0; x < img.width; ++x) {
          const float g = f.at(x, y, img.width, img.height);
          r[x * 3] *= g, r[x * 3 + 1] *= g, r[x * 3 + 2] *= g;
        }
      });
      rep.applied.push_back("FixVignetteRadial");
    } else if (op.id == kGainMap) {
      GainMap g;
      if (!parseGainMap(op, g)) {
        note(rep.skipped, "GainMap");
        continue;
      }
      const int r1 = std::min(g.bottom, img.height), c1 = std::min(g.right, img.width);
      parallelRows(img.height, [&](int y) {
        if (y < g.top || y >= r1 || (y - g.top) % g.rowPitch != 0) return;
        float* r = img.row(y);
        for (int x = std::max(g.left, 0); x < c1; x += g.colPitch)
          for (int p = g.plane; p < std::min(3, g.plane + g.planes); ++p) r[x * 3 + p] *= g.at(y, x, img.width, img.height, p - g.plane);
      });
      rep.applied.push_back("GainMap");
    } else {
      note(rep.skipped, opName(op.id));
    }
  }
}

}  // namespace dapple::dng
