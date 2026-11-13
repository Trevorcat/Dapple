// Film exposure that keeps the photo's key (film_match.h).
#include "dapple/film_match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "color.h"
#include "dapple/illuminant.h"
#include "film_internal.h"
#include "dapple/renderer.h"
#include "film_match_render.h"

namespace dapple {

// Median CIE L* of an sRGB-encoded float image (BT.709 luminance of the linearised values).
float medianLightness(const ImageF& img) {
  const std::size_t n = std::size_t(img.width) * img.height;
  if (!n) return 0.f;
  std::vector<float> ls(n);
  std::size_t k = 0;
  for (int y = 0; y < img.height; ++y) {
    const float* r = img.row(y);
    for (int x = 0; x < img.width; ++x, ++k) {
      const float rl = color::srgbDecode(r[3 * x]), gl = color::srgbDecode(r[3 * x + 1]), bl = color::srgbDecode(r[3 * x + 2]);
      const float Y = std::clamp(0.2126f * rl + 0.7152f * gl + 0.0722f * bl, 0.f, 1.f);
      ls[k] = Y > 0.008856f ? 116.f * std::cbrt(Y) - 16.f : 903.3f * Y;
    }
  }
  std::nth_element(ls.begin(), ls.begin() + n / 2, ls.end());
  return ls[n / 2];
}

std::string filmMatchKey(const EditParams& p) {
  if (p.filmStock.empty()) return {};
  EditParams k = p;
  k.filmExposure = 0.f;
  k.filmKeyEv = 0.f;
  k.filmPrintEv = 0.f;
  k.filmBlack = 0.f;
  k.filmStrength = 100.f;
  k.filmGrain = 0.f;
  k.filmHalation = 0.f;
  k.clarity = k.texture = k.dehaze = k.grain = 0.f;
  k.sharpen = k.nrLuma = 0.f;  // vignette, nrColor and sharpRadius stay in the key: the solve's develop render reads them (matchFilmExposure clears only the fields above)
  return toJson(k);
}

FilmMatchOptions FilmMatchOptions::fromEnv() {
  FilmMatchOptions o;
  const char* full = std::getenv("DAPPLE_FILM_MATCH_FULL");
  if (const char* v = std::getenv("DAPPLE_FILM_MATCH_V4")) {
    if (v[0] && v[0] != '0') o.mode = FilmMatchMode::Window;
  }
  if (const char* v = std::getenv("DAPPLE_FILM_MATCH_PULL")) o.mode = FilmMatchMode::Pull, o.pull = std::strtof(v, nullptr);
  if (full && full[0] && full[0] != '0') o.mode = FilmMatchMode::Full, o.pull = 1.f, o.latitude = false;
  if (const char* v = std::getenv("DAPPLE_FILM_MATCH_NEG_BIAS")) o.negativeBiasEv = std::strtof(v, nullptr);
  if (const char* v = std::getenv("DAPPLE_FILM_MATCH_REV_MARGIN")) o.shoulderMarginReversal = std::strtof(v, nullptr);
  return o;
}

bool filmPaperMapStretchesBlack() { return FilmMatchOptions::fromEnv().mode == FilmMatchMode::Window; }

bool filmPaperMappingEnabled() {
  if (std::getenv("DAPPLE_FILM_MATCH_OFF") || std::getenv("DAPPLE_FILM_PAPER_MAP_OFF")) return false;
  const FilmMatchMode m = FilmMatchOptions::fromEnv().mode;
  return m == FilmMatchMode::Meter || m == FilmMatchMode::Window;
}

FilmLatitude filmLatitude(const film::FilmStock& s) {
  FilmLatitude r;
  r.reversal = s.positive();
  const std::size_t n = s.logExposure.size();
  if (n < 16 || s.densityCurves.size() != n * 3) return r;
  std::vector<double> x(n), d(n), sl(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = s.logExposure[i];
    d[i] = s.densityCurves[i * 3 + 1];  // green: the mid layer
    if (!std::isfinite(x[i]) || !std::isfinite(d[i])) return r;
    if (i && !(x[i] > x[i - 1])) return r;
  }
  if (x.front() > -0.3 || x.back() < 0.3) return r;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t a = i ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
    sl[i] = std::fabs((d[b] - d[a]) / (x[b] - x[a]));
  }
  double sum = 0;
  int cnt = 0;
  std::size_t i0 = 0;
  double best = 1e9;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(x[i]) < best) best = std::fabs(x[i]), i0 = i;
    if (x[i] >= -0.3 && x[i] <= 0.3) sum += sl[i], ++cnt;
  }
  if (!cnt) return r;
  const double gm = sum / cnt;
  if (!(gm > 0.05)) return r;
  const double th = kFilmEdgeSlopeFrac * gm;
  std::size_t k = i0;
  while (k > 0 && sl[k] > th) --k;
  const double toe = x[k];
  k = i0;
  while (k + 1 < n && sl[k] > th) ++k;
  r.ok = true;
  r.midGamma = float(gm);
  r.toe = float(toe);
  r.shoulder = float(x[k]);
  return r;
}

