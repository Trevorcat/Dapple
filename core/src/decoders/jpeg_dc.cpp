#include "jpeg_dc.h"

#include <algorithm>
#include <cstring>

namespace dapple {
namespace {

struct Huff {
  bool present = false;
  std::uint16_t fast[512] = {};  // (length << 8) | symbol for codes of up to 9 bits, 0 when longer
  int maxcode[18] = {};
  int valptr[17] = {};
  int mincode[17] = {};
  std::uint8_t vals[256] = {};
};

struct Comp {
  int id = 0, h = 1, v = 1, tq = 0, td = 0, ta = 0;
  int pw = 0, ph = 0;  // plane size in blocks
  std::vector<std::int16_t> cf;  // per block the coefficients (0,0), (1,0), (0,1), (1,1) as quantised: horizontal, vertical frequency
};

struct Bits {
  const std::uint8_t* p;
  const std::uint8_t* end;
  std::uint64_t buf = 0;
  int n = 0;
  bool marker = false;
  void fill() {
    while (n <= 56) {
      std::uint8_t b = 0;
      if (!marker && p < end) {
        if (*p == 0xFF) {
          if (p + 1 >= end) {
            marker = true;
          } else if (p[1] == 0) {
            b = 0xFF;
            p += 2;
          } else {
            marker = true;
          }
        } else {
          b = *p++;
        }
      }
      buf |= std::uint64_t(b) << (56 - n);
      n += 8;
    }
  }
  int peek(int k) {
    if (n < k) fill();
    return int(buf >> (64 - k));
  }
  void drop(int k) {
    buf <<= k;
    n -= k;
  }
  int get(int k) {
    const int v = peek(k);
    drop(k);
    return v;
  }
  void reset() {
    buf = 0;
    n = 0;
    marker = false;
  }
};

inline int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

int decodeSym(Bits& b, const Huff& h) {
  const int look = b.peek(9);
  if (const int f = h.fast[look]) {
    b.drop(f >> 8);
    return f & 255;
  }
  const int code = b.peek(16);
  for (int len = 10; len <= 16; ++len) {
    const int c = code >> (16 - len);
    if (h.maxcode[len] >= 0 && c <= h.maxcode[len] && c >= h.mincode[len]) {
      b.drop(len);
      return h.vals[h.valptr[len] + c - h.mincode[len]];
    }
  }
  return -1;
}

std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }

}  // namespace

bool jpegSize(const std::uint8_t* d, std::size_t sz, int& w, int& h) {
  if (sz < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  std::size_t i = 2;
  while (i + 4 <= sz) {
    if (d[i] != 0xFF) return false;
    const int m = d[i + 1];
    if (m == 0xFF) {
      ++i;
      continue;
    }
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      i += 2;
      continue;
    }
    const std::size_t len = be16(d + i + 2);
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (i + 9 > sz) return false;
      h = be16(d + i + 5);
      w = be16(d + i + 7);
      return w > 0 && h > 0;
    }
    i += 2 + len;
  }
  return false;
}

