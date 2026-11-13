#include "colour_space.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dapple::color {

namespace {

struct D3 {
  double m[3][3];
};

D3 mulD(const D3& a, const D3& b) {
  D3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
      r.m[i][j] = s;
    }
  return r;
}

D3 invD(const D3& a) {
  const auto& m = a.m;
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
               c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  const double id = 1.0 / det;
  D3 r{};
  r.m[0][0] = c00 * id;
  r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
  r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
  r.m[1][0] = c01 * id;
  r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
  r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
  r.m[2][0] = c02 * id;
  r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
  r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
  return r;
}

void xyFromWhite(double x, double y, double out[3]) {
  out[0] = x / y;
  out[1] = 1.0;
  out[2] = (1.0 - x - y) / y;
}

// Bradford adaptation of XYZ from a white (x, y) to D50.
D3 bradfordToD50(double wx, double wy) {
  static const D3 B{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
  double ws[3], wd[3];
  xyFromWhite(wx, wy, ws);
  xyFromWhite(0.3457, 0.3585, wd);  // D50
  double cs[3] = {}, cd[3] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) cs[i] += B.m[i][j] * ws[j], cd[i] += B.m[i][j] * wd[j];
  D3 S{};
  for (int i = 0; i < 3; ++i) S.m[i][i] = cd[i] / cs[i];
  return mulD(invD(B), mulD(S, B));
}

struct Primaries {
  double r[2], g[2], b[2], w[2];
};

// Linear RGB -> XYZ D50 of a space given by its chromaticities.
D3 toXyzD50(const Primaries& p) {
  D3 P{};
  const double* c[3] = {p.r, p.g, p.b};
  for (int j = 0; j < 3; ++j) {
    P.m[0][j] = c[j][0] / c[j][1];
    P.m[1][j] = 1.0;
    P.m[2][j] = (1.0 - c[j][0] - c[j][1]) / c[j][1];
  }
  double W[3];
  xyFromWhite(p.w[0], p.w[1], W);
  const D3 Pi = invD(P);
  double s[3] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) s[i] += Pi.m[i][j] * W[j];
  D3 M = P;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) M.m[i][j] *= s[j];
  return mulD(bradfordToD50(p.w[0], p.w[1]), M);
}

Mat3 toMat3(const D3& d) {
  Mat3 m{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.m[i][j] = float(d.m[i][j]);
  return m;
}

struct Ref {
  const char* name;
  Primaries p;
};
const Ref kRefs[] = {
    {"sRGB", {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, {0.3127, 0.3290}}},
    {"Display P3", {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}}},
    {"DCI-P3", {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351}}},
    {"Rec.2020", {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}}},
    {"Adobe RGB (1998)", {{0.640, 0.330}, {0.210, 0.710}, {0.150, 0.060}, {0.3127, 0.3290}}},
};

// Name of the reference space whose D50 colorant matrix is within tolerance, or "".
const char* nameOfMatrix(const D3& m) {
  for (const auto& r : kRefs) {
    const D3 ref = toXyzD50(r.p);
    double worst = 0;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) worst = std::max(worst, std::fabs(ref.m[i][j] - m.m[i][j]));
    if (worst < 0.004) return r.name;
  }
  return "";
}

const char* nameOfChrm(const double c[8]) {
  for (const auto& r : kRefs) {
    const double v[8] = {r.p.w[0], r.p.w[1], r.p.r[0], r.p.r[1], r.p.g[0], r.p.g[1], r.p.b[0], r.p.b[1]};
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok = ok && std::fabs(v[i] - c[i]) < 0.01;
    if (ok) return r.name;
  }
  return "";
}

const char* trcName(const Trc& t) {
  switch (t.kind) {
    case Trc::Srgb: return "sRGB curve";
    case Trc::Bt709: return "BT.709 curve";
    case Trc::Linear: return "linear";
    case Trc::Gamma: return "gamma";
    default: return "curve";
  }
}

std::string fmt(const char* f, double a = 0) {
  char b[64];
  std::snprintf(b, sizeof b, f, a);
  return b;
}

std::string describe(const std::string& name, const Trc& t, const char* via) {
  std::string s = "dapple: source colour space " + name + " (" + via + ", ";
  s += trcName(t);
  if (t.kind == Trc::Gamma) s += " " + fmt("%.2f", t.gamma);
  return s + ")";
}

bool trcLooksSrgb(const Trc& t) {
  if (t.kind == Trc::Srgb) return true;
  double worst = 0;
  for (int i = 0; i <= 255; ++i) {
    const float x = i / 255.f;
    worst = std::max(worst, double(std::fabs(t.eval(x) - srgbDecode(x))));
  }
  return worst < 0.0015;
}

