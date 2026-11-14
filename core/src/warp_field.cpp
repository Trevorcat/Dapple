// The displacement field of the portrait reshaping (warp_field.h; docs/engine/pipeline-architecture.md section 45).
//
// Construction (all in isotropic photo coordinates: X = u W / L, Y = v H / L, L = long edge):
//   1. every active operator adds a smooth FORWARD displacement (where content goes) into a grid F: compactly supported kernels
//      K(s) = (1 - s^2)^3 around landmarks / pose segments, so everything far from the person is exactly zero;
//   2. background protection: the person / face operators are multiplied by w = 1 inside the person matte and a smooth decay over a
//      band delta outside it (distance on the grid); the long-legs stretch is not (it is a function of the height inside a horizontal window around
//      the main person's legs, smooth to 0 at the sides: no seam, horizontal lines stay horizontal inside the window);
//   3. fold-free: the spectral norm of grad F is limited to 0.75 (scaled down with one log line), so p -> p + F(p) is invertible;
//   4. inversion into the BACKWARD field B (p + B(p) = q with q + F(q) = p) by fixed-point iteration q <- p - F(q);
//   5. the manual push strokes are composed after the operators, dab by dab, in backward form: B'(p) = Bd(p) + B(p + Bd(p)); a restore
//      dab scales B toward 0 under the brush.
#include "warp_field.h"

#include <chrono>
#include <cmath>
#include <list>
#include <mutex>

#include "local_eval.h"

