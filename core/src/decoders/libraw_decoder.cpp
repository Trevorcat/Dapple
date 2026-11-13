// RAW decoding through LibRaw. Covers Sony ARW, Fujifilm RAF (X-Trans),
// Canon, Nikon, DNG and the rest of LibRaw's camera list.

#include <libraw/libraw.h>

#include "../sysperf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include <stb_image.h>

#include "dapple/illuminant.h"
#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic_session.h"
#include "decoders/dng_illuminant.h"
#include "decoders/dng_jxl.h"
#include "decoders/dng_opcodes.h"
#include "exposure_anchor.h"
#include "file_util.h"
#include "color.h"
#include "parallel.h"
#include "decoders/jpeg_dc.h"
#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif

namespace dapple {

namespace {

std::string sonyRawFormat(const libraw_sony_info_t& s) {
  std::string kind;
  switch (s.SonyRawFileType) {
    case 0: kind = "uncompressed 14-bit"; break;
    case 1: kind = "uncompressed 12-bit"; break;
    case 2: kind = "compressed (lossy)"; break;
    case 3: kind = "lossless compressed"; break;
    case 4: kind = "lossless compressed v2"; break;
    default: return {};
  }
  switch (s.RawSizeType) {
    case 1: kind += " (L)"; break;
    case 2: kind += " (S)"; break;
    case 3: kind += " (M)"; break;
    default: break;
  }
  return kind;
}

// Fujifilm film simulation codes (FilmMode maker note), best effort.
std::string fujiFilmSimulation(unsigned mode) {
  switch (mode) {
    case 0x000: return "Provia/Standard";
    case 0x100: case 0x110: case 0x120: case 0x130: case 0x300: return "Studio Portrait";
    case 0x200: case 0x400: return "Velvia/Vivid";
    case 0x500: return "Pro Neg. Std";
    case 0x501: return "Pro Neg. Hi";
    case 0x600: return "Classic Chrome";
    case 0x700: return "Eterna";
    case 0x800: return "Classic Negative";
    case 0x900: return "Eterna Bleach Bypass";
    case 0xa00: return "Nostalgic Neg";
    case 0xb00: return "Reala Ace";
    default: return {};
  }
}

// DNG default crop (DNG specification: DefaultCropOrigin / DefaultCropSize, relative to the ActiveArea) in active-area pixels,
// origin and size rounded to even numbers so that the half-size preview crops to exactly half of it. False when the file is not a
// DNG, records no crop, or the crop covers the whole active area. (Sony writes the crop tags raw-relative into raw_inset_crops, and
// Sony DNGs are not the phone case: left to LibRaw metadata.)
bool dngDefaultCrop(const LibRaw& raw, int& x0, int& y0, int& w, int& h) {
  const auto& d = raw.imgdata;
  if (d.idata.dng_version == 0 || d.sizes.raw_inset_crops[0].cwidth == 0 || d.sizes.raw_inset_crops[0].cheight == 0) return false;
  if (std::strncmp(d.idata.make, "SONY", 4) == 0 || std::strncmp(d.idata.make, "Sony", 4) == 0) return false;
  const auto& c = d.sizes.raw_inset_crops[0];
  int cx = int(c.cleft) - int(d.sizes.left_margin), cy = int(c.ctop) - int(d.sizes.top_margin);
  int cw = int(c.cwidth), ch = int(c.cheight);
  if (cx < 0 || cy < 0 || cx + cw > int(d.sizes.width) || cy + ch > int(d.sizes.height)) return false;
  const int ex = cx & ~1, ey = cy & ~1;
  cw = (cw + (cx - ex)) & ~1, ch = (ch + (cy - ey)) & ~1;
  cx = ex, cy = ey;
  cw = std::min(cw, int(d.sizes.width) - cx), ch = std::min(ch, int(d.sizes.height) - cy);
  if (cw < 16 || ch < 16 || (cx == 0 && cy == 0 && cw >= int(d.sizes.width) - 1 && ch >= int(d.sizes.height) - 1)) return false;
  x0 = cx, y0 = cy, w = cw, h = ch;
  return true;
}

bool anchorEnabled() {  // DAPPLE_ANCHOR=off: the profile's baselineExposureEv only (A/B); read on every call (tests switch it)
  const char* e = std::getenv("DAPPLE_ANCHOR");
  return !(e && (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0));
}

// DNG BaselineExposureOffset (tag 51109, SRATIONAL, 1.7+), which LibRaw does not expose: read from the first IFD; NaN = absent.
float dngBaselineOffsetEv(const std::filesystem::path& path);

bool dngOpcodesEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("DAPPLE_DNG_OPCODES");
    return !(e && e[0] == '0');
  }();
  return on;
}

dng::OpcodeList dngList(const LibRaw& raw, int i) {
  const auto& o = raw.imgdata.color.dng_levels.rawopcodes[i];
  return o.data && o.len ? dng::parseOpcodeList(o.data, o.len) : dng::OpcodeList{};
}

int gcdInt(int a, int b) { return b ? gcdInt(b, a % b) : a; }

// Black-level pattern of the raw buffer for the gain maps (LibRaw adds the common level, the per-colour level and the repeat pattern).
void fillBlackPattern(LibRaw& raw, dng::MosaicView& v) {
  const auto& cm = raw.imgdata.color;
  const int br = int(cm.cblack[4]), bc = int(cm.cblack[5]);
  const bool pat = br > 0 && bc > 0 && br * bc <= int(LIBRAW_CBLACK_SIZE) - 6;
  const int pr = pat ? 2 * br / gcdInt(2, br) : 2, pc = pat ? 2 * bc / gcdInt(2, bc) : 2;
  v.bpr = pr, v.bpc = pc;
  v.black.assign(std::size_t(pr) * pc, 0.f);
  for (int r = 0; r < pr; ++r)
    for (int c = 0; c < pc; ++c) {
      const int ch = raw.COLOR(r, c) & 3;
      v.black[std::size_t(r) * pc + c] = float(cm.black) + float(cm.cblack[ch]) + (pat ? float(cm.cblack[6 + (r % br) * bc + (c % bc)]) : 0.f);
    }
}

ImageF rotateFlip(const ImageF& in, int flip) {
  if (flip != 3 && flip != 5 && flip != 6) return in;
  const int W = in.width, H = in.height;
  ImageF out = flip == 3 ? ImageF(W, H) : ImageF(H, W);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      int dx, dy;
      if (flip == 3) dx = W - 1 - x, dy = H - 1 - y;
      else if (flip == 6) dx = H - 1 - y, dy = x;  // 90 degrees clockwise
      else dx = y, dy = W - 1 - x;                 // 90 degrees counter-clockwise
      std::memcpy(out.row(dy) + std::size_t(dx) * 3, in.row(y) + std::size_t(x) * 3, 3 * sizeof(float));
    }
  return out;
}