SourceSpace makeSpace(const std::string& name, const D3& toD50, const Trc t[3], const char* via, bool allowSrgbExact, bool matrixIsSrgb) {
  SourceSpace s;
  s.name = name;
  for (int i = 0; i < 3; ++i) s.trc[i] = t[i];
  const bool srgbCurve = trcLooksSrgb(t[0]) && trcLooksSrgb(t[1]) && trcLooksSrgb(t[2]);
  if (allowSrgbExact && matrixIsSrgb && srgbCurve) {
    s = srgbSpace();  // same pixels as the untagged path, bit for bit
    return s;
  }
  s.isSrgb = false;
  if (srgbCurve)
    for (int i = 0; i < 3; ++i) s.trc[i] = Trc{};
  s.toProPhoto = mul(kXyzD50ToProPhoto, toMat3(toD50));
  s.note = describe(name, s.trc[0], via);
  return s;
}

bool trcFromCicp(int tc, Trc& t) {
  t = Trc{};
  switch (tc) {
    case 13: t.kind = Trc::Srgb; return true;
    case 1: case 6: case 14: case 15: t.kind = Trc::Bt709; return true;
    case 8: t.kind = Trc::Linear; return true;
    case 4: t.kind = Trc::Gamma; t.gamma = 2.2f; return true;
    case 5: t.kind = Trc::Gamma; t.gamma = 2.8f; return true;
    default: return false;
  }
}

bool primariesFromCicp(int cp, Primaries& p, std::string& name) {
  switch (cp) {
    case 1: p = kRefs[0].p; name = kRefs[0].name; return true;
    case 12: p = kRefs[1].p; name = kRefs[1].name; return true;
    case 11: p = kRefs[2].p; name = kRefs[2].name; return true;
    case 9: p = kRefs[3].p; name = kRefs[3].name; return true;
    default: return false;
  }
}

// ---- ICC ------------------------------------------------------------------------------------------------------------------------
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
double s15(const std::uint8_t* p) { return double(std::int32_t(be32(p))) / 65536.0; }

bool readXyz(const std::uint8_t* d, std::size_t n, double out[3]) {
  if (n < 20 || std::memcmp(d, "XYZ ", 4) != 0) return false;
  for (int i = 0; i < 3; ++i) out[i] = s15(d + 8 + 4 * i);
  return true;
}

bool readTrc(const std::uint8_t* d, std::size_t n, Trc& t) {
  t = Trc{};
  if (n >= 12 && std::memcmp(d, "curv", 4) == 0) {
    const std::uint32_t c = be32(d + 8);
    if (12 + 2ull * c > n) return false;
    if (c == 0) {
      t.kind = Trc::Linear;
    } else if (c == 1) {
      t.kind = Trc::Gamma;
      t.gamma = float(((d[12] << 8) | d[13]) / 256.0);
    } else {
      t.kind = Trc::Table;
      t.table.resize(c);
      for (std::uint32_t i = 0; i < c; ++i) t.table[i] = ((d[12 + 2 * i] << 8) | d[13 + 2 * i]) / 65535.f;
    }
    return true;
  }
  if (n >= 12 && std::memcmp(d, "para", 4) == 0) {
    static const int cnt[5] = {1, 3, 4, 5, 7};
    const int ft = (d[8] << 8) | d[9];
    if (ft < 0 || ft > 4 || 12 + 4 * std::size_t(cnt[ft]) > n) return false;
    t.kind = Trc::Param;
    t.paramType = ft;
    for (int i = 0; i < cnt[ft]; ++i) t.p[i] = float(s15(d + 12 + 4 * i));
    return true;
  }
  return false;
}

}  // namespace

float Trc::eval(float x) const {
  switch (kind) {
    case Srgb: return srgbDecode(x);
    case Linear: return x;
    case Gamma: return std::pow(std::max(x, 0.f), gamma);
    case Bt709: return x < 0.081f ? x / 4.5f : std::pow((x + 0.099f) / 1.099f, 1.f / 0.45f);
    case Table: {
      const float f = std::clamp(x, 0.f, 1.f) * float(table.size() - 1);
      const std::size_t i = std::min<std::size_t>(std::size_t(f), table.size() - 2);
      return table[i] + (table[i + 1] - table[i]) * (f - float(i));
    }
    case Param: {
      const float g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
      auto pw = [&](float v) { return std::pow(std::max(v, 0.f), g); };
      switch (paramType) {
        case 0: return pw(x);
        case 1: return a != 0 && x >= -b / a ? pw(a * x + b) : 0.f;
        case 2: return a != 0 && x >= -b / a ? pw(a * x + b) + c : c;
        case 3: return x >= d ? pw(a * x + b) : c * x;
        default: return x >= d ? pw(a * x + b) + e : c * x + f;
      }
    }
  }
  return x;
}

