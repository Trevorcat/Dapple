// Half-resolution X-Trans reconstruction (docs/engine/pipeline-architecture.md 32.12). Included by demosaic_cpu.cpp inside namespace dapple::demosaic, after the
// tile runner. The GPU kernel core/engine/shaders/demosaic_half.comp does the same arithmetic on the same tables.
//
// Output pixel (x, y) is centred between sensor pixels 2x, 2x+1 (and 2y, 2y+1). Its 8 x 8 window is the sensor samples 2x-3 .. 2x+4: symmetric around the output
// pixel, and because 2x mod 6 is 0, 2 or 4 the pattern seen by the window has 9 phases. For each phase and each colour c in {R, G, B} the taps of that colour get
// weights designed by least squares so that the sum of w_k exp(2 pi i f . (p_k - centre)) follows a target response H(f) on a grid of the whole frequency plane
// (real part: the target, imaginary part: zero = no phase shift), with a ridge term against noise gain and the constraint sum w = 1 (a flat field is exact).
//   Gh: green taps, target = flat to gF0 = 0.25 cycles / sensor pixel (the output Nyquist) then rolling to zero at 0.5, with a gain peaking up to 1.6 at the output Nyquist
//       (gBoost): no anti-alias cut and a sharp edge response, like a sharp decimation (measured against LibRaw's half-size picture, docs 32.12).
//   Gl, Rl, Bl: the same for each colour at the low colour-difference bandwidth (cF0 .. cF1).
//   G = Gh, R = Gh + (Rl - Gl), B = Gh + (Bl - Gl): luminance detail from the dense greens, colour at low bandwidth (the colour-difference principle).
// Clip fallback (no colour from clipped samples): when any tap of the window is clipped (scaled value >= kHalfClip), each colour is the weighted mean of its unclipped
// taps with positive gaussian weights (all of them when fewer than 15 % of the weight is unclipped): a clipped sample's colour is a lower bound, never mixed in.
// Overshoot clamp: the peaked green may exceed the min / max of the central 4 x 4 green samples by 15 % of their range only (no ringing at steps); the same shift
// is applied to R and B, so colour differences are untouched.

namespace {
// Solve (A + ridge I) w = b by Cholesky (A symmetric positive definite), in place.
bool choleskySolve(std::vector<double>& A, std::vector<double>& b, int n) {
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j <= i; ++j) {
      double s = A[std::size_t(i) * n + j];
      for (int k = 0; k < j; ++k) s -= A[std::size_t(i) * n + k] * A[std::size_t(j) * n + k];
      if (i == j) {
        if (s <= 1e-12) return false;
        A[std::size_t(i) * n + i] = std::sqrt(s);
      } else A[std::size_t(i) * n + j] = s / A[std::size_t(j) * n + j];
    }
  }
  for (int i = 0; i < n; ++i) {
    double s = b[i];
    for (int k = 0; k < i; ++k) s -= A[std::size_t(i) * n + k] * b[k];
    b[i] = s / A[std::size_t(i) * n + i];
  }
  for (int i = n - 1; i >= 0; --i) {
    double s = b[i];
    for (int k = i + 1; k < n; ++k) s -= A[std::size_t(k) * n + i] * b[k];
    b[i] = s / A[std::size_t(i) * n + i];
  }
  return true;
}

double bandTarget(double f, double f0, double f1) {  // 1 below f0, raised cosine to 0 at f1
  f = std::fabs(f);
  if (f <= f0) return 1.0;
  if (f >= f1) return 0.0;
  const double t = (f - f0) / (f1 - f0);
  return 0.5 * (1.0 + std::cos(3.14159265358979323846 * t));
}

// weights (sum 1) of the taps at offsets (dx, dy) from the window centre so that the response follows the separable band target
void designTaps(const std::vector<float>& dx, const std::vector<float>& dy, double f0, double f1, double boost, const HalfDesign& hd, std::vector<double>& w) {
  const int n = int(dx.size());
  std::vector<double> A(std::size_t(n) * n, 0.0), b(n, 0.0), cs(n), sn(n);
  const double twoPi = 6.28318530717958647692;
  for (int iy = -8; iy <= 8; ++iy)
    for (int ix = 0; ix <= 8; ++ix) {
      const double fx = ix / 16.0, fy = iy / 16.0;
      auto bt = [&](double f) { const double a = std::min(std::fabs(f) / 0.25, 1.0); return bandTarget(f, f0, f1) * (1.0 + boost * a * a); };
      const double H = bt(fx) * bt(fy);
      double W = (std::fabs(fx) >= f1 || std::fabs(fy) >= f1) ? hd.stopW : ((std::fabs(fx) <= f0 && std::fabs(fy) <= f0) ? hd.passW : hd.transW);
      if (ix == 0 && iy == 0) W = 1000.0;  // DC: sum of the weights = 1
      for (int k = 0; k < n; ++k) {
        const double a = twoPi * (fx * dx[k] + fy * dy[k]);
        cs[k] = std::cos(a), sn[k] = std::sin(a);
      }
      for (int i = 0; i < n; ++i) {
        b[i] += W * H * cs[i];
        for (int j = 0; j < n; ++j) A[std::size_t(i) * n + j] += W * (cs[i] * cs[j] + sn[i] * sn[j]);
      }
    }
  for (int i = 0; i < n; ++i) A[std::size_t(i) * n + i] += hd.ridge * 4.0;
  w.assign(b.begin(), b.end());
  if (!choleskySolve(A, w, n)) w.assign(n, 1.0 / n);
}
}  // namespace