// AsShotWhiteXY (DNG tag 50729) of the first IFD: LibRaw does not expose it, and a DNG may record it instead of AsShotNeutral.
bool readDngWhiteXy(const std::filesystem::path& path, double* xy) {
  std::ifstream f(path, std::ios::binary);
  unsigned char h[8];
  if (!f.read(reinterpret_cast<char*>(h), 8)) return false;
  const bool le = h[0] == 'I' && h[1] == 'I';
  if (!le && !(h[0] == 'M' && h[1] == 'M')) return false;
  auto u16 = [&](const unsigned char* p) { return le ? unsigned(p[0] | p[1] << 8) : unsigned(p[1] | p[0] << 8); };
  auto u32 = [&](const unsigned char* p) { return le ? (unsigned(p[0]) | unsigned(p[1]) << 8 | unsigned(p[2]) << 16 | unsigned(p[3]) << 24) : (unsigned(p[3]) | unsigned(p[2]) << 8 | unsigned(p[1]) << 16 | unsigned(p[0]) << 24); };
  if (u16(h + 2) != 42) return false;
  f.seekg(u32(h + 4));
  unsigned char nb[2];
  if (!f.read(reinterpret_cast<char*>(nb), 2)) return false;
  const unsigned n = u16(nb);
  for (unsigned i = 0; i < n && i < 512; ++i) {
    unsigned char e[12];
    if (!f.read(reinterpret_cast<char*>(e), 12)) return false;
    if (u16(e) != 50729 || u16(e + 2) != 5 || u32(e + 4) != 2) continue;
    f.seekg(u32(e + 8));
    unsigned char v[16];
    if (!f.read(reinterpret_cast<char*>(v), 16)) return false;
    const double d0 = u32(v + 4), d1 = u32(v + 12);
    if (d0 <= 0 || d1 <= 0) return false;
    xy[0] = u32(v) / d0, xy[1] = u32(v + 8) / d1;
    return true;
  }
  return false;
}

float dngBaselineOffsetEv(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  unsigned char h[8];
  if (!f.read(reinterpret_cast<char*>(h), 8)) return std::numeric_limits<float>::quiet_NaN();
  const bool le = h[0] == 'I' && h[1] == 'I';
  if (!le && !(h[0] == 'M' && h[1] == 'M')) return std::numeric_limits<float>::quiet_NaN();
  auto u16 = [&](const unsigned char* p) { return le ? unsigned(p[0] | p[1] << 8) : unsigned(p[1] | p[0] << 8); };
  auto u32 = [&](const unsigned char* p) { return le ? (unsigned(p[0]) | unsigned(p[1]) << 8 | unsigned(p[2]) << 16 | unsigned(p[3]) << 24) : (unsigned(p[3]) | unsigned(p[2]) << 8 | unsigned(p[1]) << 16 | unsigned(p[0]) << 24); };
  if (u16(h + 2) != 42) return std::numeric_limits<float>::quiet_NaN();
  f.seekg(u32(h + 4));
  unsigned char nb[2];
  if (!f.read(reinterpret_cast<char*>(nb), 2)) return std::numeric_limits<float>::quiet_NaN();
  const unsigned n = u16(nb);
  for (unsigned i = 0; i < n && i < 512; ++i) {
    unsigned char e[12];
    if (!f.read(reinterpret_cast<char*>(e), 12)) break;
    if (u16(e) != 51109 || u16(e + 2) != 10 || u32(e + 4) != 1) continue;
    f.seekg(u32(e + 8));
    unsigned char v[8];
    if (!f.read(reinterpret_cast<char*>(v), 8)) break;
    const double den = double(std::int32_t(u32(v + 4)));
    if (den == 0) break;
    return float(double(std::int32_t(u32(v))) / den);
  }
  return std::numeric_limits<float>::quiet_NaN();
}