bool decodeJpegDcScale(const std::uint8_t* d, std::size_t sz, JpegDcImage& out) {
  if (sz < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  std::uint16_t q0[4][5] = {};
  bool haveQ[4] = {false, false, false, false};
  Huff dcT[4], acT[4];
  std::vector<Comp> comps;
  int W = 0, H = 0, restart = 0, adobeTransform = -1;
  std::size_t i = 2;
  bool sof = false;
  while (i + 4 <= sz) {
    if (d[i] != 0xFF) return false;
    const int m = d[i + 1];
    if (m == 0xFF) {
      ++i;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    if (m == 0xD9) return false;
    const std::size_t len = be16(d + i + 2);
    if (len < 2 || i + 2 + len > sz) return false;
    const std::uint8_t* s = d + i + 4;
    const std::uint8_t* e = d + i + 2 + len;
    if (m == 0xDB) {
      while (s < e) {
        const int pq = s[0] >> 4, t = s[0] & 15;
        ++s;
        if (t > 3 || s + (pq ? 128 : 64) > e) return false;
        for (int k = 0; k < 5; ++k) q0[t][k] = pq ? be16(s + 2 * k) : s[k];  // zigzag order: 0 = DC, 1 = (1,0), 2 = (0,1), 4 = (1,1)
        haveQ[t] = true;
        s += pq ? 128 : 64;
      }
    } else if (m == 0xC4) {
      while (s + 17 <= e) {
        const int tc = s[0] >> 4, th = s[0] & 15;
        if (th > 3 || tc > 1) return false;
        Huff& hf = tc ? acT[th] : dcT[th];
        int cnt[17] = {};
        int total = 0;
        for (int k = 1; k <= 16; ++k) total += cnt[k] = s[k];
        s += 17;
        if (total > 256 || s + total > e) return false;
        std::memcpy(hf.vals, s, std::size_t(total));
        s += total;
        std::memset(hf.fast, 0, sizeof hf.fast);
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
          hf.valptr[l] = k;
          hf.mincode[l] = code;
          for (int c = 0; c < cnt[l]; ++c, ++code, ++k) {
            if (l <= 9) {
              const int base = code << (9 - l);
              for (int f = 0; f < (1 << (9 - l)); ++f) hf.fast[base + f] = std::uint16_t((l << 8) | hf.vals[k]);
            }
          }
          hf.maxcode[l] = cnt[l] ? code - 1 : -1;
          code <<= 1;
        }
        hf.present = true;
      }
    } else if (m == 0xDD) {
      if (len < 4) return false;
      restart = be16(d + i + 4);
    } else if (m == 0xEE) {
      if (len >= 14 && std::memcmp(s, "Adobe", 5) == 0) adobeTransform = s[11];
    } else if (m == 0xC0 || m == 0xC1) {
      if (s[0] != 8 || sof) return false;
      H = be16(s + 1);
      W = be16(s + 3);
      const int nc = s[5];
      if ((nc != 1 && nc != 3) || W <= 0 || H <= 0 || s + 6 + 3 * nc > e) return false;
      comps.resize(std::size_t(nc));
      for (int c = 0; c < nc; ++c) {
        Comp& cc = comps[std::size_t(c)];
        cc.id = s[6 + 3 * c];
        cc.h = s[7 + 3 * c] >> 4;
        cc.v = s[7 + 3 * c] & 15;
        cc.tq = s[8 + 3 * c];
        if (cc.h < 1 || cc.h > 4 || cc.v < 1 || cc.v > 4 || cc.tq > 3) return false;
      }
      sof = true;
    } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      return false;  // progressive, lossless, arithmetic
    } else if (m == 0xDA) {
      if (!sof) return false;
      const int ns = s[0];
      if (ns != int(comps.size()) || s + 1 + 2 * ns + 3 > e) return false;  // one interleaved scan only
      for (int c = 0; c < ns; ++c) {
        int idx = -1;
        for (std::size_t k = 0; k < comps.size(); ++k)
          if (comps[k].id == s[1 + 2 * c]) idx = int(k);
        if (idx < 0) return false;
        comps[std::size_t(idx)].td = s[2 + 2 * c] >> 4;
        comps[std::size_t(idx)].ta = s[2 + 2 * c] & 15;
        if (comps[std::size_t(idx)].td > 3 || comps[std::size_t(idx)].ta > 3) return false;
      }
      const std::uint8_t* ss = s + 1 + 2 * ns;
      if (ss[0] != 0 || ss[1] != 63 || ss[2] != 0) return false;
      i += 2 + len;
      break;
    }
    i += 2 + len;
  }
  if (!sof || i >= sz) return false;
  int hmax = 1, vmax = 1;
  for (const auto& c : comps) hmax = std::max(hmax, c.h), vmax = std::max(vmax, c.v);
  const int mcux = (W + 8 * hmax - 1) / (8 * hmax), mcuy = (H + 8 * vmax - 1) / (8 * vmax);
  for (auto& c : comps) {
    if (!haveQ[c.tq] || !dcT[c.td].present || !acT[c.ta].present) return false;
    c.pw = mcux * c.h;
    c.ph = mcuy * c.v;
    c.cf.assign(std::size_t(c.pw) * std::size_t(c.ph) * 4, 0);
  }
  Bits b{d + i, d + sz};
  std::vector<int> pred(comps.size(), 0);
  int untilRestart = restart;
  for (int my = 0; my < mcuy; ++my) {
    for (int mx = 0; mx < mcux; ++mx) {
      if (restart && untilRestart == 0) {
        b.reset();
        const std::uint8_t* p = b.p;
        while (p + 1 < b.end && !(p[0] == 0xFF && p[1] >= 0xD0 && p[1] <= 0xD7)) ++p;
        if (p + 1 >= b.end) return false;
        b.p = p + 2;
        std::fill(pred.begin(), pred.end(), 0);
        untilRestart = restart;
      }
      for (std::size_t ci = 0; ci < comps.size(); ++ci) {
        Comp& c = comps[ci];
        const Huff& dt = dcT[c.td];
        const Huff& at = acT[c.ta];
        for (int by = 0; by < c.v; ++by) {
          for (int bx = 0; bx < c.h; ++bx) {
            const int t = decodeSym(b, dt);
            if (t < 0 || t > 15) return false;
            const int diff = t ? extend(b.get(t), t) : 0;
            pred[ci] += diff;
            std::int16_t* cb = &c.cf[(std::size_t(my * c.v + by) * std::size_t(c.pw) + std::size_t(mx * c.h + bx)) * 4];
            cb[0] = std::int16_t(std::clamp(pred[ci], -32768, 32767));
            for (int k = 1; k < 64;) {
              const int rs = decodeSym(b, at);
              if (rs < 0) return false;
              const int r = rs >> 4, s2 = rs & 15;
              if (s2 == 0) {
                if (r == 15) {
                  k += 16;
                  continue;
                }
                break;
              }
              k += r;
              if (k >= 64) return false;
              if (k == 1 || k == 2 || k == 4) cb[k == 4 ? 3 : k] = std::int16_t(extend(b.get(s2), s2));
              else b.drop(s2);
              ++k;
            }
          }
        }
      }
      if (restart) --untilRestart;
    }
  }
  out.fullWidth = W;
  out.fullHeight = H;
  out.width = (W + 3) / 4;
  out.height = (H + 3) / 4;
  out.rgb.assign(std::size_t(out.width) * std::size_t(out.height) * 3, 0);
  // 8x8 DCT -> 2x2 pixels (each the mean of a 4x4 sub-block): mean over x in 4i..4i+3 of cos((2x+1) u pi / 16), u = 0, 1
  const float mu[2][2] = {{1.f, 1.f}, {0.64072959f, -0.64072959f}};  // [u][sub-block]
  const bool ycc = comps.size() == 3 && adobeTransform != 0;
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      float v[3] = {128.f, 128.f, 128.f};
      for (std::size_t ci = 0; ci < comps.size(); ++ci) {
        const Comp& c = comps[ci];
        const int px = std::min(x * c.h / hmax, 2 * c.pw - 1), py = std::min(y * c.v / vmax, 2 * c.ph - 1);
        const std::int16_t* cb = &c.cf[(std::size_t(py >> 1) * std::size_t(c.pw) + std::size_t(px >> 1)) * 4];
        const int i = px & 1, j = py & 1;
        const std::uint16_t* q = q0[c.tq];
        // f = 1/4 sum C(u)C(v) F(u,v) m_u(i) m_v(j), C(0) = 1/sqrt 2, F = quantised * q
        const float dcv = float(cb[0]) * float(q[0]) * 0.125f;                                        // C(0)C(0)/4 = 1/8
        const float hf = float(cb[1]) * float(q[1]) * 0.17677670f * mu[1][i];                        // C(1)C(0)/4 = 1/(4 sqrt 2)
        const float vf = float(cb[2]) * float(q[2]) * 0.17677670f * mu[1][j];
        const float df = float(cb[3]) * float(q[4]) * 0.25f * mu[1][i] * mu[1][j];
        v[ci] = dcv + hf + vf + df + 128.f;
      }
      float r, g, bl;
      if (comps.size() == 1) {
        r = g = bl = v[0];
      } else if (ycc) {
        r = v[0] + 1.402f * (v[2] - 128.f);
        g = v[0] - 0.344136f * (v[1] - 128.f) - 0.714136f * (v[2] - 128.f);
        bl = v[0] + 1.772f * (v[1] - 128.f);
      } else {
        r = v[0], g = v[1], bl = v[2];
      }
      std::uint8_t* o = &out.rgb[(std::size_t(y) * std::size_t(out.width) + std::size_t(x)) * 3];
      o[0] = std::uint8_t(std::clamp(r + 0.5f, 0.f, 255.f));
      o[1] = std::uint8_t(std::clamp(g + 0.5f, 0.f, 255.f));
      o[2] = std::uint8_t(std::clamp(bl + 0.5f, 0.f, 255.f));
    }
  }
  return true;
}

}  // namespace dapple