SceneStats sceneLumaStats(const ImageF& lin, const float gain[3]) {
  SceneStats st;
  const std::size_t n = std::size_t(lin.width) * lin.height;
  if (!n) return st;
  std::vector<float> y(n), yc;
  yc.reserve(n);
  std::size_t k = 0;
  std::size_t clipped = 0;
  for (int j = 0; j < lin.height; ++j) {
    const float* r = lin.row(j);
    for (int i = 0; i < lin.width; ++i, ++k) {
      const float v = 0.2880f * std::max(0.f, r[3 * i] * gain[0]) + 0.7119f * std::max(0.f, r[3 * i + 1] * gain[1]) +
                      0.0001f * std::max(0.f, r[3 * i + 2] * gain[2]);
      y[k] = std::isfinite(v) ? v : 0.f;
      const float mx = std::max(r[3 * i], std::max(r[3 * i + 1], r[3 * i + 2]));
      if (mx >= kFilmClipLevel) ++clipped;
      else yc.push_back(y[k]);
    }
  }
  st.clipFrac = float(double(clipped) / double(n));
  {
    const std::vector<float>& base = (yc.empty() || st.clipFrac > kFilmClipMaxFrac) ? y : yc;
    double sl = 0;
    for (float v : base) sl += std::log(std::max(v, kFilmShadowFloorY));
    st.logMean = base.empty() ? 0.f : float(std::exp(sl / double(base.size())));
    st.logH0.resize(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) st.logH0[i] = std::log10(std::max(base[i], kFilmShadowFloorY) / kFilmGreyCard);
    std::sort(st.logH0.begin(), st.logH0.end());
    // the meter reads the median of the counted pixels (the intended key is a median too: develop render L*)
    if (!st.logH0.empty()) st.logMean = kFilmGreyCard * std::pow(10.f, st.logH0[st.logH0.size() / 2]);
  }
  if (std::getenv("DAPPLE_MATCH_TRACE")) {
    static const float th[] = {0.5f, 0.7f, 0.8f, 0.9f, 0.95f, 0.98f, 1.0f, 1.2f};
    for (float t : th) {
      std::size_t c = 0, c2 = 0;
      for (int j = 0; j < lin.height; ++j)
        for (int i = 0; i < lin.width; ++i) {
          const float* r = lin.row(j) + 3 * i;
          c += std::max(r[0], std::max(r[1], r[2])) >= t;
          c2 += r[1] >= t;
        }
      std::fprintf(stderr, "clip trace: max-channel >= %.2f : %.1f%%  (G >= : %.1f%%)\n", double(t), 100.0 * double(c) / double(n), 100.0 * double(c2) / double(n));
    }
  }
  auto pct = [&](double q) {
    const std::size_t i = std::min(n - 1, std::size_t(q * double(n - 1) + 0.5));
    std::nth_element(y.begin(), y.begin() + i, y.end());
    return y[i];
  };
  st.p2 = pct(0.02);
  st.p50 = pct(0.50);
  if (st.clipFrac > kFilmClipMaxFrac || yc.empty()) {
    st.p995 = pct(0.90);
  } else {
    const std::size_t i = std::min(yc.size() - 1, std::size_t(0.995 * double(yc.size() - 1) + 0.5));
    std::nth_element(yc.begin(), yc.begin() + i, yc.end());
    st.p995 = yc[i];
  }
  return st;
}

namespace {
constexpr float kLog2of10 = 0.30103f;  // log10(2): log10 H per stop
constexpr float kShadowFloorY = 0.002f;  // sensor noise floor: crushed blacks do not demand an infinite push
}  // namespace

bool filmExposureWindow(const FilmLatitude& lat, const SceneStats& st, float& evLo, float& evHi,
                        float shoulderMarginReversal) {
  if (!lat.ok || !(st.p995 > 0.f) || !std::isfinite(st.p995) || !std::isfinite(st.p2)) return false;
  const float lHi = std::log10(st.p995 / 0.184f);
  const float lLo = std::log10(std::max(st.p2, kShadowFloorY) / 0.184f);
  evHi = ((lat.shoulder - (lat.reversal ? shoulderMarginReversal : kFilmShoulderMarginLogH)) - lHi) / kLog2of10;
  evLo = ((lat.toe + kFilmToeMarginLogH) - lLo) / kLog2of10;
  return true;
}

float resolveFilmKeyEv(const film::FilmStock* stock, const SceneStats& st, float evMatch, const FilmMatchOptions& opt,
                       FilmMatchInfo* info) {
  constexpr float kMaxEv = 4.f;
  FilmMatchInfo local;
  FilmMatchInfo& m = info ? *info : local;
  m.evMatch = evMatch;
  m.p2 = st.p2;
  m.p50 = st.p50;
  m.p995 = st.p995;
  m.clipFrac = st.clipFrac;
  m.evKey = opt.pull * evMatch;
  m.latitudeOk = false;
  m.conflict = false;
  m.bound = FilmBound::None;
  float ev = m.evKey;
  if (opt.latitude) {
    FilmLatitude lat;
    if (stock) lat = filmLatitude(*stock);
    float lo = 0.f, hi = 0.f;
    if (filmExposureWindow(lat, st, lo, hi, opt.shoulderMarginReversal)) {
      m.latitudeOk = true;
      m.evLo = lo;
      m.evHi = hi;
      if (lo > hi) {
        m.conflict = true;
        ev = lat.reversal ? hi : lo;  // slide: protect the highlights; negative: keep the shadows, highlights roll off
        m.bound = lat.reversal ? FilmBound::Shoulder : FilmBound::Toe;
      } else if (ev < lo) {
        ev = lo;
        m.bound = FilmBound::Toe;
      } else if (ev > hi) {
        ev = hi;
        m.bound = FilmBound::Shoulder;
      }
    } else {
      // no usable curve: a plain limit, logged once per stock
      ev = std::clamp(ev, -kFilmFallbackMaxEv, kFilmFallbackMaxEv);
      m.bound = FilmBound::Fallback;
      static std::mutex mtx;
      static std::set<std::string> seen;
      const std::string id = stock ? stock->stock : std::string("(unknown)");
      std::lock_guard<std::mutex> lock(mtx);
      if (seen.insert(id).second)
        std::fprintf(stderr, "film match: no usable density curve for '%s', key EV clamped to +-%.0f\n", id.c_str(),
                     double(kFilmFallbackMaxEv));
    }
  }
  if (!std::isfinite(ev)) ev = 0.f;
  ev = std::clamp(ev, -kMaxEv, kMaxEv);
  m.ev = ev;
  return ev;
}

float solveFilmKeyEv(const std::function<float(float)>& filmL, float target, FilmMatchInfo* info) {
  constexpr float kMaxEv = 4.f, kTol = 0.35f;
  const bool trace = std::getenv("DAPPLE_MATCH_TRACE") != nullptr;
  float ev0 = 0.f, l0 = filmL(ev0);
  int evals = 1;
  const float filmL0 = l0;
  float best = ev0, bestErr = std::fabs(l0 - target), bestL = l0;
  // L* rises ~10-18 per EV around the mid-tones: a secant iteration from a nominal slope converges in 2-3 steps
  float ev1 = std::clamp((target - l0) / 14.f, -kMaxEv, kMaxEv);
  float l1 = l0;
  for (int it = 0; it < 6 && bestErr > kTol; ++it) {
    if (std::fabs(ev1 - ev0) < 1e-3f) break;
    l1 = filmL(ev1);
    if (trace) std::fprintf(stderr, "match: ev %+.3f -> L* %.2f (target %.2f)\n", ev1, l1, target);
    ++evals;
    const float err = std::fabs(l1 - target);
    if (err < bestErr) bestErr = err, best = ev1, bestL = l1;
    float slope = (l1 - l0) / (ev1 - ev0);
    // reversal stocks get darker with exposure (negative slope); a flat (clipped) region takes a bounded step
    if (std::fabs(slope) < 2.f || !std::isfinite(slope)) slope = slope < 0.f ? -2.f : 2.f;
    const float next = std::clamp(ev1 + (target - l1) / slope, -kMaxEv, kMaxEv);
    ev0 = ev1;
    l0 = l1;
    ev1 = next;
  }
  if (!std::isfinite(best)) best = 0.f;
  if (info) {
    info->ev = best;
    info->targetL = target;
    info->filmL = bestL;
    info->filmL0 = filmL0;
    info->evals = evals;
  }
  return best;
}