ShotMetadata readMetadata(const LibRaw& raw, const double* whiteXy = nullptr) {
  const auto& d = raw.imgdata;
  ShotMetadata m;
  m.make = d.idata.normalized_make[0] ? d.idata.normalized_make : d.idata.make;
  m.model = d.idata.normalized_model[0] ? d.idata.normalized_model : d.idata.model;
  m.lens = d.lens.Lens;
  m.iso = d.other.iso_speed;
  m.shutterSeconds = d.other.shutter;
  m.aperture = d.other.aperture;
  m.focalLengthMm = d.other.focal_len;
  m.timestamp = static_cast<long long>(d.other.timestamp);

  m.monochrome = d.idata.colors == 1;
  const bool swap = (d.sizes.flip & 4) != 0;
  int sw = d.sizes.width, sh = d.sizes.height;
  {
    int cx, cy, cw, ch;
    if (dngOpcodesEnabled() && dngDefaultCrop(raw, cx, cy, cw, ch)) sw = cw, sh = ch;  // the decode crops to the DNG default crop
  }
  m.sensorWidth = swap ? sh : sw;
  m.sensorHeight = swap ? sw : sh;

  if (d.idata.filters == 9) {
    m.cfa = CfaKind::XTrans;
  } else if (d.idata.filters != 0) {
    m.cfa = CfaKind::Bayer;
  } else if (d.idata.dng_version != 0) {
    m.cfa = CfaKind::None;  // linear DNG
  } else if (m.make == "Sony" && (d.makernotes.sony.RawSizeType == 2 || d.makernotes.sony.RawSizeType == 3)) {
    // Sony M/S RAW (e.g. A7R V "M" = 26 MP) is a binned, already 3-colour
    // image without a CFA mosaic, so half-size decoding gains nothing.
    m.cfa = CfaKind::None;
  } else if (d.idata.colors == 3 && d.idata.filters == 0) {
    m.cfa = CfaKind::None;  // already three-colour, no mosaic: Canon sRAW / mRAW (YCbCr 4:2:2, LibRaw reports filters 0)
  } else {
    // LibRaw has not resolved the pattern at this stage; the camera profile
    // knows the sensor type.
    m.cfa = CfaKind::Unknown;
  }
  if (d.color.raw_bps > 0 && d.color.raw_bps <= 32) {
    m.bitsPerSample = int(d.color.raw_bps);
  } else {
    const unsigned maxValue = d.color.maximum;
    m.bitsPerSample = maxValue > 0 ? int(std::ceil(std::log2(double(maxValue) + 1))) : 0;
  }

  // As-shot multipliers: the file's AsShotNeutral. DNGs that record only AsShotWhiteXY (Pixel 8 Pro) leave cam_mul empty; their
  // multipliers come from the white point below.
  float camMul[4] = {d.color.cam_mul[0], d.color.cam_mul[1], d.color.cam_mul[2], d.color.cam_mul[3]};
  const float g = camMul[1] > 0 ? camMul[1] : 1.f;
  for (int c = 0; c < 3; ++c) m.asShotWb[c] = camMul[c] > 0 ? camMul[c] / g : 1.f;
  bool haveDual = false;
  {  // DNG files: both calibration illuminants' matrices + the as-shot neutral, the iterative solve of DNG specification chapter 6
    const auto& dc = d.color;
    auto hasCm = [&](int i) {  // LibRaw copies the matrices without their parsed-field flags: a populated matrix is the test
      double sum = 0;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) sum += std::fabs(double(dc.dng_color[i].colormatrix[r][c]));
      return sum > 1e-6;
    };
    const bool cmOk = d.idata.dng_version != 0 && d.idata.colors == 3 && hasCm(0) && hasCm(1) && dc.dng_color[0].illuminant > 0 && dc.dng_color[1].illuminant > 0;
    const auto& an = dc.dng_levels.asshotneutral;
    const bool haveAn = an[0] > 0 && an[1] > 0 && an[2] > 0;
    const bool haveWhite = !haveAn && whiteXy && whiteXy[0] > 0.0 && whiteXy[1] > 0.0 && whiteXy[0] + whiteXy[1] < 1.0;
    if (cmOk && (haveAn || haveWhite)) {
      DngCalibration cal[2];
      bool ok = true;
      for (int i = 0; i < 2; ++i) {
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) {
            cal[i].cm[r * 3 + c] = dc.dng_color[i].colormatrix[r][c];
            if (std::fabs(double(dc.dng_color[i].calibration[0][0])) > 1e-6 && std::fabs(double(dc.dng_color[i].calibration[1][1])) > 1e-6 &&
                std::fabs(double(dc.dng_color[i].calibration[2][2])) > 1e-6)
              cal[i].cc[r * 3 + c] = dc.dng_color[i].calibration[r][c];
          }
        cal[i].cct = illuminantCodeCct(dc.dng_color[i].illuminant);
        ok = ok && cal[i].cct > 0;
      }
      if (ok && cal[0].cct > cal[1].cct) std::swap(cal[0], cal[1]);  // spec: calibration 1 is the lower temperature; the weights do not care, the order does for clarity
      double ab[3] = {1, 1, 1};
      for (int c = 0; c < 3; ++c)
        if (dc.dng_levels.analogbalance[c] > 0) ab[c] = dc.dng_levels.analogbalance[c];
      double neutral[3] = {an[0], an[1], an[2]};
      double x = 0, y = 0;
      if (haveWhite && ok) {  // the recorded white point is the illuminant; the neutral follows from the forward model of chapter 6
        x = whiteXy[0], y = whiteXy[1];
        const double T = std::clamp(xyToCct(x, y), 1500.0, 20000.0);
        const double w = std::clamp((1.0 / T - 1.0 / cal[1].cct) / (1.0 / cal[0].cct - 1.0 / cal[1].cct), 0.0, 1.0);
        double mx[9];
        detail::dngXyzToCamera(cal, 2, w, ab, mx);
        const double XYZ[3] = {x / y, 1.0, (1.0 - x - y) / y};
        for (int r = 0; r < 3; ++r) neutral[r] = mx[r * 3] * XYZ[0] + mx[r * 3 + 1] * XYZ[1] + mx[r * 3 + 2] * XYZ[2];
        if (neutral[0] > 0 && neutral[1] > 0 && neutral[2] > 0) {
          for (int c = 0; c < 3; ++c) m.asShotWb[c] = float(neutral[1] / neutral[c]);
          m.asShotIlluminantXy[0] = float(x), m.asShotIlluminantXy[1] = float(y);
          m.asShotCct = float(T);
          m.illuminantSource = "dng-whitexy";
          haveDual = true;
        }
      } else if (ok && solveIlluminantFromNeutral(cal, 2, ab, neutral, x, y)) {
        m.asShotIlluminantXy[0] = float(x), m.asShotIlluminantXy[1] = float(y);
        m.asShotCct = float(std::clamp(xyToCct(x, y), 1500.0, 20000.0));
        m.illuminantSource = "dng-dual";
        haveDual = true;
      }
    }
  }
  if (!haveDual) {  // as-shot illuminant chromaticity: the camera's neutral (1 / multipliers) through the inverse of the XYZ -> camera matrix
    const float(*cx)[3] = d.color.cam_xyz;
    const double a = cx[0][0], b = cx[0][1], cc = cx[0][2], dd = cx[1][0], e = cx[1][1], f = cx[1][2], gg = cx[2][0], h = cx[2][1], k = cx[2][2];
    const double det = a * (e * k - f * h) - b * (dd * k - f * gg) + cc * (dd * h - e * gg);
    if (std::fabs(det) > 1e-9 && camMul[0] > 0 && camMul[1] > 0 && camMul[2] > 0) {
      const double n[3] = {1.0 / camMul[0], 1.0 / camMul[1], 1.0 / camMul[2]};
      const double inv[9] = {(e * k - f * h) / det, (cc * h - b * k) / det, (b * f - cc * e) / det,
                             (f * gg - dd * k) / det, (a * k - cc * gg) / det, (cc * dd - a * f) / det,
                             (dd * h - e * gg) / det, (b * gg - a * h) / det, (a * e - b * dd) / det};
      const double X = inv[0] * n[0] + inv[1] * n[1] + inv[2] * n[2], Y = inv[3] * n[0] + inv[4] * n[1] + inv[5] * n[2],
                   Z = inv[6] * n[0] + inv[7] * n[1] + inv[8] * n[2];
      const double sum = X + Y + Z;
      if (sum > 1e-9 && X > 0 && Y > 0 && Z > 0) m.asShotIlluminantXy[0] = float(X / sum), m.asShotIlluminantXy[1] = float(Y / sum);
    }
    // The matrix is the file's single (D65-based) XYZ -> camera matrix, so the estimate is biased cool towards warm light (dual-illuminant
    // effect: -20 .. -39 mired on 14 Sony ARWs against the camera's own Kelvin table). Where the camera records its own Kelvin white-balance
    // table (LibRaw WBCT_Coeffs: Sony) the correlated colour temperature comes from it (the as-shot multipliers projected onto the table in
    // log(R/G), log(B/G) against mired); the matrix estimate only supplies the tint, its offset from the locus at its own temperature.
    if (m.asShotIlluminantXy[0] > 0.f && camMul[0] > 0 && camMul[1] > 0 && camMul[2] > 0) {
      struct Pt { double mired, lr, lb; };
      std::vector<Pt> tab;
      for (int i = 0; i < 64; ++i) {
        const float* w = d.color.WBCT_Coeffs[i];
        if (w[0] > 500.f && w[1] > 0.f && w[2] > 0.f && w[3] > 0.f) tab.push_back({1e6 / double(w[0]), std::log(double(w[1]) / double(w[2])), std::log(double(w[3]) / double(w[2]))});
      }
      std::sort(tab.begin(), tab.end(), [](const Pt& a, const Pt& b) { return a.mired < b.mired; });
      if (std::getenv("DAPPLE_ILL_MATRIX_ONLY")) tab.clear();
      const double xm = m.asShotIlluminantXy[0], ym = m.asShotIlluminantXy[1], Tm = dapple::xyToCct(xm, ym);
      double Tc = Tm;
      if (tab.size() >= 2) {
        const double ql = std::log(double(camMul[0]) / double(camMul[1])), qb = std::log(double(camMul[2]) / double(camMul[1]));
        double bestErr = 1e30, bestMired = 1e6 / Tm;
        for (std::size_t i = 0; i + 1 < tab.size(); ++i) {
          const double dl = tab[i + 1].lr - tab[i].lr, db = tab[i + 1].lb - tab[i].lb, den = dl * dl + db * db;
          if (den <= 0) continue;
          const double t = std::clamp(((ql - tab[i].lr) * dl + (qb - tab[i].lb) * db) / den, 0.0, 1.0);
          const double el = ql - (tab[i].lr + t * dl), eb = qb - (tab[i].lb + t * db), err = el * el + eb * eb;
          if (err < bestErr) bestErr = err, bestMired = tab[i].mired + t * (tab[i + 1].mired - tab[i].mired);
        }
        Tc = 1e6 / bestMired;
        double lx, ly, mx, my;
        dapple::cctToXy(Tc, lx, ly);
        dapple::cctToXy(Tm, mx, my);
        m.asShotIlluminantXy[0] = float(lx + (xm - mx));
        m.asShotIlluminantXy[1] = float(ly + (ym - my));
      }
      m.asShotCct = float(Tc);
      m.illuminantSource = tab.size() >= 2 ? "wbct" : "matrix";
    }
  }

  if (m.make == "Sony") m.rawFormat = sonyRawFormat(d.makernotes.sony);
  if (m.make == "Fujifilm" && (d.makernotes.fuji.DevelopmentDynamicRange > 0 || d.makernotes.fuji.DynamicRange > 0)) {  // DR modes expose the RAW lower on purpose
    char b[96];
    std::snprintf(b, sizeof b, "DR%u, exposure mid-point shift %+.1f", unsigned(d.makernotes.fuji.DevelopmentDynamicRange ? d.makernotes.fuji.DevelopmentDynamicRange : d.makernotes.fuji.DynamicRange), d.makernotes.fuji.ExpoMidPointShift);
    m.rawFormat = b;
  }
  if (d.idata.raw_count > 1) {  // pixel-shift / multi-shot files: LibRaw decodes the first frame only, say so
    if (!m.rawFormat.empty()) m.rawFormat += ", ";
    m.rawFormat += "multi-shot file (" + std::to_string(d.idata.raw_count) + " frames), frame 1 decoded, no composite";
  }
  if (m.make == "Fujifilm") m.inCameraLook = fujiFilmSimulation(d.makernotes.fuji.FilmMode);
  return m;
}