namespace dapple::warp {

namespace {

struct V {
  double x = 0, y = 0;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
inline V operator*(V a, double s) { return {a.x * s, a.y * s}; }
inline double dot(V a, V b) { return a.x * b.x + a.y * b.y; }
inline double len(V a) { return std::sqrt(dot(a, a)); }
inline V unit(V a) {
  const double l = len(a);
  return l > 1e-12 ? a * (1.0 / l) : V{0, 0};
}
inline V perp(V a) { return {-a.y, a.x}; }

// compact kernel, 1 at 0, C2 zero at 1
inline double K(double s) {
  if (s >= 1.0) return 0.0;
  if (s <= 0.0) return 1.0;
  const double t = 1.0 - s * s;
  return t * t * t;
}
// plateau: 1 for r <= r0, K decay over `w` beyond
inline double plateau(double r, double r0, double w) { return r <= r0 ? 1.0 : K((r - r0) / w); }
inline double sstep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

struct Grid {
  int gw = 0, gh = 0;
  double AX = 1, AY = 1, hX = 1, hY = 1;
  std::vector<double> f;  // 2 per node
  double X(int i) const { return i * hX; }
  double Y(int j) const { return j * hY; }
  void zero() { f.assign(std::size_t(gw) * gh * 2, 0.0); }
  // bilinear sample (isotropic position), clamped
  V at(double x, double y) const {
    double gx = std::clamp(x / hX, 0.0, double(gw - 1)), gy = std::clamp(y / hY, 0.0, double(gh - 1));
    const int i = std::min(int(gx), gw - 2), j = std::min(int(gy), gh - 2);
    const double fx = gx - i, fy = gy - j;
    const double* a = &f[2 * (std::size_t(j) * gw + i)];
    const double* c = a + 2 * std::size_t(gw);
    return {(a[0] * (1 - fx) + a[2] * fx) * (1 - fy) + (c[0] * (1 - fx) + c[2] * fx) * fy,
            (a[1] * (1 - fx) + a[3] * fx) * (1 - fy) + (c[1] * (1 - fx) + c[3] * fx) * fy};
  }
  template <class Fn>
  void splat(double x0, double y0, double x1, double y1, Fn fn) {
    const int i0 = std::max(0, int(std::floor(x0 / hX))), i1 = std::min(gw - 1, int(std::ceil(x1 / hX)));
    const int j0 = std::max(0, int(std::floor(y0 / hY))), j1 = std::min(gh - 1, int(std::ceil(y1 / hY)));
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        const V d = fn(V{X(i), Y(j)});
        if (d.x == 0 && d.y == 0) continue;
        double* p = &f[2 * (std::size_t(j) * gw + i)];
        p[0] += d.x, p[1] += d.y;
      }
  }
};

// ---- the person matte (raster plane of the store, photo-normalised) ---------------------------------------------------------------
struct Matte {
  std::shared_ptr<const local::Raster> r;
  double AX = 1, AY = 1;
  double at(V p) const {  // p isotropic
    if (!r || r->w < 2 || r->h < 2) return 1.0;
    const double x = std::clamp(p.x / AX * r->w - 0.5, 0.0, double(r->w - 1)), y = std::clamp(p.y / AY * r->h - 0.5, 0.0, double(r->h - 1));
    const int i = std::min(int(x), r->w - 2), j = std::min(int(y), r->h - 2);
    const double fx = x - i, fy = y - j;
    auto px = [&](int a, int b) { return double(r->px[std::size_t(b) * r->w + a]) / 255.0; };
    return (px(i, j) * (1 - fx) + px(i + 1, j) * fx) * (1 - fy) + (px(i, j + 1) * (1 - fx) + px(i + 1, j + 1) * fx) * fy;
  }
  // distance from p along direction d to where the matte drops below 0.5 (max `lim`)
  double edge(V p, V d, double lim) const {
    if (!r) return -1;
    const int n = 48;
    for (int k = 1; k <= n; ++k) {
      const double t = lim * k / n;
      if (at(p + d * t) < 0.5) return t;
    }
    return lim;
  }
};

// ---- faces -----------------------------------------------------------------------------------------------------------------------
struct FaceGeo {
  V P[kReshapeFacePts];
  V c, xh, yh;
  double fw = 0, fh = 0, iod = 0;
  bool ok = false;
  double lx(V p) const { return dot(p - c, xh); }
  double ly(V p) const { return dot(p - c, yh); }
};
FaceGeo faceGeo(const ReshapeFace& f, double AX, double AY) {
  FaceGeo g;
  for (int k = 0; k < kReshapeFacePts; ++k) g.P[k] = {f.pts[2 * k] * AX, f.pts[2 * k + 1] * AY};
  for (int k = 0; k < 36; ++k) g.c = g.c + g.P[k] * (1.0 / 36);
  g.xh = unit(g.P[37] - g.P[36]);
  V down = unit(g.P[18] - g.P[0]);
  g.yh = perp(g.xh);
  if (dot(g.yh, down) < 0) g.yh = g.yh * -1.0;
  g.fw = len(g.P[28] - g.P[8]);
  g.fh = len(g.P[18] - g.P[0]);
  g.iod = len(g.P[37] - g.P[36]);
  g.ok = g.fw > 1e-4 && g.fh > 1e-4 && g.iod > 1e-5 && len(g.xh) > 0.5;
  return g;
}

// 瘦脸: the jaw / cheek contour toward the face axis (horizontal in the face frame), most at the jaw angle, none at the chin / temple
void opSlim(Grid& F, const FaceGeo& g, double s) {
  static const double prof[11] = {0, 0.15, 0.35, 0.6, 0.85, 1.0, 1.0, 0.9, 0.7, 0.4, 0.15};
  struct Pt {
    V c, v;
  };
  std::vector<Pt> pts;
  const double rho = 0.28 * g.fw;
  for (int i = 8; i <= 28; ++i) {
    const int k = std::abs(i - 18);
    const double side = g.lx(g.P[i]) < 0 ? 1.0 : -1.0;  // toward the axis
    pts.push_back({g.P[i], g.xh * (side * s * 0.055 * g.fw * prof[k])});
  }
  double S0 = 0;
  for (const Pt& a : pts) {
    double sum = 0;
    for (const Pt& b : pts) sum += K(len(a.c - b.c) / rho);
    S0 = std::max(S0, sum);
  }
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const Pt& a : pts) x0 = std::min(x0, a.c.x), y0 = std::min(y0, a.c.y), x1 = std::max(x1, a.c.x), y1 = std::max(y1, a.c.y);
  F.splat(x0 - rho, y0 - rho, x1 + rho, y1 + rho, [&](V p) {
    V acc;
    for (const Pt& a : pts) {
      const double k = K(len(p - a.c) / rho);
      if (k > 0) acc = acc + a.v * k;
    }
    return acc * (1.0 / S0);
  });
}

// 小脸: uniform shrink about the face centre, plateau over the face, smooth decay beyond it
void opSmall(Grid& F, const FaceGeo& g, double s) {
  const double ax = 0.5 * g.fw, ay = 0.5 * g.fh, R = 1.6 * std::max(ax, ay);
  F.splat(g.c.x - R, g.c.y - R, g.c.x + R, g.c.y + R, [&](V p) {
    const double r = std::hypot(g.lx(p) / ax, g.ly(p) / ay);
    const double e = plateau(r, 0.9, 0.7);
    return e > 0 ? (p - g.c) * (-s * 0.07 * e) : V{};
  });
}

// 下巴: the chin along the face axis (+ longer), local to the chin
void opChin(Grid& F, const FaceGeo& g, double s) {
  const V ch = g.P[18];
  const double ax = 0.42 * g.fw, ay = 0.32 * g.fh, R = std::max(ax, ay);
  F.splat(ch.x - R, ch.y - R, ch.x + R, ch.y + R, [&](V p) {
    const V d = p - ch;
    const double k = K(std::hypot(dot(d, g.xh) / ax, dot(d, g.yh) / ay));
    return k > 0 ? g.yh * (s * 0.06 * g.fh * k) : V{};
  });
}

// 大眼: local magnification about each iris centre (an ellipse along the eye)
void opEyes(Grid& F, const FaceGeo& g, double s) {
  const double ax = 0.55 * g.iod, ay = 0.4 * g.iod, R = ax;
  for (int e = 36; e <= 37; ++e) {
    const V c = g.P[e];
    F.splat(c.x - R, c.y - R, c.x + R, c.y + R, [&](V p) {
      const V d = p - c;
      const double k = K(std::hypot(dot(d, g.xh) / ax, dot(d, g.yh) / ay));
      return k > 0 ? d * (s * 0.18 * k) : V{};
    });
  }
}

// 鼻翼: both nose wings toward the nose axis
void opNose(Grid& F, const FaceGeo& g, double s) {
  const V w1 = g.P[46], w2 = g.P[47];
  const double nw = len(w2 - w1);
  if (nw < 1e-5) return;
  const V m = (w1 + w2) * 0.5;
  const double ax = 0.5 * nw, ay = 0.6 * nw, R = ay;
  for (const V& w : {w1, w2}) {
    const V v = unit(m - w) * (s * 0.15 * 0.5 * nw);
    F.splat(w.x - R, w.y - R, w.x + R, w.y + R, [&](V p) {
      const V d = p - w;
      const double k = K(std::hypot(dot(d, g.xh) / ax, dot(d, g.yh) / ay));
      return k > 0 ? v * k : V{};
    });
  }
}

// ---- bodies ------------------------------------------------------------------------------------------------------------------------
struct BodyGeo {
  V P[kReshapeBodyPts];
  double vis[kReshapeBodyPts] = {};
  double sh = 0, hipd = 0;
  bool ok(int i) const { return vis[i] >= 0.5; }
};
BodyGeo bodyGeo(const ReshapeBody& b, double AX, double AY) {
  BodyGeo g;
  for (int k = 0; k < kReshapeBodyPts; ++k) g.P[k] = {b.pts[3 * k] * AX, b.pts[3 * k + 1] * AY}, g.vis[k] = b.pts[3 * k + 2];
  g.sh = len(g.P[11] - g.P[12]);
  g.hipd = len(g.P[23] - g.P[24]);
  return g;
}
// Half width of a limb / the torso at point c across direction n: the matte's edges on both sides when the matte is there, else `est`.
double halfWidth(const Matte* M, V c, V n, double est) {
  if (!M || !M->r || M->at(c) < 0.5) return est;
  const double a = M->edge(c, n, 2.5 * est), b = M->edge(c, n * -1.0, 2.5 * est);
  return std::clamp(0.5 * (a + b), 0.6 * est, 1.6 * est);
}
// compress a capsule a -> b toward its axis: content at the edge (half width hw) moves in by amt * hw. `side`: 0 = both edges, +1 / -1 = only the
// edge on the + / - side of the axis (n = perp(unit(b - a))), moved in by `boost` times as much: a limb pressed against the torso or the other
// leg keeps its inner edge (the neighbour is not pulled along), the outer edge does the thinning.
void limb(Grid& F, V a, V b, double hw, double amt, int side = 0, double boost = 1.0) {
  const double L = len(b - a);
  if (L < 1e-5 || hw < 1e-5) return;
  const V u = unit(b - a), n = perp(u);
  const double R = 2.0 * hw;
  F.splat(std::min(a.x, b.x) - R - 0.3 * L, std::min(a.y, b.y) - R - 0.3 * L, std::max(a.x, b.x) + R + 0.3 * L, std::max(a.y, b.y) + R + 0.3 * L,
          [&](V p) {
            const double t = dot(p - a, u) / L, q = dot(p - a, n);
            if (side * q < 0) return V{};
            const double G = t < 0.15 ? K((0.15 - t) / 0.3) : (t > 0.85 ? K((t - 0.85) / 0.3) : 1.0);
            if (G <= 0) return V{};
            const double H = plateau(std::fabs(q) / hw, 1.0, 1.0);
            return H > 0 ? n * (-amt * (side ? boost : 1.0) * q * H * G) : V{};
          });
}

void opWaist(Grid& F, const BodyGeo& g, const Matte* M, double s) {
  if (!(g.ok(11) && g.ok(12) && g.ok(23) && g.ok(24))) return;
  const V S = (g.P[11] + g.P[12]) * 0.5, Hm = (g.P[23] + g.P[24]) * 0.5;
  const double Lt = len(Hm - S);
  if (Lt < 1e-4) return;
  const V t = unit(Hm - S), n = perp(t);
  const V w = S + t * (0.75 * Lt);
  const double hw = halfWidth(M, w, n, 0.42 * std::max(g.sh, 1.4 * g.hipd));
  const double R = 2.0 * hw;
  F.splat(w.x - R - Lt, w.y - R - Lt, w.x + R + Lt, w.y + R + Lt, [&](V p) {
    const double a = dot(p - S, t) / Lt, q = dot(p - S, n);
    const double G = K(std::fabs(a - 0.75) / 0.5);
    if (G <= 0) return V{};
    const double H = plateau(std::fabs(q) / hw, 1.0, 1.0);
    return H > 0 ? n * (-s * 0.08 * q * H * G) : V{};
  });
}

// An arm hangs against the torso when its midpoint lies beside the torso (within its half width of the torso's edge): then only the outer edge moves.
void opArms(Grid& F, const BodyGeo& g, const Matte* M, double s) {
  const int seg[4][2] = {{11, 13}, {13, 15}, {12, 14}, {14, 16}};
  const bool torso = g.ok(11) && g.ok(12) && g.ok(23) && g.ok(24);
  const V S = (g.P[11] + g.P[12]) * 0.5, Hm = (g.P[23] + g.P[24]) * 0.5;
  const double Lt = torso ? len(Hm - S) : 0;
  for (int k = 0; k < 4; ++k) {
    const int a = seg[k][0], b = seg[k][1];
    if (!(g.ok(a) && g.ok(b))) continue;
    const V u = unit(g.P[b] - g.P[a]), n = perp(u);
    const double est = (k % 2 == 0 ? 0.14 : 0.11) * g.sh;
    const V mid = (g.P[a] + g.P[b]) * 0.5;
    const double hw = halfWidth(M, mid, n, est);
    int side = 0;
    if (torso && Lt > 1e-4) {
      const V t = unit(Hm - S);
      const double along = dot(mid - S, t) / Lt, lat = dot(mid - S, perp(t));
      const double tw = 0.5 * std::max(g.sh, g.hipd);  // the torso's half width
      if (along > -0.05 && along < 1.1 && std::fabs(lat) < tw + 1.2 * hw) {
        // the side of the axis that faces the torso axis gets no field
        const V toTorso = unit(S + t * dot(mid - S, t) - mid);
        side = dot(toTorso, n) > 0 ? -1 : 1;
      }
    }
    limb(F, g.P[a], g.P[b], hw, s * 0.12, side, 1.6);
  }
}

// Legs pressed together (the two midpoints closer than their widths): each leg keeps the edge that faces the other.
void opLegs(Grid& F, const BodyGeo& g, const Matte* M, double s) {
  const int seg[4][2] = {{23, 25}, {25, 27}, {24, 26}, {26, 28}};
  const double base = std::max(g.hipd, 0.5 * g.sh);
  double hwv[4] = {};
  V mid[4];
  bool ok[4];
  for (int k = 0; k < 4; ++k) {
    const int a = seg[k][0], b = seg[k][1];
    ok[k] = g.ok(a) && g.ok(b);
    if (!ok[k]) continue;
    const V u = unit(g.P[b] - g.P[a]), n = perp(u);
    mid[k] = (g.P[a] + g.P[b]) * 0.5;
    hwv[k] = halfWidth(M, mid[k], n, (k % 2 == 0 ? 0.40 : 0.27) * base);
  }
  for (int k = 0; k < 4; ++k) {
    if (!ok[k]) continue;
    const int a = seg[k][0], b = seg[k][1];
    const V n = perp(unit(g.P[b] - g.P[a]));
    int side = 0;
    const int o = k < 2 ? k + 2 : k - 2;  // the same segment of the other leg
    if (ok[o] && len(mid[k] - mid[o]) < 1.4 * (hwv[k] + hwv[o])) side = dot(mid[o] - mid[k], n) > 0 ? -1 : 1;
    limb(F, g.P[a], g.P[b], hwv[k], s * 0.10, side, 1.6);
  }
}

// Needs the shoulders and the hips: on a head-and-shoulders crop the head is most of the picture and the shrink would be far past believable.
void opHead(Grid& F, const BodyGeo& g, double s) {
  if (!(g.ok(7) && g.ok(8) && g.ok(11) && g.ok(12) && g.ok(23) && g.ok(24))) return;
  const V hc = (g.P[7] + g.P[8]) * 0.5;
  const double Rh = 0.8 * len(g.P[7] - g.P[8]);
  if (Rh < 1e-4) return;
  V down{0, 1};
  if (g.ok(11) && g.ok(12)) down = unit((g.P[11] + g.P[12]) * 0.5 - hc);
  const V nk = hc + down * Rh;
  const double R = 1.9 * Rh;
  F.splat(hc.x - R, hc.y - R, hc.x + R, hc.y + R, [&](V p) {
    const double e = plateau(len(p - hc) / Rh, 1.0, 0.9);
    return e > 0 ? (p - nk) * (-s * 0.06 * e) : V{};
  });
}

// 长腿 (main person only): a stretch in the height, applied inside a horizontal WINDOW around this person's legs: legs from the hips to the
// feet longer by up to 12 %, the ground below the feet compressed back so the frame keeps its content (exactly zero at the bottom row); nothing
// above the hips moves. Field = fy(y) * W(x): W is 1 over the legs (hips to feet, widened by the person plane where it is there) and falls to 0 over
// `fall` on both sides (smoothstep, no seam). The falloff is wide on purpose: a background line that crosses it is lifted by up to fy
// in the window and by 0 outside, so its slope is at most 1.5 * peak / fall; fall = 8 * peak (at least half the window, at most 0.30 of the long
// edge) keeps that slope at 0.19 (about 11 degrees) or less, 0.27 at the cap. Vertical lines bend by the same amount; horizontal lines stay
// horizontal inside the window. Fold-free: grad F = [[0, 0], [fy W', fy' W]] with fy' <= 0.12 and fy W' <= 0.27.
// Only for an upright body whose feet are in the picture with room below them (>= 1 % of the height below the shoes).
void opLongLegs(Grid& F, const BodyGeo& g, const Matte* M, double s) {
  if (!(g.ok(23) && g.ok(24))) return;
  if (g.ok(11) && g.ok(12)) {
    const V t = unit((g.P[23] + g.P[24]) * 0.5 - (g.P[11] + g.P[12]) * 0.5);
    if (t.y < std::cos(30.0 * 3.14159265358979 / 180.0)) return;  // not upright
  }
  const double hipY = 0.5 * (g.P[23].y + g.P[24].y);
  double footY = -1;
  for (int k = 27; k <= 32; ++k)
    if (g.ok(k)) footY = std::max(footY, g.P[k].y);
  const double B = F.AY;
  if (footY < 0) return;  // feet not visible: not a full-length picture, nothing to lengthen plausibly
  // the shoes reach below the foot landmarks (heel / toe points): the legs end 3 % of the height lower; a picture with no room below them (feet at the
  // edge) has nowhere to put longer legs and is left alone
  footY = std::min(footY + 0.03 * B, B);
  if (B - footY < 0.01 * B) return;
  if (footY - hipY < 0.05 * B) return;
  double k = s * 0.12;
  const double m = 0.06 * (footY - hipY);
  const bool ground = true;
  double c = 0;
  if (ground) {
    c = k * (footY - hipY) / (B - footY);
    if (c > 0.45) k *= 0.45 / c, c = 0.45;
  }
  // rate profile g(Y), integrated per row (8 sub-steps); the compression part is rescaled so F(B) = 0 exactly
  auto rate = [&](double y, bool pos) {
    const double up = sstep(hipY - m, hipY + m, y);
    if (!ground) return pos ? k * up : 0.0;
    const double down = sstep(footY - m, footY + m, y);
    return pos ? k * up * (1 - down) : -c * down;
  };
  const int n = F.gh;
  std::vector<double> P(n, 0), N(n, 0);
  for (int j = 1; j < n; ++j) {
    double sp = 0, sn = 0;
    for (int q = 0; q < 8; ++q) {
      const double y = F.Y(j - 1) + (q + 0.5) / 8 * F.hY;
      sp += rate(y, true), sn += rate(y, false);
    }
    P[j] = P[j - 1] + sp * F.hY / 8, N[j] = N[j - 1] + sn * F.hY / 8;
  }
  const double scale = ground && N[n - 1] < 0 ? -P[n - 1] / N[n - 1] : 0.0;
  std::vector<double> fy(n, 0.0);
  double peak = 0;
  for (int j = 0; j < n; ++j) {
    double v = P[j] + scale * N[j];
    if (j == n - 1 && ground) v = 0;
    fy[j] = v;
    peak = std::max(peak, std::fabs(v));
  }
  if (peak < 1e-9) return;
  // the window: the leg landmarks (hips to feet) plus what a skirt or coat adds (the person plane's edge on a few rows, at most one body width)
  double xl = 1e9, xr = -1e9;
  for (int q = 23; q <= 32; ++q)
    if (g.ok(q)) xl = std::min(xl, g.P[q].x), xr = std::max(xr, g.P[q].x);
  const double base = std::max(g.hipd, 0.5 * g.sh);
  double pl = 0.45 * base, pr = 0.45 * base;
  if (M && M->r) {
    for (int r = 1; r <= 5; ++r) {
      const double y = hipY + (footY - hipY) * (0.15 + 0.17 * r);
      if (M->at(V{xl, y}) >= 0.5) pl = std::max(pl, std::min(M->edge(V{xl, y}, V{-1, 0}, 1.0 * base), 1.0 * base));
      if (M->at(V{xr, y}) >= 0.5) pr = std::max(pr, std::min(M->edge(V{xr, y}, V{1, 0}, 1.0 * base), 1.0 * base));
    }
  }
  xl -= pl, xr += pr;
  const double fall = std::clamp(std::max(8.0 * peak, 0.5 * (xr - xl)), 0.03, 0.30);
  for (int j = 0; j < n; ++j) {
    if (std::fabs(fy[j]) < 1e-12) continue;
    for (int i = 0; i < F.gw; ++i) {
      const double x = F.X(i);
      const double w = x < xl ? sstep(xl - fall, xl, x) : (x > xr ? 1.0 - sstep(xr, xr + fall, x) : 1.0);
      if (w > 0) F.f[2 * (std::size_t(j) * F.gw + i) + 1] += fy[j] * w;
    }
  }
}

// ---- protection weight: 1 inside the matte, smooth decay over `delta` outside (chamfer distance on the grid) ---------------------------
std::vector<double> protection(const Grid& G, const Matte& M, double delta) {
  const int w = G.gw, h = G.gh;
  std::vector<double> dist(std::size_t(w) * h, 1e9);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i)
      if (M.at(V{G.X(i), G.Y(j)}) >= 0.5) dist[std::size_t(j) * w + i] = 0;
  const double dx = G.hX, dy = G.hY, dd = std::hypot(dx, dy);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      double& d = dist[std::size_t(j) * w + i];
      if (i > 0) d = std::min(d, dist[std::size_t(j) * w + i - 1] + dx);
      if (j > 0) {
        d = std::min(d, dist[std::size_t(j - 1) * w + i] + dy);
        if (i > 0) d = std::min(d, dist[std::size_t(j - 1) * w + i - 1] + dd);
        if (i < w - 1) d = std::min(d, dist[std::size_t(j - 1) * w + i + 1] + dd);
      }
    }
  for (int j = h - 1; j >= 0; --j)
    for (int i = w - 1; i >= 0; --i) {
      double& d = dist[std::size_t(j) * w + i];
      if (i < w - 1) d = std::min(d, dist[std::size_t(j) * w + i + 1] + dx);
      if (j < h - 1) {
        d = std::min(d, dist[std::size_t(j + 1) * w + i] + dy);
        if (i < w - 1) d = std::min(d, dist[std::size_t(j + 1) * w + i + 1] + dd);
        if (i > 0) d = std::min(d, dist[std::size_t(j + 1) * w + i - 1] + dd);
      }
    }
  for (double& d : dist) d = 1.0 - sstep(0.0, delta, d);
  return dist;
}