static float lightnessOfY(double Y) {
  Y = std::clamp(Y, 0.0, 1.0);
  return float(Y > 0.008856 ? 116.0 * std::cbrt(Y) - 16.0 : 903.3 * Y);
}
static double yOfLightness(double L) {
  const double f = (L + 16.0) / 116.0;
  return L > 8.0 ? f * f * f : L / 903.3;
}

float chooseFilmEv(const film::FilmStock* stock, const SceneStats& st, const FilmMatchOptions& opt, FilmMatchInfo* info) {
  FilmMatchInfo local;
  FilmMatchInfo& m = info ? *info : local;
  m.p2 = st.p2;
  m.p50 = st.p50;
  m.p995 = st.p995;
  m.clipFrac = st.clipFrac;
  m.latitudeOk = false;
  m.conflict = false;
  m.bound = FilmBound::None;
  FilmLatitude lat;
  if (stock) lat = filmLatitude(*stock);
  float lo = 0.f, hi = 0.f, ev = 0.f;
  if (filmExposureWindow(lat, st, lo, hi, opt.shoulderMarginReversal)) {
    m.latitudeOk = true;
    m.evLo = lo;
    m.evHi = hi;
    m.conflict = lo > hi;
    if (lat.reversal) {  // slide: the (unclipped) highlight sits at the shoulder margin, the shadows block up if they must
      ev = hi;
      m.bound = FilmBound::Shoulder;
    } else {  // negative: shadows above the toe with the overexposure bias, never past the shoulder window
      ev = lo + opt.negativeBiasEv;
      m.bound = FilmBound::Toe;
      if (ev > hi) {
        ev = std::max(hi, lo);  // scene wider than the latitude: keep the shadows, highlights roll off
        m.bound = lo > hi ? FilmBound::Toe : FilmBound::Shoulder;
      }
    }
  } else {
    m.bound = FilmBound::Fallback;
    static std::mutex mtx;
    static std::set<std::string> seen;
    const std::string id = stock ? stock->stock : std::string("(unknown)");
    std::lock_guard<std::mutex> lock(mtx);
    if (seen.insert(id).second)
      std::fprintf(stderr, "film match: no usable density curve for '%s', film EV 0\n", id.c_str());
  }
  if (!std::isfinite(ev)) ev = 0.f;
  ev = std::clamp(ev, -4.f, 4.f);
  m.ev = ev;
  return ev;
}

FilmPrintRange filmPrintRange(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& filmId,
                              const std::string& paperId) try {
  static std::mutex mtx;
  static std::map<std::string, FilmPrintRange> cache;
  FilmPrintRange r;
  if (!lib) return r;
  const bool mapped = filmPaperMappingEnabled();
  const std::string key = filmId + "|" + paperId + "|" + std::to_string(std::uintptr_t(lib.get())) + (mapped ? "|m" : "");
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  const film::FilmStock* st = lib->find(filmId);
  if (!st || st->isPrintMedium()) return r;
  const bool scan = paperId == "none" || !lib->find(paperId);
  film::FilmParams fp;
  fp.film = filmId;
  fp.paper = scan ? std::string("none") : paperId;
  fp.camera.autoExposure = false;
  applyFilmPaperMapping(fp);
  fp.disableSpatialAndStochastic();
  film::FilmSimulator sim(lib);
  auto patch = [&](float sceneEv, float printEv) -> float {
    fp.enlarger.printExposure = std::exp2(-double(printEv));
    std::string err;
    if (!sim.setParams(fp, &err)) return -1.f;
    ImageF in(8, 8), out;
    const float v = 0.184f * std::exp2(sceneEv);
    for (float& x : in.data) x = v;
    if (!sim.render(in, out)) return -1.f;
    return medianLightness(out);
  };
  // bright scene + short exposure = paper base (Dmin); dark scene + long exposure = Dmax. A film with no print (scan /
  // projection) has no paper exposure to vary: only the scene's extremes.
  const float lw = patch(8.f, scan ? 0.f : 8.f), lb = patch(-8.f, scan ? 0.f : -8.f);
  if (lw < 0.f || lb < 0.f) return r;
  r.ok = true;
  r.lWhite = lw;
  r.lBlack = lb;
  r.yWhite = float(yOfLightness(lw));
  r.yBlack = float(yOfLightness(lb));
  r.hiL = lightnessOfY(double(r.yWhite) * std::pow(10.0, -double(kPaperWhiteMarginD)));
  r.loL = lightnessOfY(double(r.yBlack) * std::pow(10.0, double(kPaperBlackMarginD)));
  if (scan && r.yWhite > 1e-4f)
    r.maxPrintEv = std::clamp(float(std::log2(std::pow(10.0, -double(kPaperWhiteMarginD)) / double(r.yWhite))), -kFilmMaxPrintEv, kFilmMaxPrintEv);
  std::lock_guard<std::mutex> lock(mtx);
  cache[key] = r;
  return r;
} catch (...) {
  return {};
}