// LibRaw datastream over std::filebuf opened with a std::filesystem::path, so
// non-ASCII paths work on Windows and LibRaw only reads the bytes it needs
// (no 60-120 MB copy for probing or thumbnail extraction).
class PathDatastream final : public LibRaw_abstract_datastream {
 public:
  explicit PathDatastream(const std::filesystem::path& path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (!ec) size_ = static_cast<INT64>(sz);
    buf_.open(path, std::ios::in | std::ios::binary);
  }
  int valid() override { return buf_.is_open() ? 1 : 0; }
  int read(void* ptr, size_t size, size_t nmemb) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    return int(buf_.sgetn(static_cast<char*>(ptr), std::streamsize(size * nmemb)) / (size > 0 ? size : 1));
  }
  int seek(INT64 o, int whence) override {
    if (!buf_.is_open()) throw LIBRAW_EXCEPTION_IO_EOF;
    std::ios_base::seekdir dir = whence == SEEK_CUR ? std::ios_base::cur
                                 : whence == SEEK_END ? std::ios_base::end
                                                      : std::ios_base::beg;
    return buf_.pubseekoff(std::streamoff(o), dir) < 0;
  }
  INT64 tell() override { return buf_.pubseekoff(0, std::ios_base::cur); }
  INT64 size() override { return size_; }
  int get_char() override { return buf_.sbumpc(); }
  char* gets(char* str, int sz) override {
    if (sz < 1) return nullptr;
    std::istream is(&buf_);
    is.getline(str, sz);
    return is.fail() ? nullptr : str;
  }
  int scanf_one(const char* fmt, void* val) override {
    std::istream is(&buf_);
    if (std::strcmp(fmt, "%d") == 0) {
      int d;
      is >> d;
      if (is.fail()) return EOF;
      *static_cast<int*>(val) = d;
    } else {
      float f;
      is >> f;
      if (is.fail()) return EOF;
      *static_cast<float*>(val) = f;
    }
    return 1;
  }
  int eof() override { return buf_.sgetc() == EOF; }

 private:
  std::filebuf buf_;
  INT64 size_ = 0;
};

// Deep-shadow chroma floor for near-empty frames. LibRaw (like every converter that works in unsigned integers) clips the black-subtracted noise at
// zero, which gives each channel a positive bias of about 0.4 sigma; the white balance multipliers (R x 2, B x 1.8) turn that into a magenta cast
// in pixels that hold no signal (a Galaxy Note10+ frame at 0.5 - 2 DN above black with sigma 7 DN came out R 0.0070 / G 0.0041 / B 0.0097).
// Rule: only a frame whose 99.5th percentile luminance is below kEmptyP995 (0.03 of white, 5 stops below it: a black / noise frame, nothing in it
// that is a photograph) is touched; there, linear luma below kShadowFloor fades to neutral (luma kept, smoothstep). Measured on 14 dark frames of
// the owner's A7R V library (daytime and dusk shots with 12 - 40 % of the pixels below L* 20) the unconditional version changed 12 - 40 % of the
// pixels by dE > 0.5 and removed up to 26 C* from legitimately coloured shadows (dark fabric, dusk foliage), so it must not run on frames that have
// a real exposure; none of those frames passes the gate (their gated renders are byte-identical to the floor-off ones).
void suppressShadowChroma(ImageF& img) {
  static const bool off = [] { const char* e = std::getenv("DAPPLE_SHADOW_CHROMA"); return e && (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0); }();
  if (off) return;  // A/B switch
  constexpr float kShadowFloor = 0.008f, kEmptyP995 = 0.03f;
  {
    // 99.5th percentile of the luma on a 1/16 subsample
    std::vector<float> ls;
    ls.reserve(std::size_t(img.width / 4 + 1) * std::size_t(img.height / 4 + 1));
    for (int y = 0; y < img.height; y += 4) {
      const float* p = img.row(y);
      for (int x = 0; x < img.width; x += 4) ls.push_back(0.2880f * p[3 * x] + 0.7119f * p[3 * x + 1] + 0.0001f * p[3 * x + 2]);
    }
    if (ls.empty()) return;
    const std::size_t k = std::min(ls.size() - 1, std::size_t(0.995 * double(ls.size())));
    std::nth_element(ls.begin(), ls.begin() + std::ptrdiff_t(k), ls.end());
    if (ls[k] >= kEmptyP995) return;  // a photograph: leave the shadows alone
  }
  const int h = img.height, w = img.width;
  parallelRows(h, [&](int y) {
    float* p = img.row(y);
    for (int x = 0; x < w; ++x, p += 3) {
      const float l = 0.2880f * p[0] + 0.7119f * p[1] + 0.0001f * p[2];
      if (l >= kShadowFloor) continue;
      const float t = std::max(l, 0.f) / kShadowFloor, k = t * t * (3.f - 2.f * t);
      for (int c = 0; c < 3; ++c) p[c] = l + (p[c] - l) * k;
    }
  });
}