// spectral norm of grad F at every node, max
double maxGradient(const Grid& G) {
  double m = 0;
  for (int j = 0; j < G.gh; ++j)
    for (int i = 0; i < G.gw; ++i) {
      const int i0 = std::max(0, i - 1), i1 = std::min(G.gw - 1, i + 1), j0 = std::max(0, j - 1), j1 = std::min(G.gh - 1, j + 1);
      const double* a = &G.f[2 * (std::size_t(j) * G.gw + i0)];
      const double* b = &G.f[2 * (std::size_t(j) * G.gw + i1)];
      const double* c = &G.f[2 * (std::size_t(j0) * G.gw + i)];
      const double* d = &G.f[2 * (std::size_t(j1) * G.gw + i)];
      if (a[0] == b[0] && a[1] == b[1] && c[0] == d[0] && c[1] == d[1]) continue;
      const double ex = (i1 - i0) * G.hX, ey = (j1 - j0) * G.hY;
      const double J00 = (b[0] - a[0]) / ex, J10 = (b[1] - a[1]) / ex, J01 = (d[0] - c[0]) / ey, J11 = (d[1] - c[1]) / ey;
      // largest singular value of [J00 J01; J10 J11]
      const double p = J00 * J00 + J10 * J10, q = J01 * J01 + J11 * J11, r = J00 * J01 + J10 * J11;
      const double s = 0.5 * (p + q) + std::sqrt(0.25 * (p - q) * (p - q) + r * r);
      m = std::max(m, std::sqrt(s));
    }
  return m;
}