float solvePrintEv(const std::function<float(float)>& printL, float target, const FilmPrintRange& range, FilmMatchInfo* info) {
  FilmMatchInfo local;
  FilmMatchInfo& m = info ? *info : local;
  float t = target;
  PrintBound bound = PrintBound::None;
  if (range.ok) {
    m.paperLoL = range.loL;
    m.paperHiL = range.hiL;
    if (t > range.hiL) t = range.hiL, bound = PrintBound::PaperWhite;
    else if (t < range.loL) t = range.loL, bound = PrintBound::PaperBlack;
  }
  // L* rises monotonically with the print EV but not linearly (paper toe / shoulder): bracket, then Illinois regula falsi
  const float tol = 0.35f;
  int evals = 0;
  auto f = [&](float e) {
    ++evals;
    return printL(e) - t;
  };
  const float limLo = -kFilmMaxPrintEv, limHi = range.ok ? std::min(range.maxPrintEv, kFilmMaxPrintEv) : kFilmMaxPrintEv;
  float xa = std::clamp(0.f, limLo, limHi), fa = f(xa), xb = xa, fb = fa;
  float ev = xa, fbest = fa;
  if (std::fabs(fa) > tol) {
    const float dir = fa < 0.f ? 1.f : -1.f;  // brighter print = higher L*
    float step = 1.5f;
    bool bracketed = false;
    for (int i = 0; i < 6; ++i) {
      const float xn = std::clamp(xb + dir * step, limLo, limHi);
      if (xn == xb) break;
      xa = xb, fa = fb;
      xb = xn, fb = f(xb);
      if (std::fabs(fb) < std::fabs(fbest)) ev = xb, fbest = fb;
      if ((fa < 0.f) != (fb < 0.f)) {
        bracketed = true;
        break;
      }
      step *= 1.5f;
    }
    if (bracketed) {
      float side = 0.f;
      for (int i = 0; i < 8 && std::fabs(fbest) > tol; ++i) {
        const float denom = fb - fa;
        float xm = std::fabs(denom) > 1e-6f ? xb - fb * (xb - xa) / denom : 0.5f * (xa + xb);
        xm = std::clamp(xm, std::min(xa, xb), std::max(xa, xb));
        const float fm = f(xm);
        if (std::fabs(fm) < std::fabs(fbest)) ev = xm, fbest = fm;
        if ((fm < 0.f) == (fb < 0.f)) {
          xb = xm, fb = fm;
          if (side < 0.f) fa *= 0.5f;
          side = -1.f;
        } else {
          xa = xm, fa = fm;
          if (side > 0.f) fb *= 0.5f;
          side = 1.f;
        }
      }
    }
  }
  FilmMatchInfo tmp;
  tmp.filmL = fbest + t;
  tmp.evals = evals;
  if (bound == PrintBound::None && std::fabs(fbest) > 1.0f) bound = PrintBound::SolverLimit;
  float evc = std::clamp(ev, limLo, limHi);
  float Lp = tmp.filmL;
  if (range.ok && range.maxPrintEv < kFilmMaxPrintEv && fbest < -tol && evc >= limHi - 1e-4f)
    bound = PrintBound::PaperWhite;  // no print: the lamp may not lift the clear film above display white
  m.printEv = evc;
  m.keyL = t;
  m.printL = Lp;
  m.printBound = bound;
  m.printEvals = tmp.evals;
  return m.printEv;
}

float meterEv(const SceneStats& st0) {
  if (!(st0.logMean > 0.f)) return 0.f;
  return std::log2(kFilmGreyCard / st0.logMean);
}

float sceneKeyEv(float keyL) {
  const double Y = std::max(yOfLightness(double(keyL)), 1e-4);
  float scale = kFilmSceneKeyScale;  // DAPPLE_FILM_SCENE_EV_SCALE overrides (1 = the previous default)
  if (const char* v = std::getenv("DAPPLE_FILM_SCENE_EV_SCALE")) scale = std::strtof(v, nullptr);
  return std::clamp(scale * float(std::log2(Y / double(kFilmGreyCard))) / kFilmSystemGamma, kFilmSceneEvMin, kFilmSceneEvMax);
}

void filmInputAdaptation(const EditParams& p, double* x, double* y, double* D) {
  *x = *y = 0.0;
  *D = 1.0;
  const double wb = std::clamp(double(p.filmWb), 0.0, 1.0);
  if (!(wb > 0.0) || !(p.filmSceneX > 0.f) || !(p.filmSceneY > 0.f)) return;
  namespace fd = film::detail;
  EditParams c = p;
  c.clamp();
  float g[3];
  Renderer::whiteBalanceGain(c, g);  // the gain the user's white balance applies to the white-balanced data
  const fd::Vec3 w = {1.0 / g[0], 1.0 / g[1], 1.0 / g[2]};  // the colour that gain turns neutral: the light the edit declares
  const fd::Vec3 xyz = fd::mul(fd::adaptationMatrix(fd::cat16(), fd::kD50x, fd::kD50y, p.filmSceneX, p.filmSceneY), fd::mul(fd::prophotoToXyz(), w));
  const double s = xyz[0] + xyz[1] + xyz[2];
  if (!(s > 0.0)) return;
  *x = xyz[0] / s;
  *y = xyz[1] / s;
  *D = 1.0 - wb;
}

void applyFilmWb(film::FilmParams& fp, const EditParams& p) {
  double x, y, D;
  filmInputAdaptation(p, &x, &y, &D);
  fp.camera.sceneWhiteX = x;
  fp.camera.sceneWhiteY = y;
  fp.camera.adaptationDegree = D;
}

bool filmReferenceXy(const film::FilmStock& s, double* x, double* y) {
  film::detail::Spec e;
  if (!film::detail::illuminant(s.referenceIlluminant, e)) return false;
  film::detail::xyOfXyz(film::detail::illuminantXyz(e), *x, *y);
  return true;
}

void scenePlacement(const FilmLatitude& lat, const SceneStats& st, float ev, float* belowToe, float* aboveShoulder) {
  if (belowToe) *belowToe = 0.f;
  if (aboveShoulder) *aboveShoulder = 0.f;
  if (!lat.ok || !(st.p995 > 0.f)) return;
  const float lHi = std::log10(st.p995 / 0.184f) + ev * 0.30103f;
  const float lLo = std::log10(std::max(st.p2, kShadowFloorY) / 0.184f) + ev * 0.30103f;
  if (belowToe) *belowToe = std::max(0.f, (lat.toe - lLo) / 0.30103f);
  if (aboveShoulder) *aboveShoulder = std::max(0.f, (lHi - lat.shoulder) / 0.30103f);
}