void buildHalfTables(const std::uint8_t* cfa3, const HalfDesign& hd, HalfTables& T) {
  std::memset(&T, 0, sizeof T);
  for (int b = 0; b < 3; ++b)
    for (int a = 0; a < 3; ++a) {
      const int ph = b * 3 + a;
      int col[64];
      for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) col[j * 8 + i] = cfa3[pmod(2 * b - 3 + j, 6) * 6 + pmod(2 * a - 3 + i, 6)];
      std::vector<double> lo[3];
      std::vector<int> idx[3];
      std::vector<double> gh;
      for (int c = 0; c < 3; ++c) {
        std::vector<float> dx, dy;
        for (int k = 0; k < 64; ++k)
          if (col[k] == c) idx[c].push_back(k), dx.push_back(float(k % 8) - 3.5f), dy.push_back(float(k / 8) - 3.5f);
        if (dx.empty()) continue;
        designTaps(dx, dy, hd.cF0, hd.cF1, 0.0, hd, lo[c]);
        if (c == 1) designTaps(dx, dy, hd.gF0, hd.gF1, hd.gBoost, hd, gh);
      }
      double w[3][64] = {};
      if (!gh.empty())
        for (std::size_t q = 0; q < idx[1].size(); ++q) {
          const int k = idx[1][q];
          w[0][k] += gh[q] - lo[1][q], w[1][k] += gh[q], w[2][k] += gh[q] - lo[1][q];
        }
      for (std::size_t q = 0; q < idx[0].size(); ++q) w[0][idx[0][q]] += lo[0][q];
      for (std::size_t q = 0; q < idx[2].size(); ++q) w[2][idx[2][q]] += lo[2][q];
      for (int o = 0; o < 3; ++o) {  // prune the negligible taps (the clip test and the cost follow the support), then renormalise to sum 1
        double sum = 0;
        for (int k = 0; k < 64; ++k) {
          if (std::fabs(w[o][k]) < 0.002) w[o][k] = 0;
          sum += w[o][k];
        }
        for (int k = 0; k < 64; ++k) {
          T.w[ph][o][k] = float(w[o][k] / sum);
          if (w[o][k] != 0) T.sup[ph][k] = 1.f;
        }
      }
      for (int k = 0; k < 64; ++k)
        if (col[k] == 1 && k % 8 >= 2 && k % 8 <= 5 && k / 8 >= 2 && k / 8 <= 5) T.cen[ph][k] = 1.f;
      for (int c = 0; c < 3; ++c) {
        double sum = 0;
        double gw[64] = {};
        for (int k : idx[c]) {
          const double ddx = double(k % 8) - 3.5, ddy = double(k / 8) - 3.5;
          gw[k] = std::exp(-(ddx * ddx + ddy * ddy) / (2.0 * 1.5 * 1.5));
          sum += gw[k];
        }
        for (int k = 0; k < 64; ++k) T.g[ph][c][k] = sum > 0 ? float(gw[k] / sum) : 0.f;
      }
    }
}

const HalfTables& halfTables(const Desc& d) {
  static std::mutex mu;
  static std::map<std::string, std::unique_ptr<HalfTables>> cache;
  std::string key(reinterpret_cast<const char*>(d.cfa3), 36);
  std::lock_guard<std::mutex> lk(mu);
  auto it = cache.find(key);
  if (it == cache.end()) {
    auto t = std::make_unique<HalfTables>();
    const HalfDesign hd;
    buildHalfTables(d.cfa3, hd, *t);
    it = cache.emplace(key, std::move(t)).first;
  }
  return *it->second;
}