// the file's exposure anchor (core/src/exposure_anchor.h) into out.meta: an explicit reference first, else the embedded preview, else a sibling camera JPEG
void attachExposureAnchor(SourceImage& out, const std::filesystem::path& path, const DecodeOptions& options, float dngBaselineEv, std::future<Result<Image8>>* embeddedFuture = nullptr) {
  // camera RAW (not a Dapple linear merge): the file's own exposure anchor
  // reference of this frame: an explicit one first, else the embedded preview, else a sibling camera JPEG (RAW+JPEG)
  ExposureAnchor an;
  std::optional<Image8> ref;
  if (!options.referenceImage.empty()) {
  if (auto r = loadReferenceJpeg(options.referenceImage)) ref = std::move(*r);
  }
  std::optional<Image8> emb;
  if (embeddedFuture && embeddedFuture->valid()) {
    auto r = embeddedFuture->get();  // extracted on a second thread while LibRaw decoded
    if (r) emb = std::move(*r);
  }
  an = computeExposureAnchor(path, out.linear, dngBaselineEv, ref ? &*ref : nullptr, emb ? &*emb : nullptr, out.meta.monochrome);
  if (an.tier != 1 && !ref) {
  for (const char* ext : {".jpg", ".JPG", ".jpeg", ".JPEG"}) {
    std::filesystem::path sib = path;
    sib.replace_extension(ext);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sib, ec)) continue;
    auto r = loadReferenceJpeg(sib);
    if (!r) continue;
    const ExposureAnchor a2 = computeExposureAnchor(path, out.linear, dngBaselineEv, &*r, nullptr, out.meta.monochrome);
    if (a2.tier == 1) {
      an = a2;
      break;
    }
  }
  }
  out.meta.exposureAnchorTier = an.tier;
  out.meta.exposureAnchorEv = an.ev;
  out.meta.exposureAnchorConfidence = an.confidence;
  out.meta.exposureAnchorNote = an.note;
  out.meta.exposureAnchorSource = an.source;
  out.meta.exposureAnchorPreviewEv = an.previewEv;
  out.meta.exposureAnchorStatsEv = an.statsEv;
  out.meta.exposureAnchorHasDng = std::isfinite(dngBaselineEv);
  out.meta.exposureAnchorDngEv = std::isfinite(dngBaselineEv) ? dngBaselineEv : 0.f;
  }

// DNG 1.7 JPEG XL files (LinearRaw, three samples per pixel): LibRaw cannot unpack them, so the samples come from libjxl (dng_jxl.h) and the
// conversion LibRaw would do is done here: black / white level, as-shot white balance, DNG ForwardMatrix (or the inverse ColorMatrix) to
// XYZ D50, then ProPhoto. LibRaw is still used to open the file for the metadata and the colour tags.
Result<SourceImage> decodeJxlDngFile(LibRaw& raw, const std::filesystem::path& path, const DecodeOptions& options) {
  const auto& d = raw.imgdata;
  dng::JxlRaw jr = dng::decodeJxlDng(path);
  if (!jr.error.empty()) return fail("JPEG XL DNG: " + jr.error);
  if (jr.channels != 3) return fail("JPEG XL DNG with " + std::to_string(jr.channels) + " sample(s) per pixel (CFA mosaic) is not supported yet");
  double wxy[2] = {0, 0};
  ShotMetadata meta = readMetadata(raw, readDngWhiteXy(path, wxy) ? wxy : nullptr);
  const auto& dc = d.color;
  float black[3] = {0, 0, 0}, white[3] = {65535.f, 65535.f, 65535.f};
  for (int c = 0; c < 3; ++c) {
    if (dc.dng_levels.dng_whitelevel[c] > 0) white[c] = float(dc.dng_levels.dng_whitelevel[c]);
    black[c] = dc.dng_levels.dng_fblack + dc.dng_levels.dng_fcblack[c];
    if (black[c] == 0.f) black[c] = float(dc.dng_levels.dng_black) + float(dc.dng_levels.dng_cblack[c]);
  }
  // white balance: AsShotNeutral (camera RGB of a white object); else the multipliers LibRaw found
  double neutral[3] = {1, 1, 1};
  const auto& an = dc.dng_levels.asshotneutral;
  if (an[0] > 0 && an[1] > 0 && an[2] > 0) {
    for (int c = 0; c < 3; ++c) neutral[c] = an[c];
  } else if (meta.asShotWb[0] > 0 && meta.asShotWb[2] > 0) {
    for (int c = 0; c < 3; ++c) neutral[c] = 1.0 / meta.asShotWb[c];
  }
  // camera (white balanced) -> XYZ D50
  double wgt = 1.0;  // weight of calibration 0 (interpolated by inverse CCT between the two illuminants)
  const double T0 = illuminantCodeCct(dc.dng_color[0].illuminant), T1 = illuminantCodeCct(dc.dng_color[1].illuminant);
  if (T0 > 0 && T1 > 0 && std::fabs(1.0 / T0 - 1.0 / T1) > 1e-9 && meta.asShotCct > 0)
    wgt = std::clamp((1.0 / meta.asShotCct - 1.0 / T1) / (1.0 / T0 - 1.0 / T1), 0.0, 1.0);
  auto sumAbs = [](const float(*m)[4]) {
    double s = 0;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) s += std::fabs(double(m[r][c]));
    return s;
  };
  double toXyz[9];  // camera (already divided by the neutral) -> XYZ D50
  bool ok = false;
  if (sumAbs(dc.dng_color[0].forwardmatrix) > 1e-6 && sumAbs(dc.dng_color[1].forwardmatrix) > 1e-6) {
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) toXyz[r * 3 + c] = wgt * dc.dng_color[0].forwardmatrix[r][c] + (1 - wgt) * dc.dng_color[1].forwardmatrix[r][c];
    ok = true;
  } else {
    // no ForwardMatrix: XYZ = inverse(ColorMatrix(w)) * camera, then Bradford from the illuminant white to D50
    double cm[9], inv[9];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) cm[r * 3 + c] = wgt * dc.dng_color[0].colormatrix[r][c] + (1 - wgt) * dc.dng_color[1].colormatrix[r][c];
    if (detail::invert3(cm, inv)) {
      const double x = meta.asShotIlluminantXy[0] > 0 ? meta.asShotIlluminantXy[0] : 0.3457, y = meta.asShotIlluminantXy[1] > 0 ? meta.asShotIlluminantXy[1] : 0.3585;
      const double ws[3] = {x / y, 1.0, (1 - x - y) / y}, wd[3] = {0.96422, 1.0, 0.82521};
      const double B[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
      const double Bi[9] = {0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603, 0.0492912, -0.0085287, 0.0400428, 0.9684867};
      double cs[3], cd[3];
      for (int r = 0; r < 3; ++r) {
        cs[r] = B[r * 3] * ws[0] + B[r * 3 + 1] * ws[1] + B[r * 3 + 2] * ws[2];
        cd[r] = B[r * 3] * wd[0] + B[r * 3 + 1] * wd[1] + B[r * 3 + 2] * wd[2];
      }
      double S[9] = {0}, t[9], A[9];
      for (int r = 0; r < 3; ++r) S[r * 3 + r] = cd[r] / cs[r];
      detail::mul3(S, B, t);
      detail::mul3(Bi, t, A);
      detail::mul3(A, inv, toXyz);
      ok = true;
    }
  }
  if (!ok) return fail("JPEG XL DNG without usable colour matrices");
  float m[9];  // camera -> ProPhoto, folded with 1 / neutral
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      double v = 0;
      for (int k = 0; k < 3; ++k) v += double(color::kXyzD50ToProPhoto.m[r][k]) * toXyz[k * 3 + c];
      m[r * 3 + c] = float(v / neutral[c]);
    }
  const int W = jr.width, H = jr.height;
  const int half = options.quality == DecodeQuality::Preview ? 2 : 1;  // the preview decode needs no more than a quarter of the samples
  const int ow = W / half, oh = H / half;
  SourceImage out;
  out.meta = meta;
  out.sceneReferred = true;
  out.linear = ImageF(ow, oh);
  parallelRows(oh, [&](int y) {
    float* dst = out.linear.row(y);
    for (int x = 0; x < ow; ++x) {
      float cam[3] = {0, 0, 0};
      for (int dy = 0; dy < half; ++dy)
        for (int dx = 0; dx < half; ++dx) {
          const std::uint16_t* s = &jr.data[(std::size_t(y * half + dy) * W + (x * half + dx)) * 3];
          for (int c = 0; c < 3; ++c) cam[c] += std::max(0.f, (float(s[c]) - black[c]) / (white[c] - black[c]));
        }
      const float inv = 1.f / float(half * half);
      for (int c = 0; c < 3; ++c) cam[c] *= inv;
      for (int r = 0; r < 3; ++r) dst[3 * x + r] = m[r * 3] * cam[0] + m[r * 3 + 1] * cam[1] + m[r * 3 + 2] * cam[2];
    }
  });
  out.linear = rotateFlip(out.linear, d.sizes.flip);
  out.meta.sensorWidth = (d.sizes.flip & 4) ? H : W;
  out.meta.sensorHeight = (d.sizes.flip & 4) ? W : H;
  suppressShadowChroma(out.linear);
  out.meta.dngCorrections = "JPEG XL (libjxl), levels, forward matrix";
  if (!out.meta.rawFormat.empty()) out.meta.rawFormat += ", ";
  out.meta.rawFormat += "DNG 1.7 JPEG XL";
  const float be = dc.dng_levels.baseline_exposure > -50.f && dc.dng_levels.baseline_exposure < 16.f ? dc.dng_levels.baseline_exposure : std::numeric_limits<float>::quiet_NaN();
  if (anchorEnabled()) attachExposureAnchor(out, path, options, be);
  return out;
}