// ---- print-exposure calibrations on disk (wave 9) ----------------------------------------------------------------------------------------------------
// filmPrintConstant costs ~190 ms per (stock, paper, EV bucket) on a phone and does not depend on the photo: one 40-byte file per constant in the film table
// directory (setFilmCalibrationDisk, called with the table directory), keyed by the pack content hash + build salt + the cache key, checksummed, written
// atomically from a detached thread (never on the calling frame thread), a bad file is ignored and recomputed.
namespace {
struct CalibDisk {
  std::mutex m;
  std::string dir, salt;
};
CalibDisk& calibDisk() {
  static CalibDisk* c = new CalibDisk;
  return *c;
}
std::uint64_t calibFnv(const void* p, std::size_t n, std::uint64_t h = 14695981039346656037ull) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
std::string calibPath(const std::string& key, std::string& dirOut) {
  std::string salt;
  {
    CalibDisk& c = calibDisk();
    std::lock_guard<std::mutex> lock(c.m);
    dirOut = c.dir, salt = c.salt;
  }
  if (dirOut.empty()) {  // not configured by the app: the directory of tests and benches
    if (const char* e = std::getenv("DAPPLE_FILM_TABLE_DIR")) dirOut = e, salt = "env";
  }
  if (dirOut.empty() || std::getenv("DAPPLE_FILM_DISK_OFF")) return {};
  const std::uint64_t pack = film::libraryContentHash();
  std::uint64_t h = calibFnv(salt.data(), salt.size(), calibFnv(&pack, sizeof pack));
  h = calibFnv(key.data(), key.size(), h);
  char nm[64];
  std::snprintf(nm, sizeof nm, "calib_%016llx.bin", (unsigned long long)h);
  return (std::filesystem::path(dirOut) / nm).string();
}
// layout: magic u32, format u32, key hash u64, value f32, checksum u64 of the preceding bytes
bool calibLoad(const std::string& path, const std::string& key, float& v) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char b[28];
  in.read(b, sizeof b);
  if (in.gcount() != std::streamsize(sizeof b)) return false;
  std::uint32_t magic, ver;
  std::uint64_t kh, sum;
  float val;
  std::memcpy(&magic, b, 4), std::memcpy(&ver, b + 4, 4), std::memcpy(&kh, b + 8, 8), std::memcpy(&val, b + 16, 4), std::memcpy(&sum, b + 20, 8);
  if (magic != 0x42434644u || ver != 1 || kh != calibFnv(key.data(), key.size()) || sum != calibFnv(b, 20) || !std::isfinite(val)) {
    std::error_code ec;
    in.close();
    std::filesystem::remove(path, ec);
    return false;
  }
  v = val;
  return true;
}
void calibStore(const std::string& path, const std::string& key, float v) {
  char b[28];
  const std::uint32_t magic = 0x42434644u, ver = 1;
  const std::uint64_t kh = calibFnv(key.data(), key.size());
  std::memcpy(b, &magic, 4), std::memcpy(b + 4, &ver, 4), std::memcpy(b + 8, &kh, 8), std::memcpy(b + 16, &v, 4);
  const std::uint64_t sum = calibFnv(b, 20);
  std::memcpy(b + 20, &sum, 8);
  static const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tag));
  const std::string tmp = path + "." + tag + ".tmp";
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(b, sizeof b);
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
  }
}
}  // namespace
void setFilmCalibrationDisk(const std::string& dir, const std::string& salt) {
  CalibDisk& c = calibDisk();
  std::lock_guard<std::mutex> lock(c.m);
  c.dir = dir, c.salt = salt;
}

float filmPrintConstant(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& filmId, const std::string& paperId,
                        float deltaEv) try {
  static std::mutex mtx;
  static std::map<std::string, float> cache;
  if (!lib) return 0.f;
  const film::FilmStock* st = lib->find(filmId);
  if (!st || st->isPrintMedium()) return 0.f;
  const bool scan = paperId == "none" || !lib->find(paperId);
  const std::string key = filmId + "|" + (scan ? std::string("none") : paperId) + "|" + std::to_string(std::uintptr_t(lib.get())) +
                          (filmPaperMappingEnabled() ? "|m" : "") + (filmPaperMapStretchesBlack() ? "b" : "") + "|d" + std::to_string(int(std::lround(deltaEv * 1000.f)));
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  if (scan) {  // the clear film is display white (the paper mapping): a lamp constant would only dim it, the slide is what it is
    std::lock_guard<std::mutex> lock(mtx);
    cache[key] = 0.f;
    return 0.f;
  }
  std::string calibDir;
  std::string dkey = key;  // the disk key without the library pointer (the pack content hash is in the file name instead)
  {
    const std::string ps = std::to_string(std::uintptr_t(lib.get()));
    const auto pp = dkey.find("|" + ps);
    if (pp != std::string::npos) dkey.erase(pp + 1, ps.size());
  }
  const std::string calibFile = calibPath(dkey, calibDir);
  if (!calibFile.empty()) {
    float dv = 0.f;
    if (calibLoad(calibFile, dkey, dv)) {
      std::lock_guard<std::mutex> lock(mtx);
      cache[key] = dv;
      return dv;
    }
  }
  const FilmPrintRange range = filmPrintRange(lib, filmId, paperId);
  if (!range.ok) return 0.f;
  film::FilmParams fp;
  fp.film = filmId;
  fp.paper = scan ? std::string("none") : paperId;
  fp.camera.autoExposure = false;
  applyFilmPaperMapping(fp);
  fp.disableSpatialAndStochastic();
  film::FilmSimulator sim(lib);
  const float conv = st->positive() ? kFilmReversalBiasEv : kFilmNegativeBiasEv;
  const float g = std::exp2(conv + deltaEv);
  const float gain3[3] = {g, g, g};
  auto patch = [&](float printEv) -> float {
    fp.enlarger.printExposure = std::exp2(-double(printEv));
    std::string err;
    if (!sim.setParams(fp, &err)) return 0.f;
    ImageF in(8, 8), out;
    for (float& x : in.data) x = kFilmGreyCard;
    if (!sim.render(in, out, nullptr, gain3)) return 0.f;
    return medianLightness(out);
  };
  // mid grey: L* 50 relative to the paper white as displayed (Y = 0.184 * white Y)
  const float target = lightnessOfY(double(kFilmGreyCard) * double(range.yWhite));
  FilmPrintRange open;  // no limits: this is a calibration, not a target
  FilmMatchInfo mi;
  const float ev = solvePrintEv(patch, target, open, &mi);
  std::lock_guard<std::mutex> lock(mtx);
  cache[key] = ev;
  if (!calibFile.empty()) std::thread([calibFile, dkey, ev] { calibStore(calibFile, dkey, ev); }).detach();
  return ev;
} catch (...) {
  return 0.f;
}

float filmPrintConstantAt(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& filmId, const std::string& paperId,
                          float deltaEv) {
  if (std::fabs(deltaEv) < 1e-4f) return filmPrintConstant(lib, filmId, paperId, 0.f);
  const float step = 0.5f;
  const float lo = std::floor(deltaEv / step) * step;
  const float t = (deltaEv - lo) / step;
  const float a = filmPrintConstant(lib, filmId, paperId, lo);
  if (t < 1e-4f) return a;
  const float b = filmPrintConstant(lib, filmId, paperId, lo + step);
  return a + (b - a) * t;
}