// ---- push strokes, composed in backward form ---------------------------------------------------------------------------------------
void composePush(Grid& B, const std::vector<ReshapePush>& strokes) {
  std::vector<double> patch;
  for (const ReshapePush& s : strokes) {
    const double R = s.r;
    std::vector<V> pts;
    for (std::size_t k = 0; k + 1 < s.pts.size(); k += 2) pts.push_back({s.pts[k] * B.AX, s.pts[k + 1] * B.AY});
    if (pts.empty()) continue;
    if (s.restore) {
      // dabs every 0.25 R along the stroke: B *= 1 - f K, f limited so that the scaled field stays fold-free
      std::vector<V> dabs{pts[0]};
      for (std::size_t k = 1; k < pts.size(); ++k) {
        const V a = pts[k - 1], b = pts[k];
        const int n = std::max(1, int(std::ceil(len(b - a) / (0.25 * R))));
        for (int q = 1; q <= n; ++q) dabs.push_back(a + (b - a) * (double(q) / n));
      }
      for (const V& c : dabs) {
        double mx = 0;
        const int i0 = std::max(0, int(std::floor((c.x - R) / B.hX))), i1 = std::min(B.gw - 1, int(std::ceil((c.x + R) / B.hX)));
        const int j0 = std::max(0, int(std::floor((c.y - R) / B.hY))), j1 = std::min(B.gh - 1, int(std::ceil((c.y + R) / B.hY)));
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) {
            const double* p = &B.f[2 * (std::size_t(j) * B.gw + i)];
            mx = std::max(mx, std::hypot(p[0], p[1]));
          }
        if (mx <= 0) continue;
        const double fl = std::min(0.5, 0.35 * R / (1.72 * mx));
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) {
            const double k = K(std::hypot(B.X(i) - c.x, B.Y(j) - c.y) / R);
            if (k <= 0) continue;
            double* p = &B.f[2 * (std::size_t(j) * B.gw + i)];
            const double m = 1.0 - fl * k;
            p[0] *= m, p[1] *= m;
            if (std::fabs(p[0]) < 1e-12) p[0] = 0;
            if (std::fabs(p[1]) < 1e-12) p[1] = 0;
          }
      }
      continue;
    }
    if (pts.size() < 2) continue;
    // forward dabs: content around a moves by v (|v| <= 0.2 R: each dab fold-free)
    for (std::size_t k = 1; k < pts.size(); ++k) {
      const V a0 = pts[k - 1], b0 = pts[k];
      const int n = std::max(1, int(std::ceil(len(b0 - a0) / (0.2 * R))));
      for (int q = 0; q < n; ++q) {
        const V a = a0 + (b0 - a0) * (double(q) / n);
        const V v = (b0 - a0) * (1.0 / n);
        const double ext = R + len(v);
        const int i0 = std::max(0, int(std::floor((a.x - ext) / B.hX))), i1 = std::min(B.gw - 1, int(std::ceil((a.x + ext) / B.hX)));
        const int j0 = std::max(0, int(std::floor((a.y - ext) / B.hY))), j1 = std::min(B.gh - 1, int(std::ceil((a.y + ext) / B.hY)));
        // the old field around the dab (the sample points stay inside [i0 - 1, i1 + 1])
        const int pi0 = std::max(0, i0 - 2), pi1 = std::min(B.gw - 1, i1 + 2), pj0 = std::max(0, j0 - 2), pj1 = std::min(B.gh - 1, j1 + 2);
        const int pw = pi1 - pi0 + 1;
        patch.resize(std::size_t(pw) * (pj1 - pj0 + 1) * 2);
        for (int j = pj0; j <= pj1; ++j)
          std::copy(&B.f[2 * (std::size_t(j) * B.gw + pi0)], &B.f[2 * (std::size_t(j) * B.gw + pi1) + 2], &patch[2 * std::size_t(j - pj0) * pw]);
        auto old = [&](V p) {
          const double gx = std::clamp(p.x / B.hX, double(pi0), double(pi1)), gy = std::clamp(p.y / B.hY, double(pj0), double(pj1));
          const int i = std::min(int(gx), std::max(pi0, pi1 - 1)), j = std::min(int(gy), std::max(pj0, pj1 - 1));
          const double fx = gx - i, fy = gy - j;
          const int ii = std::min(i + 1, pi1), jj = std::min(j + 1, pj1);
          auto at = [&](int x, int y, int c) { return patch[2 * (std::size_t(y - pj0) * pw + (x - pi0)) + c]; };
          return V{(at(i, j, 0) * (1 - fx) + at(ii, j, 0) * fx) * (1 - fy) + (at(i, jj, 0) * (1 - fx) + at(ii, jj, 0) * fx) * fy,
                   (at(i, j, 1) * (1 - fx) + at(ii, j, 1) * fx) * (1 - fy) + (at(i, jj, 1) * (1 - fx) + at(ii, jj, 1) * fx) * fy};
        };
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) {
            const V p{B.X(i), B.Y(j)};
            if (len(p - a) >= ext) continue;
            V qq = p;
            for (int it = 0; it < 6; ++it) qq = p - v * K(len(qq - a) / R);
            if (qq.x == p.x && qq.y == p.y) continue;
            const V nb = (qq - p) + old(qq);
            double* d = &B.f[2 * (std::size_t(j) * B.gw + i)];
            d[0] = nb.x, d[1] = nb.y;
          }
      }
    }
  }
}