namespace {
// Scaled window of the mosaic [x0, x0 + w) x [y0, y0 + h) (absolute, may reach outside: coordinate mapping keeps the CFA phase): the arithmetic of buildWindow.
void buildRawWindow(const Desc& d, int x0, int y0, int w, int h, std::vector<float>& S) {
  S.resize(std::size_t(w) * h);
  std::vector<int> xs(w), px(w), bxi(w);  // mapped column, CFA column phase, black column phase: no division in the sample loop (the 40 MP window build was division bound on a phone)
  const int P = d.period;
  for (int i = 0; i < w; ++i) xs[i] = mapCoord(x0 + i, d.width, d.cfa), px[i] = pmod(x0 + i, P), bxi[i] = pmod(x0 + i, d.blackCols);
  for (int j = 0; j < h; ++j) {
    const int ay = y0 + j, y = mapCoord(ay, d.height, d.cfa);
    float* row = S.data() + std::size_t(j) * w;
    const std::uint16_t* src = d.data + std::size_t(y) * d.width;
    const std::uint8_t* c4row = d.cfa4 + pmod(ay, P) * P;
    const float* bkrow = d.black + pmod(ay, d.blackRows) * d.blackCols;
    for (int i = 0; i < w; ++i) {
      // the sample comes from the mapped pixel; the colour phase and the black table phase from the window position (what the GPU kernel sees)
      const float v = std::max(float(src[xs[i]]) - bkrow[bxi[i]], 0.f) * d.wb[c4row[px[i]]] * d.invSpan;
      row[i] = std::min(v, 1.f);
    }
  }
}

void halfTile(const Desc& d, const HalfTables& T, Rect roi, float* out, std::size_t stride, std::vector<float>& S) {
  const int rw = roi.w(), rh = roi.h(), ww = 2 * rw + 6, wh = 2 * rh + 6;
  buildRawWindow(d, 2 * roi.x0 - 3, 2 * roi.y0 - 3, ww, wh, S);
  for (int y = 0; y < rh; ++y) {
    float* o = out + std::size_t(y) * stride;
    for (int x = 0; x < rw; ++x) {
      const int ph = pmod(roi.y0 + y, 3) * 3 + pmod(roi.x0 + x, 3);
      const float* base = S.data() + std::size_t(2 * y) * ww + 2 * x;
      const float* w0 = T.w[ph][0];
      const float* w1 = T.w[ph][1];
      const float* w2 = T.w[ph][2];
      const float* sp = T.sup[ph];
      // eight lanes (one per window column) accumulate independently so the compiler can vectorise (a single accumulator is a latency-bound dependent chain);
      // the order is fixed per pixel, so tiles and threads give identical values
      float l0[8] = {}, l1[8] = {}, l2[8] = {}, lm[8] = {};
      for (int j = 0; j < 8; ++j) {
        const float* row = base + std::size_t(j) * ww;
        const int k0 = j * 8;
        for (int i = 0; i < 8; ++i) {
          const float s = row[i];
          l0[i] += w0[k0 + i] * s, l1[i] += w1[k0 + i] * s, l2[i] += w2[k0 + i] * s;
          lm[i] = std::max(lm[i], s * sp[k0 + i]);
        }
      }
      float a0 = 0.f, a1 = 0.f, a2 = 0.f, smax = 0.f;
      for (int i = 0; i < 8; ++i) a0 += l0[i], a1 += l1[i], a2 += l2[i], smax = std::max(smax, lm[i]);
      {  // overshoot clamp: the green may not leave the range of the central green samples (plus a margin); the same shift goes to R and B (colour differences kept)
        const float* cn = T.cen[ph];
        float lo = 2.f, hi = -1.f;
        for (int j = 2; j < 6; ++j)
          for (int i = 2; i < 6; ++i) {
            const int k = j * 8 + i;
            if (cn[k] > 0.f) lo = std::min(lo, base[std::size_t(j) * ww + i]), hi = std::max(hi, base[std::size_t(j) * ww + i]);
          }
        const float mg = kHalfOvershoot * (hi - lo);
        const float gc = std::min(std::max(a1, lo - mg), hi + mg), dg = gc - a1;
        a0 += dg, a1 += dg, a2 += dg;
      }
      float rgb[3] = {a0, a1, a2};
      if (smax >= kHalfClip) {  // clip fallback: colours from the unclipped taps only
        float su[3] = {0, 0, 0}, wu[3] = {0, 0, 0}, sa[3] = {0, 0, 0};
        const int oy = pmod(2 * roi.y0 - 3, 6), ox = pmod(2 * roi.x0 - 3, 6);
        for (int j = 0; j < 8; ++j)
          for (int i = 0; i < 8; ++i) {
            const int c = d.cfa3[pmod(oy + 2 * y + j, 6) * 6 + pmod(ox + 2 * x + i, 6)];
            const float s = base[std::size_t(j) * ww + i], g = T.g[ph][c][j * 8 + i];
            sa[c] += g * s;
            if (s < kHalfClip) su[c] += g * s, wu[c] += g;
          }
        for (int c = 0; c < 3; ++c) rgb[c] = wu[c] >= 0.15f ? su[c] / wu[c] : sa[c];
      }
      float* px = o + 3 * x;
      for (int q = 0; q < 3; ++q) rgb[q] = clamp01(rgb[q]);
      for (int r = 0; r < 3; ++r) {
        const float v = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
        px[r] = d.clampOut ? clamp01(v) : v;
      }
    }
  }
}
}  // namespace

void demosaicHalf(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads) {
  if (roi.empty()) return;
  const HalfTables& T = halfTables(d);
  runTiles(d, roi, threads, 128, [&](Rect t, std::vector<float>& S) { halfTile(d, T, t, out + std::size_t(t.y0 - roi.y0) * strideFloats + std::size_t(t.x0 - roi.x0) * 3, strideFloats, S); });
}