float inRangeFraction(const SceneStats& st, const FilmLatitude& lat, float ev) {
  if (!lat.ok || st.logH0.empty()) return 1.f;
  const float shift = ev * 0.30103f;  // logH = logH0 + shift in [toe, shoulder]
  const auto lo = std::lower_bound(st.logH0.begin(), st.logH0.end(), lat.toe - shift);
  const auto hi = std::upper_bound(st.logH0.begin(), st.logH0.end(), lat.shoulder - shift);
  return float(std::max<std::ptrdiff_t>(0, hi - lo)) / float(st.logH0.size());
}

float rangeFractionFor(float l) {
  l = std::clamp(l, -1.f, 1.f);
  return l < 0.f ? kFilmRangeFraction + (kFilmRangeFractionMin - kFilmRangeFraction) * (-l)
                 : kFilmRangeFraction + (kFilmRangeFractionMax - kFilmRangeFraction) * l;
}

namespace {
// Rule A: keep the shared exposure when `target` of the scene is inside the range, else the nearest EV (smallest move) that reaches
// it, else the EV that maximises the share (ties: nearest to the shared exposure).
float placeEv(const SceneStats& st, const FilmLatitude& lat, float shared, float target) {
  if (!lat.ok || st.logH0.empty()) return shared;
  if (inRangeFraction(st, lat, shared) >= target) return shared;
  float bestReach = shared, bestReachDist = 1e9f, bestAny = shared, bestFrac = -1.f, bestAnyDist = 1e9f;
  for (int i = -120; i <= 120; ++i) {
    const float ev = shared + 0.05f * float(i);
    const float f = inRangeFraction(st, lat, ev), d = std::fabs(ev - shared);
    if (f >= target && d < bestReachDist) bestReach = ev, bestReachDist = d;
    if (f > bestFrac + 1e-6f || (std::fabs(f - bestFrac) <= 1e-6f && d < bestAnyDist)) bestAny = ev, bestFrac = f, bestAnyDist = d;
  }
  return bestReachDist < 1e8f ? bestReach : bestAny;
}

// film_shoot: between the EV where the share falls to kFilmShootEdgeFraction on the under side and on the over side
float shootEv(const SceneStats& st, const FilmLatitude& lat, float autoEv, float s) {
  s = std::clamp(s, -1.f, 1.f);
  if (s == 0.f || !lat.ok) return autoEv;
  const bool under = s < 0.f;
  const float dir = under ? -1.f : 1.f;
  float ev = autoEv;
  if (inRangeFraction(st, lat, autoEv) >= kFilmShootEdgeFraction) {
    while (std::fabs(ev + dir * 0.05f - autoEv) <= kFilmShootMaxEv && inRangeFraction(st, lat, ev + dir * 0.05f) >= kFilmShootEdgeFraction)
      ev += dir * 0.05f;
  } else {
    ev = autoEv + dir * 1.f;  // the scene does not fit this stock at all: a fixed, safe travel
  }
  return autoEv + std::fabs(s) * (ev - autoEv);
}

float targetDisplayL(const FilmPrintRange& r, float relL) {
  const float y = r.yWhite * float(yOfLightness(double(relL)));
  return std::clamp(lightnessOfY(double(y)), r.loL, r.hiL);
}
}  // namespace

FilmExposure solveMeter(const MeterInputs& in, FilmMatchInfo* info) try {
  FilmMatchInfo local;
  FilmMatchInfo& mi = info ? *info : local;
  const film::FilmStock* stock = in.lib ? in.lib->find(in.film) : nullptr;
  const bool reversal = stock && stock->positive();
  FilmLatitude lat;
  if (stock) lat = filmLatitude(*stock);
  mi.meterY = in.st0.logMean;
  mi.evMeter = meterEv(in.st0);
  mi.evScene = sceneKeyEv(in.keyL0);
  mi.keyL0 = in.keyL0;
  mi.p2 = in.stFull.p2, mi.p50 = in.stFull.p50, mi.p995 = in.stFull.p995, mi.clipFrac = in.stFull.clipFrac;
  const float conv = reversal ? kFilmReversalBiasEv : in.negativeBiasEv;
  const float shared = std::clamp(mi.evMeter + mi.evScene + conv, -6.f, 6.f);
  mi.sharedEv = shared;
  const float target = rangeFractionFor(in.latitude);
  float autoEv = placeEv(in.stFull, lat, shared, target);
  {  // DAPPLE_FILM_SLIDE_PROTECT=1: decision material, the v4 slide rule (the unclipped highlight at the shoulder margin)
    const char* pv = std::getenv("DAPPLE_FILM_SLIDE_PROTECT");
    float lo = 0.f, hi = 0.f;
    if (pv && pv[0] && pv[0] != '0' && reversal && filmExposureWindow(lat, in.stFull, lo, hi)) autoEv = hi;
  }
  float filmEv = std::clamp(shootEv(in.stFull, lat, autoEv, in.shoot), -9.f, 9.f);
  if (reversal && in.shoot != 0.f && in.medianL) {
    // a slide's brightness is its exposure: keep the override inside a usable median (L* 33 .. 86), bisecting towards the automatic EV
    auto usable = [&](float e) {
      const float m = in.medianL(e, filmPrintConstant(in.lib, in.film, in.paper, 0.f));
      return m >= kFilmSafeMedianMin && m <= kFilmSafeMedianMax;
    };
    if (!usable(filmEv)) {
      float good = autoEv, bad = filmEv;
      for (int i = 0; i < 7; ++i) {
        const float mid = 0.5f * (good + bad);
        (usable(mid) ? good : bad) = mid;
      }
      filmEv = good;
    }
  }
  mi.shootEv = filmEv;
  mi.ev = filmEv;
  mi.printDelta = filmEv - shared;
  mi.fracShared = inRangeFraction(in.stFull, lat, shared);
  mi.fracFinal = inRangeFraction(in.stFull, lat, filmEv);
  if (lat.ok && !in.stFull.logH0.empty()) {
    const float shift = filmEv * 0.30103f;
    const auto& v = in.stFull.logH0;
    mi.fracBelowToe = float(std::lower_bound(v.begin(), v.end(), lat.toe - shift) - v.begin()) / float(v.size());
    mi.fracAboveShoulder = float(v.end() - std::upper_bound(v.begin(), v.end(), lat.shoulder - shift)) / float(v.size());
    float lo = 0.f, hi = 0.f;
    if (filmExposureWindow(lat, in.stFull, lo, hi)) mi.latitudeOk = true, mi.evLo = lo, mi.evHi = hi;
    scenePlacement(lat, in.stFull, filmEv, &mi.stopsBelowToe, &mi.stopsAboveShoulder);
  }
  // print / scan exposure: the per stock + paper constant; a negative whose EV moved is printed down / up by the lab
  float printEv = reversal ? filmPrintConstant(in.lib, in.film, in.paper, 0.f)
                           : filmPrintConstantAt(in.lib, in.film, in.paper, mi.printDelta);
  mi.printConst = printEv;
  if (in.print != 0.f && in.medianL) {
    const FilmPrintRange range = filmPrintRange(in.lib, in.film, in.paper);
    if (range.ok) {
      const float s = std::clamp(in.print, -1.f, 1.f);
      const float lAuto = in.medianL(filmEv, printEv);
      const float lEnd = targetDisplayL(range, s < 0.f ? kFilmPrintDarkL : kFilmPrintLightL);
      // -1 never brightens and +1 never darkens (the automatic median may already lie beyond the end of the band)
      const float lTo = s < 0.f ? std::min(lEnd, lAuto) : std::max(lEnd, lAuto);
      const float tgt = lAuto + (lTo - lAuto) * std::fabs(s);
      auto pl = [&](float e) { return in.medianL(filmEv, e); };
      const float autoPrint = printEv;
      printEv = solvePrintEv(pl, tgt, range, &mi);
      printEv = s > 0.f ? std::max(printEv, autoPrint) : std::min(printEv, autoPrint);  // the end of the pad never moves the other way
    }
  }
  mi.printEv = printEv;
  return {filmEv, printEv};
} catch (...) {
  return {};
}