// ---- cache --------------------------------------------------------------------------------------------------------------------------
std::mutex g_mu;
std::list<std::shared_ptr<const Field>> g_cache;
Counters g_counters;

}  // namespace

Map makeMap(const Field& f, int srcW, int srcH, int photoW, int photoH, int x0, int y0) {
  (void)srcW, (void)srcH;
  Map m;
  m.ax = float(f.gw - 1) / float(photoW);
  m.bx = float(x0) * m.ax;
  m.ay = float(f.gh - 1) / float(photoH);
  m.by = float(y0) * m.ay;
  m.lpx = float(std::max(photoW, photoH));
  return m;
}

Counters counters() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_counters;
}
void resetCache() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache.clear();
}

std::shared_ptr<const Field> bind(const ReshapeParams& r, int photoW, int photoH, bool interactive) {
  if (!r.active() || photoW < 2 || photoH < 2) return nullptr;
  const int N = interactive ? kGridInteractive : kGridExact;
  const double L = std::max(photoW, photoH);
  Grid G;
  G.AX = photoW / L, G.AY = photoH / L;
  G.gw = std::max(2, int(std::lround(N * G.AX)) + 1);
  G.gh = std::max(2, int(std::lround(N * G.AY)) + 1);
  G.hX = G.AX / (G.gw - 1), G.hY = G.AY / (G.gh - 1);
  std::uint64_t key = reshapeHash(r);
  key = (key ^ std::uint64_t(G.gw) * 0x9E3779B97F4A7C15ull) * 1099511628211ull;
  key = (key ^ std::uint64_t(G.gh) * 0xC2B2AE3D27D4EB4Full) * 1099511628211ull;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
      if ((*it)->id == key) {
        auto f = *it;
        g_cache.erase(it);
        g_cache.push_front(f);
        ++g_counters.hits;
        return f;
      }
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto out = std::make_shared<Field>();
  out->gw = G.gw, out->gh = G.gh, out->photoW = photoW, out->photoH = photoH, out->id = key;

  Matte M;
  M.AX = G.AX, M.AY = G.AY;
  if (!r.matte.empty()) {
    M.r = local::rasterGet(r.matte);
    if (!M.r) {
      out->matteMissing = true;
      local::logLine("reshape: person plane '" + r.matte.substr(0, 48) + "' is missing: no background protection");
    }
  }
  const Matte* Mp = M.r ? &M : nullptr;  // reset below when it does not cover the people

  // 1. forward field of the protected operators
  // the matte must cover the people it protects: every active face centre and the visible torso points of every active body. A segmenter
  // that missed the person (black-and-white photo, unusual pose) would otherwise cancel the operators: then no protection, logged.
  std::vector<char> bodySkip(r.bodies.size(), 0);
  if (Mp) {
    int checked = 0, inside = 0;
    for (const ReshapeFace& f : r.faces)
      if (f.active()) {
        const FaceGeo g = faceGeo(f, G.AX, G.AY);
        ++checked, inside += M.at(g.c) >= 0.5;
      }
    // a body the person plane does not cover (a segmenter that missed it: black-and-white photos, odd poses) is not warped: its operators would
    // bend the background behind it with nothing to confine them
    for (std::size_t bi = 0; bi < r.bodies.size(); ++bi) {
      const ReshapeBody& b = r.bodies[bi];
      if (b.active() && (b.waist || b.arms || b.legs || b.head)) {
        const BodyGeo g = bodyGeo(b, G.AX, G.AY);
        int n = 0, in = 0;
        for (int k : {0, 11, 12, 23, 24})
          if (g.ok(k)) ++n, in += M.at(g.P[k]) >= 0.5;
        if (n > 0 && 2 * in < n) {
          bodySkip[bi] = 1;
          out->matteMissing = true;
          local::logLine("reshape: the person plane does not cover the posed body: its operators are off");
        }
      }
    }
    if (inside < checked) {
      out->matteMissing = true;
      Mp = nullptr;
      local::logLine("reshape: the person plane does not cover the reshaped person: no background protection");
    }
  }
  Grid F = G;
  F.zero();
  double scale = 0;
  for (const ReshapeFace& f : r.faces) {
    if (!f.active()) continue;
    const FaceGeo g = faceGeo(f, G.AX, G.AY);
    if (!g.ok) continue;
    if (f.slim) opSlim(F, g, f.slim / 100.0);
    if (f.small) opSmall(F, g, f.small / 100.0);
    if (f.chin) opChin(F, g, f.chin / 100.0);
    if (f.eyes) opEyes(F, g, f.eyes / 100.0);
    if (f.nose) opNose(F, g, f.nose / 100.0);
    scale = std::max(scale, 0.15 * g.fw);
  }
  bool band = false;
  for (std::size_t bi = 0; bi < r.bodies.size(); ++bi) {
    const ReshapeBody& b = r.bodies[bi];
    if (!b.active()) continue;
    const BodyGeo g = bodyGeo(b, G.AX, G.AY);
    if (!bodySkip[bi]) {
      if (b.waist) opWaist(F, g, Mp, b.waist / 100.0);
      if (b.arms) opArms(F, g, Mp, b.arms / 100.0);
      if (b.legs) opLegs(F, g, Mp, b.legs / 100.0);
      if (b.head) opHead(F, g, b.head / 100.0);
      if (b.waist || b.arms || b.legs || b.head) scale = std::max(scale, 0.25 * g.sh);
    }
    band = band || b.longLegs != 0;
  }
  // 2. background protection
  if (Mp && scale > 0) {
    const double delta = std::clamp(scale, 0.006, 0.05);
    const std::vector<double> w = protection(G, M, delta);
    for (std::size_t k = 0; k < w.size(); ++k) F.f[2 * k] *= w[k], F.f[2 * k + 1] *= w[k];
    out->delta = float(delta);
    out->matteUsed = true;
  }
  if (band)
    for (const ReshapeBody& b : r.bodies)
      if (b.active() && b.longLegs) opLongLegs(F, bodyGeo(b, G.AX, G.AY), M.r ? &M : nullptr, b.longLegs / 100.0);
  // 3. fold-free
  double gmax = maxGradient(F);
  if (gmax > 0.75) {
    const double k = 0.75 / gmax;
    for (double& v : F.f) v *= k;
    out->gradLimited = true;
    local::logLine("reshape: operators limited to stay fold-free (gradient " + std::to_string(gmax) + ")");
    gmax = 0.75;
  }
  out->maxGrad = float(gmax);
  // 4. inversion
  Grid B = G;
  B.zero();
  for (int j = 0; j < G.gh; ++j)
    for (int i = 0; i < G.gw; ++i) {
      const double* fp = &F.f[2 * (std::size_t(j) * G.gw + i)];
      const V p{G.X(i), G.Y(j)};
      V q = p - V{fp[0], fp[1]};
      if (q.x == p.x && q.y == p.y) continue;
      for (int it = 0; it < 12; ++it) {
        const V nq = p - F.at(q.x, q.y);
        const bool done = std::fabs(nq.x - q.x) + std::fabs(nq.y - q.y) < 1e-9;
        q = nq;
        if (done) break;
      }
      double* d = &B.f[2 * (std::size_t(j) * G.gw + i)];
      d[0] = q.x - p.x, d[1] = q.y - p.y;
    }
  // 5. manual strokes
  if (!r.push.empty()) composePush(B, r.push);

  out->d.resize(B.f.size());
  double mx = 0;
  for (std::size_t k = 0; k < B.f.size(); k += 2) {
    out->d[k] = float(B.f[k]), out->d[k + 1] = float(B.f[k + 1]);
    mx = std::max(mx, std::hypot(B.f[k], B.f[k + 1]));
  }
  out->maxDisp = float(mx);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ++g_counters.builds;
    g_counters.lastBuildMs = ms;
    g_cache.push_front(out);
    std::size_t bytes = 0;
    int n = 0;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      bytes += (*it)->bytes();
      if (++n > 8 || bytes > (64u << 20)) it = g_cache.erase(it);
      else ++it;
    }
  }
  return out;
}