std::vector<float> SourceSpace::lut(int channel, int n) const {
  std::vector<float> v(static_cast<std::size_t>(n), 0.f);
  const float den = float(n - 1);
  for (int i = 0; i < n; ++i) v[std::size_t(i)] = trc[channel].eval(float(i) / den);
  return v;
}

SourceSpace srgbSpace() { return SourceSpace{}; }

bool spaceFromIcc(const std::uint8_t* d, std::size_t size, SourceSpace& out, std::string& why) {
  if (size < 132 || std::memcmp(d + 36, "acsp", 4) != 0) return why = "not an ICC profile", false;
  if (std::memcmp(d + 16, "RGB ", 4) != 0) return why = "not an RGB profile", false;
  const std::uint32_t count = be32(d + 128);
  if (count > 200 || 132ull + 12ull * count > size) return why = "bad tag table", false;
  auto find = [&](const char* sig, const std::uint8_t*& p, std::size_t& n) {
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint8_t* e = d + 132 + 12 * i;
      if (std::memcmp(e, sig, 4) != 0) continue;
      const std::uint32_t off = be32(e + 4), len = be32(e + 8);
      if (std::uint64_t(off) + len > size) return false;
      p = d + off;
      n = len;
      return true;
    }
    return false;
  };
  const std::uint8_t* p;
  std::size_t n;
  D3 M{};
  const char* cols[3] = {"rXYZ", "gXYZ", "bXYZ"};
  const char* trcs[3] = {"rTRC", "gTRC", "bTRC"};
  Trc t[3];
  for (int c = 0; c < 3; ++c) {
    double xyz[3];
    if (!find(cols[c], p, n) || !readXyz(p, n, xyz)) return why = "no matrix colorants (LUT profile)", false;
    for (int r = 0; r < 3; ++r) M.m[r][c] = xyz[r];
    if (!find(trcs[c], p, n) || !readTrc(p, n, t[c])) return why = "no usable tone curve", false;
  }
  const char* nm = nameOfMatrix(M);
  const std::string name = *nm ? nm : "custom RGB profile";
  out = makeSpace(name, M, t, "ICC", true, std::strcmp(nm, "sRGB") == 0);
  return true;
}

SourceSpace resolveSourceSpace(const ColourTags& tg) {
  auto fallback = [](const std::string& why) {
    SourceSpace s = srgbSpace();
    s.fallback = true;
    s.note = "dapple: unknown colour space (" + why + "), read as sRGB";
    return s;
  };
  if (tg.hasCicp) {
    Primaries p;
    std::string name;
    Trc t;
    if (tg.cicpMatrix != 0) return fallback("cICP matrix " + std::to_string(tg.cicpMatrix) + " is not RGB");
    if (!primariesFromCicp(tg.cicpPrimaries, p, name)) return fallback("cICP primaries " + std::to_string(tg.cicpPrimaries));
    if (!trcFromCicp(tg.cicpTransfer, t)) return fallback("cICP transfer " + std::to_string(tg.cicpTransfer) + " (HDR transfer not supported)");
    const Trc tt[3] = {t, t, t};
    return makeSpace(name, toXyzD50(p), tt, "cICP", true, tg.cicpPrimaries == 1);
  }
  if (!tg.icc.empty()) {
    SourceSpace s;
    std::string why;
    if (spaceFromIcc(tg.icc.data(), tg.icc.size(), s, why)) return s;
    return fallback("ICC profile: " + why);
  }
  if (tg.hasSrgbChunk) return srgbSpace();
  if (tg.hasChrm) {
    const double* c = tg.chrm;
    const char* nm = nameOfChrm(c);
    Trc t;
    if (tg.gamma > 0.05 && tg.gamma < 1.0) {
      // 0.45455 beside sRGB primaries is the usual sRGB fallback pair; with other primaries it means a pure 2.2 gamma
      if (std::strcmp(nm, "sRGB") == 0 && std::fabs(tg.gamma - 0.45455) < 0.015) t.kind = Trc::Srgb;
      else t.kind = Trc::Gamma, t.gamma = float(1.0 / tg.gamma);
    }
    if (!*nm) {
      for (int i = 0; i < 8; ++i)
        if (!(c[i] > 0 && c[i] < 1)) return fallback("bad cHRM");
    }
    Primaries p{{c[2], c[3]}, {c[4], c[5]}, {c[6], c[7]}, {c[0], c[1]}};
    const Trc tt[3] = {t, t, t};
    return makeSpace(*nm ? nm : "custom primaries", toXyzD50(p), tt, "cHRM", true, std::strcmp(nm, "sRGB") == 0);
  }
  return srgbSpace();  // no colour information (or only gAMA): sRGB, as before
}

}  // namespace dapple::color