FilmPadInfo filmPadInfo(const MeterInputs& in) try {
  FilmPadInfo pad;
  const film::FilmStock* stock = in.lib ? in.lib->find(in.film) : nullptr;
  if (!stock) return pad;
  FilmMatchInfo mi;
  MeterInputs base = in;
  base.shoot = base.print = 0.f;
  const FilmExposure autoE = solveMeter(base, &mi);
  pad.ok = true;
  pad.sharedEv = mi.sharedEv;
  pad.finalEv = autoE.filmEv;
  pad.fractionAtFinal = mi.fracFinal;
  const FilmLatitude lat = filmLatitude(*stock);
  (void)lat;
  // the solved values at both ends (film_shoot: with the usable-median guard of a slide; film_print: with the lamp cap), and whether
  // each end differs from the automatic render
  MeterInputs lo = in, hi = in;
  lo.shoot = -1.f, hi.shoot = 1.f;
  lo.print = hi.print = 0.f;
  const FilmExposure sLo = solveMeter(lo), sHi = solveMeter(hi);
  pad.shootEv[0] = sLo.filmEv, pad.shootEv[1] = autoE.filmEv, pad.shootEv[2] = sHi.filmEv;
  pad.effectiveLo[0] = std::fabs(sLo.filmEv - autoE.filmEv) > 0.02f;
  pad.effectiveHi[0] = std::fabs(sHi.filmEv - autoE.filmEv) > 0.02f;
  {  // film_latitude: an end is effective only when the rescue moves the solved film EV (on most photos the share is already above the target)
    MeterInputs l0 = in, lLo = in, lHi = in;
    l0.shoot = lLo.shoot = lHi.shoot = 0.f;
    l0.print = lLo.print = lHi.print = 0.f;
    l0.latitude = 0.f, lLo.latitude = -1.f, lHi.latitude = 1.f;
    const FilmExposure e0 = solveMeter(l0), eLo = solveMeter(lLo), eHi = solveMeter(lHi);
    pad.effectiveLo[4] = std::fabs(eLo.filmEv - e0.filmEv) > 0.02f;
    pad.effectiveHi[4] = std::fabs(eHi.filmEv - e0.filmEv) > 0.02f;
  }
  for (int k = 0; k < 3; ++k) pad.latitude[k] = k == 0 ? kFilmRangeFractionMin : (k == 1 ? kFilmRangeFraction : kFilmRangeFractionMax);
  const FilmPrintRange range = filmPrintRange(in.lib, in.film, in.paper);
  if (range.ok) {
    pad.whiteL = range.lWhite;
    pad.blackL[0] = lightnessOfY(double(kFilmMatteBlackY));
    pad.blackL[1] = range.lBlack;
    pad.blackL[2] = lightnessOfY(double(0.00077));
    pad.printEv[1] = autoE.printEv;
    if (in.medianL) {
      MeterInputs pl = in, ph = in;
      pl.shoot = ph.shoot = 0.f;
      pl.print = -1.f, ph.print = 1.f;
      const FilmExposure eLo = solveMeter(pl), eHi = solveMeter(ph);
      pad.printEv[0] = eLo.printEv, pad.printEv[2] = eHi.printEv;
      pad.printL[0] = in.medianL(autoE.filmEv, eLo.printEv);
      pad.printL[1] = in.medianL(autoE.filmEv, autoE.printEv);
      pad.printL[2] = in.medianL(autoE.filmEv, eHi.printEv);
      pad.effectiveLo[1] = std::fabs(pad.printL[0] - pad.printL[1]) > 0.5f;
      pad.effectiveHi[1] = std::fabs(pad.printL[2] - pad.printL[1]) > 0.5f;
    }
  }
  const bool mapped = filmPaperMappingEnabled();
  pad.effectiveLo[2] = pad.effectiveHi[2] = mapped;  // film_black needs the paper mapping
  // film_wb: the assumed scene illuminant against the stock's reference illuminant; the axis does nothing when they coincide
  {
    double rx = 0, ry = 0;
    const bool haveRef = stock && filmReferenceXy(*stock, &rx, &ry);
    if (haveRef) pad.referenceCct = float(xyToCct(rx, ry));
    if (in.sceneX > 0 && in.sceneY > 0) pad.sceneCct = float(xyToCct(in.sceneX, in.sceneY));
    pad.effectiveLo[3] = false;  // 0 is the lowest film_wb: no negative end
    pad.effectiveHi[3] = haveRef && in.sceneX > 0 && std::hypot(in.sceneX - rx, in.sceneY - ry) > 0.004;
  }
  return pad;
} catch (...) {
  return {};
}