void sampleUV(const Field& f, double u, double v, double& dx, double& dy) {
  double gx = std::clamp(u * (f.gw - 1), 0.0, double(f.gw - 1)), gy = std::clamp(v * (f.gh - 1), 0.0, double(f.gh - 1));
  const int i = std::min(int(gx), f.gw - 2), j = std::min(int(gy), f.gh - 2);
  const double fx = gx - i, fy = gy - j;
  const float* a = f.d.data() + 2 * (std::size_t(j) * f.gw + i);
  const float* c = a + 2 * std::size_t(f.gw);
  dx = (a[0] * (1 - fx) + a[2] * fx) * (1 - fy) + (c[0] * (1 - fx) + c[2] * fx) * fy;
  dy = (a[1] * (1 - fx) + a[3] * fx) * (1 - fy) + (c[1] * (1 - fx) + c[3] * fx) * fy;
}

void backwardUV(const Field& f, double u, double v, double& uo, double& vo) {
  const double L = std::max(f.photoW, f.photoH);
  double dx, dy;
  sampleUV(f, u, v, dx, dy);
  uo = u + dx * L / f.photoW;
  vo = v + dy * L / f.photoH;
}

void forwardUV(const Field& f, double u, double v, double& uo, double& vo) {
  // solve p + B(p) = q by damped Newton steps (a composed push field can compress content more than a plain fixed-point iteration converges on)
  auto G = [&](double pu, double pv, double& gx, double& gy) {
    double a, b;
    backwardUV(f, pu, pv, a, b);
    gx = a - u, gy = b - v;
  };
  double dx, dy;
  sampleUV(f, u, v, dx, dy);
  const double L = std::max(f.photoW, f.photoH);
  double pu = u - dx * L / f.photoW, pv = v - dy * L / f.photoH;
  double gx, gy;
  G(pu, pv, gx, gy);
  const double eu = 0.5 / (f.gw - 1), ev = 0.5 / (f.gh - 1);
  for (int it = 0; it < 40 && std::hypot(gx, gy) > 1e-11; ++it) {
    double ax, ay, bx, by;
    G(pu + eu, pv, ax, ay);
    G(pu, pv + ev, bx, by);
    const double j00 = (ax - gx) / eu, j10 = (ay - gy) / eu, j01 = (bx - gx) / ev, j11 = (by - gy) / ev;
    const double det = j00 * j11 - j01 * j10;
    double su, sv;
    if (std::fabs(det) > 1e-9) su = (j11 * gx - j01 * gy) / det, sv = (-j10 * gx + j00 * gy) / det;
    else su = gx, sv = gy;
    double t = 1.0;
    const double r0 = std::hypot(gx, gy);
    for (int k = 0; k < 12; ++k, t *= 0.5) {
      double nx, ny;
      G(pu - t * su, pv - t * sv, nx, ny);
      if (std::hypot(nx, ny) < r0 || k == 11) {
        pu -= t * su, pv -= t * sv, gx = nx, gy = ny;
        break;
      }
    }
  }
  uo = pu, vo = pv;
}

}  // namespace dapple::warp