// DAPPLE_DECODE_TIMING=1: one stderr line per RAW decode with the split of the time (file read + open, decompression, each LibRaw processing stage,
// the copy to float, geometry / anchor). Measurement aid for the demosaic-on-GPU design (docs/engine/pipeline-architecture.md section 31).
struct DecodeTimer {
  using Clock = std::chrono::steady_clock;
  bool on = false;
  Clock::time_point last;
  std::vector<std::pair<std::string, double>> marks;
  std::string stage;  // LibRaw stage in progress
  void start() { last = Clock::now(); }
  void mark(const std::string& name) {
    const auto n = Clock::now();
    marks.emplace_back(name, std::chrono::duration<double, std::milli>(n - last).count());
    last = n;
  }
  static int progress(void* d, enum LibRaw_progress p, int, int) {
    auto* t = static_cast<DecodeTimer*>(d);
    const char* nm = "other";
    switch (p) {
      case LIBRAW_PROGRESS_SCALE_COLORS: nm = "scale_colors (black / white / WB)"; break;
      case LIBRAW_PROGRESS_PRE_INTERPOLATE: nm = "pre_interpolate"; break;
      case LIBRAW_PROGRESS_INTERPOLATE: nm = "demosaic"; break;
      case LIBRAW_PROGRESS_MIX_GREEN: nm = "mix_green"; break;
      case LIBRAW_PROGRESS_MEDIAN_FILTER: nm = "median_filter"; break;
      case LIBRAW_PROGRESS_HIGHLIGHTS: nm = "highlights"; break;
      case LIBRAW_PROGRESS_FUJI_ROTATE: nm = "fuji_rotate"; break;
      case LIBRAW_PROGRESS_FLIP: nm = "flip"; break;
      case LIBRAW_PROGRESS_CONVERT_RGB: nm = "convert_rgb (colour matrix)"; break;
      case LIBRAW_PROGRESS_STRETCH: nm = "stretch"; break;
      case LIBRAW_PROGRESS_RAW2_IMAGE: nm = "raw2image"; break;
      default: break;
    }
    if (!t->stage.empty()) t->mark(t->stage);
    t->stage = nm;
    return 0;
  }
  void print(const char* file) {
    if (!on) return;
    double total = 0;
    for (auto& m : marks) total += m.second;
    std::fprintf(stderr, "decode-timing %s total %.0f ms:", file, total);
    for (auto& m : marks) std::fprintf(stderr, " | %s %.0f", m.first.c_str(), m.second);
    std::fprintf(stderr, "\n");
  }
};

class LibRawDecoder final : public ImageDecoder {
 public:
  const char* name() const override { return "libraw"; }

  bool canDecode(const std::filesystem::path& path) const override {
    static const char* kExts[] = {".arw", ".srf", ".sr2", ".raf", ".dng", ".cr2", ".cr3", ".nef",
                                  ".nrw", ".orf", ".rw2", ".pef", ".srw", ".3fr", ".iiq", ".erf"};
    const std::string ext = lowerExtension(path);
    return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
  }

  Result<ShotMetadata> probe(const std::filesystem::path& path) const override {
    PathDatastream stream(path);
    if (!stream.valid()) return fail("Cannot open " + pathToUtf8(path));
    auto raw = std::make_unique<LibRaw>();
    if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
      return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
    double wxy[2] = {0, 0};
    return readMetadata(*raw, readDngWhiteXy(path, wxy) ? wxy : nullptr);
  }

