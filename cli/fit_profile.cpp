// `dapple fit-profile`: fits a camera matching profile (ColorMatchProfile) so the untouched RAW
// render looks like the camera's own JPEG.
//
//   dapple fit-profile --dir D:/photos --out core/src/profiles/sony_ilce7rm5.inc
//        [--edge 640] [--cache DIR] [--train-res 4,5,6] [--valid-res 0] [--mod 10]
//        [--lut-l 13] [--lut-ab 17] [--smooth 1e-4] [--no-matrix] [--name sony_ilce7rm5]
//
// Pairs of (our render without any colour profile, camera JPEG registered to it) are collected
// from the training files (file index modulo --mod in --train-res) and a held-out set
// (--valid-res; the same files `dapple eval --step 10` uses). Stage 1 fits a 3x3 matrix in linear
// sRGB (rows sum to 1) by least squares; stage 2 fits a smooth OKLab offset LUT by regularised,
// robust (IRLS) least squares. The held-out dE2000 is reported before and after.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "color.h"
#include "colour_metrics.h"
#include "dapple/developer.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#include "eval.h"
#include "eval_common.h"

namespace dapple::evaltool {

namespace {

struct Pair {
  float pre[3];  // linear sRGB of our render without profile
  float cam[3];  // linear sRGB of the camera JPEG
  float w;
};

std::vector<int> parseList(const std::string& s) {
  std::vector<int> v;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, ',');) v.push_back(std::atoi(t.c_str()));
  return v;
}

CameraProfile bareProfile(const CameraProfile& p) {
  CameraProfile b = p;
  b.colorMatch = {};
  b.baselineHsl = {};
  return b;
}

// Mean CIEDE2000 of a profile over cases (render at the case size, camera registered).
double meanDE(const std::vector<Case>& cases, const CameraProfile& prof, double* p95 = nullptr) {
  double sum = 0;
  long n = 0;
  std::vector<long> hist(1201, 0);
  for (const Case& c : cases) {
    ImageF out;
    if (!engine::renderImageF(EditParams{}, prof, nullptr, c.src, true, out)) continue;
    const int m = c.align.margin;
    double s = 0;
    long k = 0;
    for (int y = m; y < out.height - m; y += 2)
      for (int x = m; x < out.width - m; x += 2) {
        const std::size_t i = (std::size_t(y) * out.width + x) * 3;
        const double de = metrics::deltaE2000(metrics::srgbToLab(&c.cam.data[i]), metrics::srgbToLab(&out.data[i]));
        s += de;
        ++k;
        hist[std::min(1200, int(de / 0.05))]++;
      }
    sum += s / std::max(1L, k);  // every image counts equally
    ++n;
  }
  if (p95) {
    long tot = 0, acc = 0;
    for (long h : hist) tot += h;
    *p95 = 60;
    for (int i = 0; i <= 1200; ++i) {
      acc += hist[i];
      if (acc >= 0.95 * tot) {
        *p95 = (i + 0.5) * 0.05;
        break;
      }
    }
  }
  return n ? sum / n : 0;
}

void collectPairs(const std::vector<Case>& cases, const CameraProfile& bare, std::vector<Pair>& out, int stride) {
  for (const Case& c : cases) {
    ImageF pre;
    if (!engine::renderImageF(EditParams{}, bare, nullptr, c.src, true, pre)) continue;
    const int m = c.align.margin;
    long cnt = 0;
    for (int y = m; y < pre.height - m; y += stride)
      for (int x = m; x < pre.width - m; x += stride) ++cnt;
    const float w = 1.f / float(std::max(1L, cnt));  // every image counts equally
    for (int y = m; y < pre.height - m; y += stride)
      for (int x = m; x < pre.width - m; x += stride) {
        const std::size_t i = (std::size_t(y) * pre.width + x) * 3;
        Pair p;
        for (int k = 0; k < 3; ++k) {
          p.pre[k] = color::srgbDecode(pre.data[i + k]);
          p.cam[k] = color::srgbDecode(c.cam.data[i + k]);
        }
        p.w = w;
        out.push_back(p);
      }
  }
}