FilmPadInfo filmPadFor(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy) try {
  const FilmMatchOptions opt = FilmMatchOptions::fromEnv();
  if (p.filmStock.empty() || opt.mode != FilmMatchMode::Meter || proxy.linear.width <= 0) return {};
  std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  if (!lib) return {};
  EditParams ref = p;
  ref.filmStock.clear();
  ref.filmKeyEv = ref.filmPrintEv = 0.f;
  ref.clarity = ref.texture = ref.dehaze = ref.grain = 0.f;
  ref.sharpen = ref.nrLuma = 0.f;
  float gain[3], keyL0;
  {
    Renderer r(ref, camera, lut);
    r.setToneProxy(&proxy);
    r.sceneGain(proxy, gain);
    EditParams ref0 = ref;
    ref0.exposure = 0.f;
    keyL0 = medianLightness(renderMatchProxy(ref0, camera, lut, proxy));
  }
  float g0[3];
  for (int c = 0; c < 3; ++c) g0[c] = gain[c] / std::exp2(p.exposure);
  std::string paper = p.filmPaper;
  if (paper.empty() || (paper != "none" && !(lib->find(paper) && lib->find(paper)->isPrintMedium())))
    paper = film::defaultPaperFor(*lib, p.filmStock);
  auto fp = std::make_shared<EditParams>(p);
  fp->filmStrength = 100.f;
  fp->filmExposure = 0.f;
  fp->vignette = 0.f;
  MeterInputs in;
  in.lib = lib;
  in.film = p.filmStock;
  in.paper = paper;
  in.st0 = sceneLumaStats(proxy.linear, g0);
  in.stFull = sceneLumaStats(proxy.linear, gain);
  in.keyL0 = keyL0;
  in.shoot = p.filmShoot;
  in.print = p.filmPrint;
  in.latitude = p.filmLatitude;
  {
    EditParams wb1 = p;
    wb1.filmWb = 1.f;
    double sd;
    filmInputAdaptation(wb1, &in.sceneX, &in.sceneY, &sd);
  }
  in.negativeBiasEv = opt.negativeBiasEv;
  in.medianL = [fp, &camera, lut, &proxy](float fev, float pev) {
    fp->filmKeyEv = fev;
    fp->filmPrintEv = pev;
    return medianLightness(renderMatchProxy(*fp, camera, lut, proxy));
  };
  return filmPadInfo(in);
} catch (...) {
  return {};
}

FilmExposure matchFilmExposure(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy,
                               FilmMatchInfo* info, const FilmMatchOptions* optIn) try {
  if (info) *info = FilmMatchInfo{};
  if (p.filmStock.empty() || proxy.linear.width <= 0 || proxy.linear.height <= 0) return {};

  // the target: the normal develop render of this edit (film off), tonal analysis taken on this same proxy
  EditParams ref = p;
  ref.filmStock.clear();
  ref.filmKeyEv = 0.f;
  ref.filmPrintEv = 0.f;
  ref.clarity = ref.texture = ref.dehaze = ref.grain = 0.f;
  ref.sharpen = ref.nrLuma = 0.f;
  float target;
  float gain[3];
  {
    Renderer r(ref, camera, lut);
    r.setToneProxy(&proxy);
    target = medianLightness(renderMatchProxy(ref, camera, lut, proxy));
    r.sceneGain(proxy, gain);
  }

  // the film alone (strength 100), spatial stages off
  EditParams fp = p;
  fp.filmStrength = 100.f;
  fp.filmExposure = 0.f;
  fp.vignette = 0.f;
  fp.filmKeyEv = 0.f;
  fp.filmPrintEv = 0.f;
  auto render = [&](const EditParams& e) {
    return medianLightness(renderMatchProxy(e, camera, lut, proxy));
  };

  FilmMatchInfo local;
  FilmMatchInfo& mi = info ? *info : local;
  const FilmMatchOptions opt = optIn ? *optIn : FilmMatchOptions::fromEnv();
  const SceneStats st = sceneLumaStats(proxy.linear, gain);
  std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  const film::FilmStock* stock = lib ? lib->find(p.filmStock) : nullptr;

  if (opt.mode == FilmMatchMode::Pull || opt.mode == FilmMatchMode::Full) {  // v2 / v3: one EV on the film, the digital median as target
    auto evalAt = [&](float ev) {
      fp.filmKeyEv = ev;
      return render(fp);
    };
    const float evMatch = solveFilmKeyEv(evalAt, target, &mi);
    return {resolveFilmKeyEv(stock, st, evMatch, opt, &mi), 0.f};
  }

  std::string paper = p.filmPaper;
  if (lib && (paper.empty() || (paper != "none" && !(lib->find(paper) && lib->find(paper)->isPrintMedium()))))
    paper = film::defaultPaperFor(*lib, p.filmStock);

  if (opt.mode == FilmMatchMode::Window) {  // v4: A (latitude window) and B (print solved to the key) per image and stock
    const float filmEv = chooseFilmEv(stock, st, opt, &mi);
    mi.targetL = target;
    const FilmPrintRange range = filmPrintRange(lib, p.filmStock, paper);
    fp.filmKeyEv = filmEv;
    mi.filmL0 = render(fp);
    auto printL = [&](float ev) {
      fp.filmPrintEv = ev;
      return render(fp);
    };
    const float printEv = solvePrintEv(printL, target, range, &mi);
    mi.filmL = mi.printL;
    mi.ev = filmEv;
    return {filmEv, printEv};
  }

  // v5: metering (stock independent) -> film exposure; print = per stock + paper constant (film_match.h, solveMeter)
  EditParams ref0 = ref;
  ref0.exposure = 0.f;
  float keyL0;
  {
    keyL0 = medianLightness(renderMatchProxy(ref0, camera, lut, proxy));
  }
  float g0[3];
  const float expGain = std::exp2(p.exposure);
  for (int c = 0; c < 3; ++c) g0[c] = gain[c] / expGain;  // the scene as the camera recorded it, before the develop exposure
  MeterInputs mi_in;
  mi_in.lib = lib;
  mi_in.film = p.filmStock;
  mi_in.paper = paper;
  mi_in.st0 = sceneLumaStats(proxy.linear, g0);
  mi_in.stFull = st;
  mi_in.keyL0 = keyL0;
  mi_in.shoot = p.filmShoot;
  mi_in.print = p.filmPrint;
  mi_in.latitude = p.filmLatitude;
  mi_in.negativeBiasEv = opt.negativeBiasEv;
  mi_in.medianL = [&](float fev, float pev) {
    fp.filmKeyEv = fev;
    fp.filmPrintEv = pev;
    return render(fp);
  };
  const FilmExposure fe = solveMeter(mi_in, &mi);
  mi.targetL = target;
  fp.filmKeyEv = fe.filmEv;
  fp.filmPrintEv = fe.printEv;
  mi.printL = mi.filmL = render(fp);
  return fe;
} catch (...) {
  return {};
}

}  // namespace dapple