  Result<SourceImage> decode(const std::filesystem::path& path, const DecodeOptions& options) const override {
    if (options.quality == DecodeQuality::Preview) {  // the same cached mosaic as the full decode and the regions: one unpack, then block reduction
      std::string why;
      if (auto r = decodePreviewFromMosaic(path, options, &why)) return r;
    }
    if (options.quality == DecodeQuality::Full) {  // the Demosaic node: LibRaw only decompresses (core/src/decoders/libraw_mosaic.inc, docs 32); refusals fall through
      std::string why;
      if (auto r = decodeFullFromMosaic(path, options, &why)) return r;
    }
    PathDatastream stream(path);
    if (!stream.valid()) return fail("Cannot open " + pathToUtf8(path));

    auto raw = std::make_unique<LibRaw>();  // large object, keep off the stack
    auto& params = raw->imgdata.params;
    params.output_color = 4;  // ProPhoto RGB
    params.output_bps = 16;
    params.gamm[0] = 1.0;     // linear output
    params.gamm[1] = 1.0;
    params.no_auto_bright = 1;
    params.use_camera_wb = 1;
    params.adjust_maximum_thr = 0.f;  // keep white level = sensor clipping point (no per-frame brightening)
    params.highlight = 0;     // clip; highlight reconstruction is done by us later
    params.half_size = options.quality == DecodeQuality::Preview ? 1 : 0;
    params.user_qual = 3;     // AHD for Bayer; X-Trans uses its own 3-pass method
    if (const char* q = std::getenv("DAPPLE_USER_QUAL")) params.user_qual = std::atoi(q);  // experiments

    if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
      return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
    if (dng::isJxlDng(path)) return decodeJxlDngFile(*raw, path, options);  // DNG 1.7 JPEG XL: LibRaw cannot unpack it
    DecodeTimer timer;
    timer.on = std::getenv("DAPPLE_DECODE_TIMING") != nullptr;
    timer.start();
    timer.mark("open (file read of headers, identify)");
    std::future<Result<Image8>> embeddedPreview;  // for the exposure anchor: the full-size JPEG decode (0.25 - 0.8 s) overlaps the demosaic
    if (anchorEnabled() && options.referenceImage.empty() && !std::getenv("DAPPLE_NO_ASYNC_PREVIEW")) embeddedPreview = std::async(std::launch::async, [&path] { return extractEmbeddedPreviewForAnchor(path); });
#if defined(__APPLE__)
    dapple::sys::appleOmpInit();
#endif
    if (int rc = raw->unpack(); rc != LIBRAW_SUCCESS)
      return fail("LibRaw unpack failed: " + std::string(libraw_strerror(rc)));
    timer.mark("unpack (file read + decompression)");
    double wxy[2] = {0, 0};
    ShotMetadata meta = readMetadata(*raw, readDngWhiteXy(path, wxy) ? wxy : nullptr);  // before processing alters sizes/filters
    if (meta.illuminantSource == "dng-whitexy") {  // the file has no AsShotNeutral: use the multipliers of its white point
      for (int c = 0; c < 3; ++c) raw->imgdata.params.user_mul[c] = meta.asShotWb[c];
      // LibRaw's scale_colors runs its automatic white balance when use_camera_wb is set and cam_mul is empty (as here) and overwrites user_mul:
      // without this line the white point was ignored (Pixel 8 Pro: red / blue 0.92 / 0.95 of the intended multipliers, docs 32.9)
      raw->imgdata.params.use_camera_wb = 0;
    }
    raw->imgdata.params.user_mul[3] = meta.illuminantSource == "dng-whitexy" ? meta.asShotWb[1] : 0.f;
    // DNG opcode lists (phone DNGs): LibRaw does not apply them. OpcodeList2 (GainMap lens shading, FixVignetteRadial) acts on the
    // mosaic here, before LibRaw demosaics; OpcodeList3 (WarpRectilinear, ...) and the default crop act on the demosaiced image below.
    dng::OpcodeList l3;
    dng::ApplyReport corr;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    bool cropIt = false, geom = false;
    const int fileFlip = raw->imgdata.sizes.flip;
    if (raw->imgdata.idata.dng_version != 0 && dngOpcodesEnabled()) {
      try {
        const dng::OpcodeList l1 = dngList(*raw, 0), l2 = dngList(*raw, 1);
        l3 = dngList(*raw, 2);
        for (const dng::OpcodeList* l : {&l1, &l2, (const dng::OpcodeList*)&l3})
          if (!l->error.empty()) corr.skipped.push_back("malformed list (" + l->error + ")");
        if (!l1.ops.empty()) {  // OpcodeList1 acts on the raw values as stored: bad pixel fixes are applied, the rest is reported
          const auto& S1 = raw->imgdata.sizes;
          if (raw->imgdata.rawdata.raw_image && raw->imgdata.idata.filters != 0 && S1.width > 0 && S1.height > 0) {
            dng::MosaicView v;
            v.px = raw->imgdata.rawdata.raw_image;
            v.pitch = std::size_t(S1.raw_pitch) / sizeof(std::uint16_t);
            v.top = S1.top_margin, v.left = S1.left_margin, v.width = S1.width, v.height = S1.height;
            dng::applyMosaic(l1, v, corr);
          } else {
            corr.skipped.push_back("OpcodeList1 (no 16-bit mosaic)");
          }
        }
        const auto& S = raw->imgdata.sizes;
        if (!l2.ops.empty()) {
          if (raw->imgdata.rawdata.raw_image && raw->imgdata.idata.filters != 0 && S.width > 0 && S.height > 0) {
            dng::MosaicView v;
            v.px = raw->imgdata.rawdata.raw_image;
            v.pitch = std::size_t(S.raw_pitch) / sizeof(std::uint16_t);
            v.top = S.top_margin, v.left = S.left_margin, v.width = S.width, v.height = S.height;
            fillBlackPattern(*raw, v);
            dng::applyMosaic(l2, v, corr);
          } else {
            if (!dng::isIdentityGainList(l2)) corr.skipped.push_back("OpcodeList2 (no 16-bit mosaic)");  // Samsung LinearRaw: identity maps only
          }
        }
        cropIt = dngDefaultCrop(*raw, cropX, cropY, cropW, cropH);
        geom = !l3.ops.empty() || cropIt;
        if (geom) raw->imgdata.params.user_flip = 0;  // geometry runs in sensor orientation; rotated afterwards
      } catch (...) {
        l3 = {};
        cropIt = geom = false;
        raw->imgdata.params.user_flip = -1;
        corr.skipped.push_back("exception");
      }
    }
    // Linear DNGs written by Dapple's HDR/panorama merge store value / headroom and record
    // BaselineExposure = log2(headroom); undo it so merged values above 1.0 survive a round trip.
    float dappleGain = 1.f;
    if (std::strcmp(raw->imgdata.color.UniqueCameraModel, "Dapple Linear Merge") == 0 &&
        raw->imgdata.color.dng_levels.baseline_exposure > -50.f && raw->imgdata.color.dng_levels.baseline_exposure < 16.f)
      dappleGain = std::exp2(raw->imgdata.color.dng_levels.baseline_exposure);
    const float dngBaselineEv = raw->imgdata.idata.dng_version != 0 && raw->imgdata.color.dng_levels.baseline_exposure > -50.f &&
                                        raw->imgdata.color.dng_levels.baseline_exposure < 16.f
                                    ? raw->imgdata.color.dng_levels.baseline_exposure + (std::isfinite(dngBaselineOffsetEv(path)) ? dngBaselineOffsetEv(path) : 0.f)
                                    : std::numeric_limits<float>::quiet_NaN();
    if (timer.on) raw->set_progress_handler(&DecodeTimer::progress, &timer);
    timer.mark("opcodes / metadata");
    if (int rc = raw->dcraw_process(); rc != LIBRAW_SUCCESS)
      return fail("LibRaw processing failed: " + std::string(libraw_strerror(rc)));

    if (timer.on && !timer.stage.empty()) timer.mark(timer.stage);
    timer.mark("process end");
    int rc = 0;
    libraw_processed_image_t* mem = raw->dcraw_make_mem_image(&rc);
    if (!mem || rc != LIBRAW_SUCCESS)
      return fail("LibRaw image export failed: " + std::string(libraw_strerror(rc)));
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> guard(
        mem, [](libraw_processed_image_t* p) { LibRaw::dcraw_clear_mem(p); });
    if (mem->type != LIBRAW_IMAGE_BITMAP || (mem->colors != 3 && mem->colors != 1) || mem->bits != 16)  // 1 = monochrome sensor (Leica M Monochrom, Pentax K-3 III Monochrome)
      return fail("Unexpected LibRaw output layout");

    const int rawActiveWidth = int(raw->imgdata.sizes.raw_width);  // unchanged by half_size; before recycle() clears the sizes
    // Free LibRaw's own image buffers before allocating the float copy.
    raw->recycle();

    SourceImage out;
    out.meta = meta;
    out.sceneReferred = true;
    out.linear = ImageF(mem->width, mem->height);
    const auto* src = reinterpret_cast<const std::uint16_t*>(mem->data);
    const int w = mem->width;
    const bool mono = mem->colors == 1;
    const float kScale = dappleGain / 65535.f;
    parallelRows(mem->height, [&](int y) {
      float* d = out.linear.row(y);
      if (mono) {
        const std::uint16_t* s = src + std::size_t(y) * w;
        for (int i = 0; i < w; ++i) d[3 * i] = d[3 * i + 1] = d[3 * i + 2] = s[i] * kScale;
        return;
      }
      const std::uint16_t* s = src + std::size_t(y) * w * 3;
      for (int i = 0; i < w * 3; ++i) d[i] = s[i] * kScale;
    });
    guard.reset();  // release the 16-bit copy
    timer.mark("make_mem_image + copy to float");
    if (geom) {
      try {
        dng::applyImage(l3, out.linear, corr);
        if (cropIt) {
          // the preview decode halves the size of mosaic sensors only: a monochrome / linear file (filters 0) comes out at full size
          const int k = (options.quality == DecodeQuality::Preview && rawActiveWidth >= 2 * out.linear.width - 2) ? 2 : 1;
          out.linear = dng::cropImage(out.linear, cropX / k, cropY / k, cropW / k, cropH / k);
          corr.applied.push_back("DefaultCrop");
        }
      } catch (...) {
        corr.skipped.push_back("exception in geometry");
      }
      out.linear = rotateFlip(out.linear, fileFlip);
    }
    out.meta.dngCorrections = corr.summary();
    if (dappleGain == 1.f) suppressShadowChroma(out.linear);  // not for Dapple merges (their values are not sensor data)
    timer.mark("geometry / shadow floor");
    if (dappleGain == 1.f && anchorEnabled()) attachExposureAnchor(out, path, options, dngBaselineEv, &embeddedPreview);
    timer.mark("exposure anchor (preview decode + compare)");
    timer.print(path.filename().string().c_str());
    return out;
  }
};