// ---- matrix: per output row, least squares with the row sum fixed to 1 ----
void fitMatrix(const std::vector<Pair>& pairs, float M[9]) {
  for (int r = 0; r < 3; ++r) {
    // y - x2 = m0 (x0 - x2) + m1 (x1 - x2), with the other two indices cyclic-free: pick base channel = r
    const int a = (r + 1) % 3, b = (r + 2) % 3;  // free coefficients for a and b, coefficient of r = 1 - ma - mb
    double saa = 0, sab = 0, sbb = 0, sya = 0, syb = 0;
    for (const Pair& p : pairs) {
      const double lum = 0.2126 * p.cam[0] + 0.7152 * p.cam[1] + 0.0722 * p.cam[2];
      const double w = p.w / (0.04 + lum);  // roughly perceptual weighting
      const double xa = p.pre[a] - p.pre[r], xb = p.pre[b] - p.pre[r], y = p.cam[r] - p.pre[r];
      saa += w * xa * xa;
      sab += w * xa * xb;
      sbb += w * xb * xb;
      sya += w * y * xa;
      syb += w * y * xb;
    }
    const double det = saa * sbb - sab * sab;
    const double ma = (sya * sbb - syb * sab) / det, mb = (syb * saa - sya * sab) / det;
    M[r * 3 + r] = float(1 - ma - mb);
    M[r * 3 + a] = float(ma);
    M[r * 3 + b] = float(mb);
  }
}

struct Grid {
  int nl, nab;
  float abMax;
  std::size_t nodes() const { return std::size_t(nl) * nab * nab; }
};

struct Node8 {
  std::size_t idx[8];
  float w[8];
};

Node8 locate(const Grid& g, const float lab[3]) {
  const float fl = std::clamp(lab[0], 0.f, 1.f) * float(g.nl - 1);
  const float k = float(g.nab - 1) / (2.f * g.abMax);
  const float fa = (std::clamp(lab[1], -g.abMax, g.abMax) + g.abMax) * k;
  const float fb = (std::clamp(lab[2], -g.abMax, g.abMax) + g.abMax) * k;
  const int il = std::min(int(fl), g.nl - 2), ia = std::min(int(fa), g.nab - 2), ib = std::min(int(fb), g.nab - 2);
  const float tl = fl - il, ta = fa - ia, tb = fb - ib;
  Node8 n;
  int j = 0;
  for (int dl = 0; dl < 2; ++dl)
    for (int db = 0; db < 2; ++db)
      for (int da = 0; da < 2; ++da, ++j) {
        n.idx[j] = (std::size_t(il + dl) * g.nab + (ib + db)) * g.nab + (ia + da);
        n.w[j] = (dl ? tl : 1 - tl) * (db ? tb : 1 - tb) * (da ? ta : 1 - ta);
      }
  return n;
}