Image8 rotate90(const Image8& src, bool clockwise) {
  Image8 dst;
  dst.width = src.height;
  dst.height = src.width;
  dst.data.resize(src.data.size());
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x) {
      const int dx = clockwise ? src.height - 1 - y : y;
      const int dy = clockwise ? x : src.width - 1 - x;
      std::memcpy(&dst.data[(std::size_t(dy) * dst.width + dx) * 3], &src.data[(std::size_t(y) * src.width + x) * 3], 3);
    }
  return dst;
}

}  // namespace

static Result<Image8> extractPreviewImpl(const std::filesystem::path& rawPath, int longEdge, bool forAnchor) {
  PathDatastream stream(rawPath);
  if (!stream.valid()) return fail("Cannot open " + pathToUtf8(rawPath));
  auto raw = std::make_unique<LibRaw>();
  if (int rc = raw->open_datastream(&stream); rc != LIBRAW_SUCCESS)
    return fail("LibRaw open failed: " + std::string(libraw_strerror(rc)));
  if (int rc = raw->unpack_thumb(); rc != LIBRAW_SUCCESS)
    return fail("No embedded preview: " + std::string(libraw_strerror(rc)));
  const auto& th = raw->imgdata.thumbnail;
  if (th.tformat != LIBRAW_THUMBNAIL_JPEG || !th.thumb || th.tlength == 0)
    return fail("Embedded preview is not a JPEG");
  int w = 0, h = 0, n = 0;
  Image8 img;
  bool reduced = false;
  if (forAnchor) {
    const char* fe = std::getenv("DAPPLE_ANCHOR_FAST");
    const bool off = fe && std::strcmp(fe, "off") == 0;
    int jw = 0, jh = 0;
    JpegDcImage dc;
    if (!off && jpegSize(reinterpret_cast<const std::uint8_t*>(th.thumb), th.tlength, jw, jh) && std::max(jw, jh) >= 2048 &&
        decodeJpegDcScale(reinterpret_cast<const std::uint8_t*>(th.thumb), th.tlength, dc)) {
      w = dc.width, h = dc.height;
      img.data = std::move(dc.rgb);
      reduced = true;
    }
  }
  if (!reduced) {
    std::unique_ptr<stbi_uc, void (*)(void*)> px(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(th.thumb), int(th.tlength), &w, &h, &n, 3),
        stbi_image_free);
    if (!px) return fail("Preview decode failed: " + std::string(stbi_failure_reason()));
    img.data.assign(px.get(), px.get() + std::size_t(w) * h * 3);
  }
  img.width = w;
  img.height = h;

  // Embedded previews usually keep sensor orientation; rotate when the file
  // says portrait but the preview is landscape.
  const int flip = raw->imgdata.sizes.flip;
  if ((flip & 4) != 0 && w > h) img = rotate90(img, flip == 6);
  else if (flip == 3) {  // 180 degrees (DNG Orientation 3, e.g. Pixel 7 Pro): the preview keeps sensor orientation like the others
    std::reverse(reinterpret_cast<std::array<std::uint8_t, 3>*>(img.data.data()), reinterpret_cast<std::array<std::uint8_t, 3>*>(img.data.data()) + std::size_t(w) * h);
  }

  if (longEdge > 0 && std::max(img.width, img.height) > longEdge) {
    ImageF f(img.width, img.height);
    for (std::size_t i = 0; i < f.data.size(); ++i) f.data[i] = img.data[i] / 255.f;
    f = resizeToLongEdge(f, longEdge);
    img.width = f.width;
    img.height = f.height;
    img.data.resize(f.data.size());
    for (std::size_t i = 0; i < f.data.size(); ++i) img.data[i] = std::uint8_t(std::clamp(f.data[i], 0.f, 1.f) * 255.f + 0.5f);
  }
  return img;
}

Result<Image8> extractEmbeddedPreview(const std::filesystem::path& rawPath, int longEdge) { return extractPreviewImpl(rawPath, longEdge, false); }

Result<Image8> extractEmbeddedPreviewForAnchor(const std::filesystem::path& rawPath) { return extractPreviewImpl(rawPath, 0, true); }

#include "decoders/libraw_mosaic.inc"  // decodeRawMosaic (Demosaic node input), docs/engine/pipeline-architecture.md section 32

std::unique_ptr<ImageDecoder> makeLibRawDecoder() { return std::make_unique<LibRawDecoder>(); }

}  // namespace dapple