// Solves (A + smooth * Laplacian + ridge) D = B for 3 right-hand sides with conjugate gradients.
void solve(const std::vector<double>& A, const std::vector<double>& B, const Grid& g, double smooth, double ridge,
           std::vector<double>& D) {
  const std::size_t N = g.nodes();
  auto neighbours = [&](std::size_t i, std::size_t out[6]) {
    const int ia = int(i % g.nab), ib = int((i / g.nab) % g.nab), il = int(i / (std::size_t(g.nab) * g.nab));
    int c = 0;
    if (ia > 0) out[c++] = i - 1;
    if (ia < g.nab - 1) out[c++] = i + 1;
    if (ib > 0) out[c++] = i - g.nab;
    if (ib < g.nab - 1) out[c++] = i + g.nab;
    if (il > 0) out[c++] = i - std::size_t(g.nab) * g.nab;
    if (il < g.nl - 1) out[c++] = i + std::size_t(g.nab) * g.nab;
    return c;
  };
  auto mul = [&](const std::vector<double>& x, std::vector<double>& y) {  // x,y: N*3
    for (std::size_t i = 0; i < N; ++i) {
      double acc[3] = {0, 0, 0};
      const int ia = int(i % g.nab), ib = int((i / g.nab) % g.nab), il = int(i / (std::size_t(g.nab) * g.nab));
      const double* row = &A[i * 27];
      for (int dl = -1; dl <= 1; ++dl) {
        if (il + dl < 0 || il + dl >= g.nl) continue;
        for (int db = -1; db <= 1; ++db) {
          if (ib + db < 0 || ib + db >= g.nab) continue;
          for (int da = -1; da <= 1; ++da) {
            if (ia + da < 0 || ia + da >= g.nab) continue;
            const double a = row[(dl + 1) * 9 + (db + 1) * 3 + (da + 1)];
            if (a == 0) continue;
            const std::size_t j = (std::size_t(il + dl) * g.nab + (ib + db)) * g.nab + (ia + da);
            acc[0] += a * x[3 * j];
            acc[1] += a * x[3 * j + 1];
            acc[2] += a * x[3 * j + 2];
          }
        }
      }
      std::size_t nb[6];
      const int c = neighbours(i, nb);
      for (int k = 0; k < c; ++k)
        for (int ch = 0; ch < 3; ++ch) acc[ch] += smooth * (x[3 * i + ch] - x[3 * nb[k] + ch]);
      for (int ch = 0; ch < 3; ++ch) y[3 * i + ch] = acc[ch] + ridge * x[3 * i + ch];
    }
  };
  std::vector<double> diag(N);
  for (std::size_t i = 0; i < N; ++i) {
    std::size_t nb[6];
    diag[i] = A[i * 27 + 13] + smooth * neighbours(i, nb) + ridge;
  }
  std::vector<double> r = B, z(N * 3), p, Ap(N * 3);
  D.assign(N * 3, 0.0);
  for (std::size_t i = 0; i < N * 3; ++i) z[i] = r[i] / diag[i / 3];
  p = z;
  double rz = 0;
  for (std::size_t i = 0; i < N * 3; ++i) rz += r[i] * z[i];
  const double rz0 = rz;
  for (int it = 0; it < 400 && rz > 1e-14 * rz0; ++it) {
    mul(p, Ap);
    double pAp = 0;
    for (std::size_t i = 0; i < N * 3; ++i) pAp += p[i] * Ap[i];
    const double alpha = rz / pAp;
    for (std::size_t i = 0; i < N * 3; ++i) {
      D[i] += alpha * p[i];
      r[i] -= alpha * Ap[i];
      z[i] = r[i] / diag[i / 3];
    }
    double rzn = 0;
    for (std::size_t i = 0; i < N * 3; ++i) rzn += r[i] * z[i];
    const double beta = rzn / rz;
    rz = rzn;
    for (std::size_t i = 0; i < N * 3; ++i) p[i] = z[i] + beta * p[i];
  }
}

}  // namespace

int cmdFitProfile(const std::vector<std::string>& args) {
  std::string dir, out, cache = "build/eval/cache", name = "profile";
  int edge = 640, mod = 10, nl = 13, nab = 17, stride = 3, irls = 2;
  std::vector<int> trainRes = {4, 5, 6}, validRes = {0};
  double smooth = 1e-4;
  bool useMatrix = true;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto val = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
    const std::string& a = args[i];
    if (a == "--dir") dir = val();
    else if (a == "--out") out = val();
    else if (a == "--cache") cache = val();
    else if (a == "--name") name = val();
    else if (a == "--edge") edge = std::atoi(val().c_str());
    else if (a == "--mod") mod = std::atoi(val().c_str());
    else if (a == "--train-res") trainRes = parseList(val());
    else if (a == "--valid-res") validRes = parseList(val());
    else if (a == "--lut-l") nl = std::atoi(val().c_str());
    else if (a == "--lut-ab") nab = std::atoi(val().c_str());
    else if (a == "--smooth") smooth = std::atof(val().c_str());
    else if (a == "--stride") stride = std::atoi(val().c_str());
    else if (a == "--irls") irls = std::atoi(val().c_str());
    else if (a == "--no-matrix") useMatrix = false;
    else {
      std::fprintf(stderr, "fit-profile: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (dir.empty()) {
    std::fputs("usage: dapple fit-profile --dir <ARW folder> [--out profile.inc] [--edge 640] [--train-res 4,5,6]\n"
               "                           [--valid-res 0] [--mod 10] [--lut-l 13] [--lut-ab 17] [--smooth 1e-4]\n"
               "                           [--no-matrix] [--name id]\n",
               stderr);
    return 2;
  }
  const Developer dev;
  const auto all = listArw(fromU8(dir));
  std::vector<fs::path> trainFiles, validFiles;
  for (std::size_t i = 0; i < all.size(); ++i) {
    const int r = int(i % std::size_t(mod));
    if (std::find(trainRes.begin(), trainRes.end(), r) != trainRes.end()) trainFiles.push_back(all[i]);
    if (std::find(validRes.begin(), validRes.end(), r) != validRes.end()) validFiles.push_back(all[i]);
  }
  std::fprintf(stderr, "fit-profile: %zu training files, %zu held-out files (of %zu)\n", trainFiles.size(),
               validFiles.size(), all.size());
  const auto train = loadCases(dev, trainFiles, edge, fromU8(cache));
  const auto valid = loadCases(dev, validFiles, edge, fromU8(cache));
  if (train.empty() || valid.empty()) return 1;
  const CameraProfile base = dev.cameras().resolve(train[0].src.meta.make, train[0].src.meta.model);
  const CameraProfile bare = bareProfile(base);
  std::fprintf(stderr, "profile base: %s\n", base.id.c_str());

  double p95 = 0;
  const double trainBare = meanDE(train, bare, &p95), validBare = meanDE(valid, bare, &p95);
  std::printf("no colour profile:      train dE %.3f   held-out dE %.3f (p95 %.2f)\n", trainBare, validBare, p95);

  std::vector<Pair> pairs;
  collectPairs(train, bare, pairs, stride);
  std::fprintf(stderr, "%zu training pixel pairs\n", pairs.size());

  ColorMatchProfile prof;
  float M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  if (useMatrix) {
    fitMatrix(pairs, M);
    prof.hasMatrix = true;
    std::copy(M, M + 9, prof.matrix);
    CameraProfile cp = bare;
    cp.colorMatch = prof;
    std::printf("matrix only:            train dE %.3f   held-out dE %.3f (p95 %.2f)\n", meanDE(train, cp),
                meanDE(valid, cp, &p95), p95);
    std::printf("  matrix: [%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]\n", M[0], M[1], M[2], M[3], M[4], M[5],
                M[6], M[7], M[8]);
  }

  // ---- OKLab offset LUT ----
  const Grid g{nl, nab, 0.32f};
  const std::size_t N = g.nodes();
  struct S {
    float lab[3];
    float d[3];
    float w;
  };
  std::vector<S> samples(pairs.size());
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    float pre[3] = {pairs[i].pre[0], pairs[i].pre[1], pairs[i].pre[2]};
    if (useMatrix) {
      float t[3];
      for (int r = 0; r < 3; ++r) t[r] = M[3 * r] * pre[0] + M[3 * r + 1] * pre[1] + M[3 * r + 2] * pre[2];
      std::copy(t, t + 3, pre);
    }
    float cam[3] = {pairs[i].cam[0], pairs[i].cam[1], pairs[i].cam[2]};
    float lp[3], lc[3];
    color::linearSrgbToOklab(pre, lp);
    color::linearSrgbToOklab(cam, lc);
    for (int k = 0; k < 3; ++k) {
      samples[i].lab[k] = lp[k];
      samples[i].d[k] = lc[k] - lp[k];
    }
    samples[i].w = pairs[i].w;
  }
  pairs.clear();
  pairs.shrink_to_fit();
  double wsum = 0;
  for (const S& s : samples) wsum += s.w;
  for (S& s : samples) s.w = float(s.w / wsum);

  std::vector<double> D;
  std::vector<float> rw(samples.size(), 1.f);
  for (int pass = 0; pass <= irls; ++pass) {
    std::vector<double> A(N * 27, 0.0), B(N * 3, 0.0);  // sparse: 27 neighbour offsets per node
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const S& s = samples[i];
      const double w = double(s.w) * rw[i];
      const Node8 n = locate(g, s.lab);
      for (int a = 0; a < 8; ++a) {
        const double wa = w * n.w[a];
        if (wa == 0) continue;
        for (int c = 0; c < 3; ++c) B[3 * n.idx[a] + c] += wa * s.d[c];
        double* row = &A[n.idx[a] * 27];
        for (int b = 0; b < 8; ++b)
          row[((b >> 2) - (a >> 2) + 1) * 9 + (((b >> 1) & 1) - ((a >> 1) & 1) + 1) * 3 + ((b & 1) - (a & 1) + 1)] +=
              wa * n.w[b];
      }
    }
    solve(A, B, g, smooth / double(N), 1e-9, D);  // smoothing relative to the average node weight 1/N
    // Robust weights from the residuals of this solution.
    double meanRes = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const S& s = samples[i];
      const Node8 n = locate(g, s.lab);
      double off[3] = {0, 0, 0};
      for (int a = 0; a < 8; ++a)
        for (int c = 0; c < 3; ++c) off[c] += n.w[a] * D[3 * n.idx[a] + c];
      const double r = std::sqrt((s.d[0] - off[0]) * (s.d[0] - off[0]) + (s.d[1] - off[1]) * (s.d[1] - off[1]) +
                                 (s.d[2] - off[2]) * (s.d[2] - off[2]));
      rw[i] = float(1.0 / std::max(1.0, r / 0.04));
      meanRes += r * s.w;
    }
    std::fprintf(stderr, "  pass %d: weighted mean OKLab residual %.4f\n", pass, meanRes);
  }

  prof.lutL = nl;
  prof.lutAB = nab;
  prof.lutABMax = g.abMax;
  prof.lut.resize(N * 3);
  for (std::size_t i = 0; i < N * 3; ++i) prof.lut[i] = float(D[i]);
  CameraProfile fitted = bare;
  fitted.colorMatch = prof;
  const double trainFit = meanDE(train, fitted), validFit = meanDE(valid, fitted, &p95);
  std::printf("matrix + OKLab LUT:     train dE %.3f   held-out dE %.3f (p95 %.2f)\n", trainFit, validFit, p95);

  if (!out.empty()) {
    std::ofstream f(fromU8(out));
    f << "// Generated by `dapple fit-profile` -- do not edit by hand.\n";
    f << "// training files: " << train.size() << " (index mod " << mod << " in {";
    for (std::size_t i = 0; i < trainRes.size(); ++i) f << (i ? "," : "") << trainRes[i];
    f << "}), held-out files: " << valid.size() << ", fit size " << edge << " px\n";
    char buf[200];
    std::snprintf(buf, sizeof buf, "// dE2000 (mean per image): no profile train %.2f / held-out %.2f -> fitted train %.2f / held-out %.2f\n",
                  trainBare, validBare, trainFit, validFit);
    f << buf;
    f << "static const bool k_" << name << "_hasMatrix = " << (useMatrix ? "true" : "false") << ";\n";
    f << "static const float k_" << name << "_matrix[9] = {";
    for (int i = 0; i < 9; ++i) {
      std::snprintf(buf, sizeof buf, "%s%.5ff", i ? ", " : "", M[i]);
      f << buf;
    }
    f << "};\n";
    f << "static const int k_" << name << "_lutL = " << nl << ";\nstatic const int k_" << name << "_lutAB = " << nab
      << ";\nstatic const float k_" << name << "_lutABMax = " << g.abMax << "f;\n";
    f << "static const float k_" << name << "_lut[" << N * 3 << "] = {\n";
    for (std::size_t i = 0; i < N * 3; ++i) {
      std::snprintf(buf, sizeof buf, "%.5ff,", prof.lut[i]);
      f << buf << ((i % 9 == 8) ? "\n" : " ");
    }
    f << "};\n";
    std::printf("wrote %s\n", out.c_str());
  }
  return 0;
}

}  // namespace dapple::evaltool
