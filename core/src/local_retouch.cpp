// Portrait retouch as local operations (docs/engine/pipeline-architecture.md section 43): bind-time heal-spot detection and source search, and
// the CPU kernels of the retouch part of LocalDetail. GPU twins: core/engine/shaders/local_heal.comp, local_rt_down.comp, local_retouch.comp
// (keep the per-pixel arithmetic identical). Classical image processing only: no model, no generative fill.
//
// Clean-room notes: frequency separation with an edge-aware base is the textbook decomposition (guided filter: He, Sun, Tang 2010/2013);
// blob detection by differences of Gaussians across scales (Lindeberg 1998; Lowe 2004 for the DoG approximation; SIFT's patent has
// expired and only the published idea is used); the heal = source patch + a membrane fitted to the boundary mismatch (the "healing brush"
// idea, Georgiev 2004 / US 6,587,592, expired) with the membrane approximated by Shepard's inverse-distance interpolation of ring samples
// (Shepard 1968). PatchMatch (Barnes et al. 2009, patents held by Adobe) was considered and rejected: the source is chosen by a small
// directional search over 16 candidate offsets instead.
#include "local_retouch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "parallel.h"

namespace dapple::local {

namespace {

constexpr float kLumaPP[3] = {0.2880402f, 0.7118741f, 0.0000857f};   // ProPhoto Y (scene-linear source)
constexpr float kLumaS[3] = {0.2126729f, 0.7151522f, 0.0721750f};    // display luma (spatial.cpp, common.glsl kLuma)

inline float sstep(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

std::uint64_t mix64(std::uint64_t h, std::uint64_t x) {
  h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0x100000001B3ull;
  return h ^ (h >> 29);
}
std::uint64_t mixf(std::uint64_t h, float v) {
  if (v == 0.f) v = 0.f;
  std::uint32_t b;
  std::memcpy(&b, &v, 4);
  return mix64(h, b);
}

// unit directions of the 16 ring samples (k * 22.5 degrees), literal so that CPU and GPU use the same numbers (local_heal.comp)
constexpr float kRing[16][2] = {{1.f, 0.f},          {0.9238795f, 0.3826834f},  {0.7071068f, 0.7071068f},  {0.3826834f, 0.9238795f},
                                {0.f, 1.f},          {-0.3826834f, 0.9238795f}, {-0.7071068f, 0.7071068f}, {-0.9238795f, 0.3826834f},
                                {-1.f, 0.f},         {-0.9238795f, -0.3826834f}, {-0.7071068f, -0.7071068f}, {-0.3826834f, -0.9238795f},
                                {0.f, -1.f},         {0.3826834f, -0.9238795f}, {0.7071068f, -0.7071068f},  {0.9238795f, -0.3826834f}};

// ---- separable gaussian (detection only, CPU) ---------------------------------------------------------------------------------------------
void gauss(const std::vector<float>& in, std::vector<float>& out, int w, int h, float sigma) {
  const int r = std::max(1, int(std::ceil(3.f * sigma)));
  std::vector<float> k(std::size_t(2 * r + 1));
  double s = 0;
  for (int i = -r; i <= r; ++i) s += (k[std::size_t(i + r)] = float(std::exp(-0.5 * double(i * i) / (double(sigma) * sigma))));
  for (float& v : k) v = float(v / s);
  std::vector<float> t(in.size());
  parallelFor(h, [&](int y) {
    const float* row = in.data() + std::size_t(y) * w;
    float* o = t.data() + std::size_t(y) * w;
    for (int x = 0; x < w; ++x) {
      float acc = 0;
      for (int i = -r; i <= r; ++i) acc += k[std::size_t(i + r)] * row[std::clamp(x + i, 0, w - 1)];
      o[x] = acc;
    }
  });
  out.resize(in.size());
  parallelFor(h, [&](int y) {
    float* o = out.data() + std::size_t(y) * w;
    for (int x = 0; x < w; ++x) {
      float acc = 0;
      for (int i = -r; i <= r; ++i) acc += k[std::size_t(i + r)] * t[std::size_t(std::clamp(y + i, 0, h - 1)) * w + x];
      o[x] = acc;
    }
  });
}

// mask of adjustment `adj` of the frame at photo point (u, v) for the linear pixel `px` (pre-gain: range components see the photo without the
// global white balance / exposure at bind time)
float maskAt(const Frame& f, int adj, float u, float v, const float px[3]) {
  if (adj < 0) return 1.f;
  return maskOf(f, adj, u, v, px, px) / std::max(f.adj[std::size_t(adj)].weight, 1e-6f);
}

int zeroBased(int v) { return v < 0 ? 0 : v; }

}  // namespace

// ---- detection ---------------------------------------------------------------------------------------------------------------------------
std::vector<Blob> detectBlobs(const float* rgb, const float* mask, int w, int h, float iodPx, float sens, bool searchSources) {
  std::vector<Blob> out;
  if (w < 8 || h < 8 || !(iodPx > 4.f)) return out;
  const std::size_t N = std::size_t(w) * h;
  std::vector<float> L(N), R(N), M(N, 1.f);
  for (std::size_t i = 0; i < N; ++i) {
    const float* p = rgb + 3 * i;
    const float Y = std::max(1e-5f, p[0] * kLumaPP[0] + p[1] * kLumaPP[1] + p[2] * kLumaPP[2]);
    L[i] = std::log2(Y);
    R[i] = 0.5f * std::log2((std::max(p[0], 0.f) + 1e-4f) / (std::max(p[1], 0.f) + 1e-4f));
    if (mask) M[i] = mask[i];
  }
  // three blemish scales (a blemish is 1.5 .. 8 % of the inter-ocular distance across); DoG with a factor 2 between the two gaussians
  const float scales[3] = {std::max(0.7f, 0.016f * iodPx), std::max(0.9f, 0.026f * iodPx), std::max(1.1f, 0.042f * iodPx)};
  const float rho = std::max(3.f, 0.35f * iodPx);  // neighbourhood of the band statistics
  const float z0 = 5.6f - 0.04f * std::clamp(sens, 0.f, 100.f);
  const float smin = 0.045f;  // log2 units: a blemish at least ~3 % darker (or redder) than its surroundings
  std::vector<float> zmax(N, 0.f), smax(N, 0.f), tex;
  std::vector<int> jmax(N, 0);
  std::vector<float> a, b, ra, rb, e2, gm, gme;
  // statistics only over well-inside skin (a soft mask edge next to an eye or a nostril would dominate the band energy), the band clipped
  std::vector<float> Ms(N);
  for (std::size_t i = 0; i < N; ++i) Ms[i] = sstep(0.5f, 0.9f, M[i]);
  gauss(Ms, gm, w, h, rho);
  for (int j = 0; j < 3; ++j) {
    gauss(L, a, w, h, scales[j]);
    gauss(L, b, w, h, 2.f * scales[j]);
    gauss(R, ra, w, h, scales[j]);
    gauss(R, rb, w, h, 2.f * scales[j]);
    std::vector<float> s(N);
    e2.assign(N, 0.f);
    for (std::size_t i = 0; i < N; ++i) {
      const float dl = a[i] - b[i], dr = ra[i] - rb[i];
      s[i] = std::max(0.f, -dl) + 0.5f * std::max(0.f, dr);
      const float cl = std::clamp(dl, -0.2f, 0.2f), cr = std::clamp(dr, -0.2f, 0.2f);
      e2[i] = Ms[i] * (cl * cl + 0.25f * cr * cr);
    }
    gauss(e2, gme, w, h, rho);
    if (j == 0) {  // fine-band energy: the texture measure of the source search
      tex.resize(N);
      for (std::size_t i = 0; i < N; ++i) tex[i] = std::sqrt(gme[i] / std::max(gm[i], 1e-3f));
    }
    for (std::size_t i = 0; i < N; ++i) {
      const float rms = std::sqrt(gme[i] / std::max(gm[i], 1e-3f));
      const float z = s[i] / std::max(rms, 0.012f);
      if (z > zmax[i]) zmax[i] = z, smax[i] = s[i], jmax[i] = j;
    }
  }
  // candidates: local maxima of z, strong enough, inside the mask
  struct Cand {
    int x, y, j;
    float z, s;
  };
  std::vector<Cand> cand;
  for (int y = 1; y + 1 < h; ++y)
    for (int x = 1; x + 1 < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      const float z = zmax[i];
      // a blemish is a moderate spot inside skin: much darker than ~30 % (nostrils, eye and mouth corners, a scarf or hair edge) is a feature
      if (z < 0.6f * z0 || smax[i] < smin || smax[i] > 0.5f || M[i] < 0.5f) continue;
      {  // the whole spot and its ring inside the mask (the mask's own edge next to a dark feature is not a blemish)
        const float rad = 1.6f * std::max(1.5f, 2.4f * scales[jmax[i]]);
        bool inside = true;
        for (int q = 0; q < 16 && inside; ++q) {
          const int xx = int(std::floor(float(x) + 0.5f + rad * kRing[q][0])), yy = int(std::floor(float(y) + 0.5f + rad * kRing[q][1]));
          inside = xx >= 0 && yy >= 0 && xx < w && yy < h && M[std::size_t(yy) * w + xx] >= 0.5f;
        }
        if (!inside) continue;
      }
      const int rr = std::max(1, int(std::lround(scales[jmax[i]])));
      bool peak = true;
      for (int dy = -rr; dy <= rr && peak; ++dy)
        for (int dx = -rr; dx <= rr && peak; ++dx) {
          if (!dx && !dy) continue;
          const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
          const float zz = zmax[std::size_t(yy) * w + xx];
          if (zz > z || (zz == z && (dy < 0 || (dy == 0 && dx < 0)))) peak = false;  // ties: the first in raster order wins
        }
      if (peak) cand.push_back({x, y, jmax[i], z, smax[i]});
    }
  // crowding: many blob-like spots close together are freckles (or pores of a coarse skin), not blemishes: raise the bar
  const float crowdR2 = (0.3f * iodPx) * (0.3f * iodPx);
  std::vector<Blob> keep;
  for (const Cand& c : cand) {
    int n = 0;
    for (const Cand& d : cand)
      if (float((d.x - c.x) * (d.x - c.x) + (d.y - c.y) * (d.y - c.y)) <= crowdR2) ++n;  // includes c itself
    const float zeff = c.z / (1.f + 0.4f * float(std::max(0, n - 2)));
    if (zeff < z0) continue;
    Blob b2;
    b2.x = float(c.x) + 0.5f, b2.y = float(c.y) + 0.5f;
    b2.r = std::max(1.5f, 2.4f * scales[c.j]);
    b2.z = zeff;
    b2.conf = sstep(z0, z0 + 1.5f, zeff) * sstep(smin, 2.f * smin, c.s);
    keep.push_back(b2);
  }
  std::stable_sort(keep.begin(), keep.end(), [](const Blob& p, const Blob& q) { return p.z > q.z; });
  for (const Blob& b2 : keep) {
    bool clash = false;
    for (const Blob& o : out)
      if (std::hypot(o.x - b2.x, o.y - b2.y) < 0.8f * (o.r + b2.r)) clash = true;
    if (!clash) out.push_back(b2);
    if (out.size() >= 96) break;
  }
  if (!searchSources || out.empty()) return out;
  // source search: 16 candidate offsets (8 directions x 2 distances); the patch must be clean skin (inside the mask, no blob, no other spot)
  // of a similar low band (shading), colour and texture energy; the nearest wins ties (deterministic order)
  std::vector<float> lowL, lowR;
  gauss(L, lowL, w, h, std::max(1.f, 0.05f * iodPx));
  gauss(R, lowR, w, h, std::max(1.f, 0.05f * iodPx));
  auto discStats = [&](float cx, float cy, float r, float& mMean, float& zMax, float& l, float& rd, float& tx, bool& inside) {
    double sm = 0, sl = 0, sr = 0, st = 0;
    int n = 0;
    zMax = 0.f;
    inside = cx - r >= 0.f && cy - r >= 0.f && cx + r <= float(w) && cy + r <= float(h);
    const int x0 = std::max(0, int(std::floor(cx - r))), x1 = std::min(w - 1, int(std::ceil(cx + r)));
    const int y0 = std::max(0, int(std::floor(cy - r))), y1 = std::min(h - 1, int(std::ceil(cy + r)));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
        if (dx * dx + dy * dy > r * r) continue;
        const std::size_t i = std::size_t(y) * w + x;
        sm += M[i], sl += lowL[i], sr += lowR[i], st += tex[i], ++n;
        zMax = std::max(zMax, zmax[i]);
      }
    if (!n) {
      mMean = 0, l = 0, rd = 0, tx = 0;
      return;
    }
    mMean = float(sm / n), l = float(sl / n), rd = float(sr / n), tx = float(st / n);
  };
  for (std::size_t k = 0; k < out.size(); ++k) {
    Blob& b2 = out[k];
    // reference: the ring just outside the spot
    double rl = 0, rr2 = 0, rt = 0;
    for (int q = 0; q < 16; ++q) {
      const int x = std::clamp(int(std::floor(b2.x + 1.3f * b2.r * kRing[q][0])), 0, w - 1);
      const int y = std::clamp(int(std::floor(b2.y + 1.3f * b2.r * kRing[q][1])), 0, h - 1);
      const std::size_t i = std::size_t(y) * w + x;
      rl += lowL[i], rr2 += lowR[i], rt += tex[i];
    }
    rl /= 16, rr2 /= 16, rt /= 16;
    float best = 1e30f, bx = 0, by = 0;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass)
      for (int di = 0; di < 2; ++di)
        for (int a8 = 0; a8 < 8; ++a8) {
          const float d = (di == 0 ? 2.5f : 3.5f) * b2.r;
          const float ox = d * kRing[2 * a8][0], oy = d * kRing[2 * a8][1];
          float mMean, zMax, l, rd, tx;
          bool inside;
          discStats(b2.x + ox, b2.y + oy, 1.3f * b2.r, mMean, zMax, l, rd, tx, inside);
          if (!inside || mMean < 0.6f) continue;
          if (pass == 0) {
            if (zMax > 0.5f * z0) continue;
            bool other = false;
            for (std::size_t o = 0; o < out.size() && !other; ++o)
              if (o != k && std::hypot(out[o].x - (b2.x + ox), out[o].y - (b2.y + oy)) < out[o].r + 1.3f * b2.r) other = true;
            if (other) continue;
          }
          const float cost = std::fabs(l - float(rl)) + 0.5f * std::fabs(rd - float(rr2)) +
                             0.25f * std::fabs(std::log((tx + 1e-3f) / (float(rt) + 1e-3f))) + 0.01f * d / b2.r;
          if (cost < best) best = cost, bx = ox, by = oy, found = true;
        }
    if (!found) bx = 2.5f * b2.r, by = 0.f;  // nothing clean nearby: the first direction
    b2.sx = bx, b2.sy = by;
  }
  return out;
}

// ---- bind time ---------------------------------------------------------------------------------------------------------------------------
float retouchIodOf(const LocalAdjustment& a, const Frame& f, int adj, const SourceImage& src) {
  if (a.face > 0.f) return a.face;
  if (adj < 0 || src.linear.width <= 0) return 0.06f;
  // estimate from the mask's area on the 64 x 64 guide samples (the pixels of the streaming capture when installed): a face-skin mask of one face
  // covers about 3.8 iod^2, an under-eye pair about 0.2 iod^2, a face oval about 4.6 iod^2
  const int W = src.linear.width, H = src.linear.height;
  double area = 0;
  for (int j = 0; j < 64; ++j)
    for (int i = 0; i < 64; ++i) {
      int x, y;
      guideSamplePos(W, H, i, j, x, y);
      float p[3];
      guideSample(src, i, j, p);  // the streaming export's capture when installed (the source then carries its size only)
      area += maskAt(f, adj, (float(x) + 0.5f) / float(W), (float(y) + 0.5f) / float(H), p);
    }
  area *= double(f.sx) * f.sy / 4096.0;
  const auto& v = a.ops.v;
  const bool skin = v[kLoSmooth] > 0 || v[kLoEven] > 0 || v[kLoRedness] > 0 || v[kLoShine] > 0 || v[kLoHeal] > 0;
  const float per = skin ? 3.8f : v[kLoUnderEye] > 0 ? 0.2f : 4.6f;
  if (area <= 0) return 0.06f;
  return std::clamp(float(std::sqrt(area / per)), 0.01f, 0.4f);
}

int healGridLong(const LocalAdjustment& a, int W, int H) {
  const int L = std::max(W, H);
  const int G = a.face > 0.f && a.face < 0.07f ? 2048 : 1024;
  return std::max(64, std::min(G, L));
}

void healSpotsOf(const LocalAdjustment& a, const Frame& f, int adj, const SourceImage& src, float iod, std::vector<SpotM>& out) {
  const bool autoHeal = a.ops.v[kLoHeal] > 0.f && adj >= 0;
  bool needSearch = autoHeal;
  for (const HealSpot& h : a.spots) needSearch = needSearch || !h.src;
  const float weight = a.strength / 100.f;
  if (!needSearch) {
    for (const HealSpot& h : a.spots) out.push_back({h.x * f.sx, h.y * f.sy, h.r, h.dx, h.dy, weight});
    return;
  }
  const int W = src.linear.width, H = src.linear.height;
  if (W <= 0 || H <= 0) return;
  const int G = healGridLong(a, W, H);
  const int gw = std::max(8, int(std::lround(float(G) * f.sx))), gh = std::max(8, int(std::lround(float(G) * f.sy)));
  float ref = 1.f;
  std::uint64_t gkey = 0;
  const std::shared_ptr<const Plane> grid = guideGridOf(src, gw, gh, ref, gkey);
  if (!grid) return;
  const float pxPerMetric = float(G);  // grid pixels per photo metric unit (gw = G * sx)
  const float iodG = iod * pxPerMetric;
  // region: the mask's extent (+ the manual spots), with a margin for the statistics and the source search
  int x0 = gw, y0 = gh, x1 = -1, y1 = -1;
  std::vector<float> fullMask;
  auto pixelAt = [&](int i, int j, float px[3]) {
    const float* g = grid->data.data() + (std::size_t(j) * gw + i) * 3;
    for (int c = 0; c < 3; ++c) px[c] = g[c] * g[c] * ref;
  };
  if (autoHeal) {
    // coarse pass on every 4th grid pixel to find the extent
    for (int j = 0; j < gh; j += 4)
      for (int i = 0; i < gw; i += 4) {
        float px[3];
        pixelAt(i, j, px);
        if (maskAt(f, adj, (float(i) + 0.5f) / float(gw), (float(j) + 0.5f) / float(gh), px) > 0.05f)
          x0 = std::min(x0, i), y0 = std::min(y0, j), x1 = std::max(x1, i), y1 = std::max(y1, j);
      }
  }
  for (const HealSpot& h : a.spots) {
    const int cx = int(h.x * float(gw)), cy = int(h.y * float(gh)), r = int(std::ceil(h.r * pxPerMetric));
    x0 = std::min(x0, cx - r), y0 = std::min(y0, cy - r), x1 = std::max(x1, cx + r), y1 = std::max(y1, cy + r);
  }
  const int margin = int(std::ceil(std::max(8.f, 0.6f * iodG)));
  x0 = std::max(0, x0 - margin), y0 = std::max(0, y0 - margin), x1 = std::min(gw - 1, x1 + margin), y1 = std::min(gh - 1, y1 + margin);
  if (x1 < x0 || y1 < y0) {  // empty mask
    for (const HealSpot& h : a.spots)
      if (h.src) out.push_back({h.x * f.sx, h.y * f.sy, h.r, h.dx, h.dy, weight});
    return;
  }
  const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
  // cache: the grid content, the region, the mask (components and rasters), the scale and the sensitivity
  std::uint64_t key = mix64(mix64(gkey, 0x4EA1), std::uint64_t(x0) << 48 | std::uint64_t(y0) << 32 | std::uint64_t(cw) << 16 | std::uint64_t(ch));
  key = mixf(mixf(key, iod), a.healSens);
  key = mix64(key, autoHeal ? 1 : 0);
  {
    LocalAdjustment m = a;
    m.ops = LocalOps{};
    m.strength = 100.f;
    m.enabled = true;
    m.spots.clear();
    m.face = 0.f;
    m.extraKeys.clear(), m.extraOps.clear();
    key = mix64(key, localHash({m}));
    key = mix64(key, std::uint64_t(f.adj.empty() || adj < 0 ? 0 : (f.adj[std::size_t(adj)].invert ? 1 : 0)));
  }
  for (const HealSpot& h : a.spots) key = mixf(mixf(mixf(key, h.x), h.y), h.r), key = mix64(key, h.src ? 1 : 0);
  std::shared_ptr<const Plane> res = planeCacheFind(key);
  if (!res) {
    std::vector<float> rgb(std::size_t(cw) * ch * 3), mk(std::size_t(cw) * ch, 1.f);
    parallelFor(ch, [&](int j) {
      for (int i = 0; i < cw; ++i) {
        float px[3];
        pixelAt(x0 + i, y0 + j, px);
        std::memcpy(rgb.data() + (std::size_t(j) * cw + i) * 3, px, sizeof px);
        if (adj >= 0)
          mk[std::size_t(j) * cw + i] = maskAt(f, adj, (float(x0 + i) + 0.5f) / float(gw), (float(y0 + j) + 0.5f) / float(gh), px);
      }
    });
    auto pl = std::make_shared<Plane>();
    pl->ch = 7, pl->h = 1, pl->key = key;
    std::vector<float>& d = pl->data;
    // manual spots without a source: the same search with the spot as the blob (mask: the adjustment's, or everything when it has none)
    std::vector<Blob> blobs;
    if (autoHeal) blobs = detectBlobs(rgb.data(), mk.data(), cw, ch, iodG, a.healSens, true);
    if (autoHeal) logLine("local adjustments: heal detection on a " + std::to_string(cw) + " x " + std::to_string(ch) + " region (iod " + std::to_string(int(iodG)) + " px): " + std::to_string(blobs.size()) + " spot(s)");
    for (const Blob& b2 : blobs) {
      const float mc = mk[std::size_t(std::clamp(int(b2.y), 0, ch - 1)) * cw + std::size_t(std::clamp(int(b2.x), 0, cw - 1))];
      const float vals[7] = {(float(x0) + b2.x) / pxPerMetric, (float(y0) + b2.y) / pxPerMetric, b2.r / pxPerMetric, b2.sx / pxPerMetric,
                             b2.sy / pxPerMetric, b2.conf, mc};
      d.insert(d.end(), vals, vals + 7);
    }
    for (const HealSpot& h : a.spots) {
      float vals[7] = {h.x * f.sx, h.y * f.sy, h.r, h.dx, h.dy, -1.f, 1.f};  // conf -1 = manual
      if (!h.src) {
        std::vector<float> mk2 = adj >= 0 ? mk : std::vector<float>(mk.size(), 1.f);
        const float bx = h.x * float(gw) - float(x0), by = h.y * float(gh) - float(y0), br = h.r * pxPerMetric;
        // candidates around the spot (as the automatic search): mask mean >= 0.6, inside the region, the closest low band to the ring around the spot
        float best = 1e30f, sx = 2.5f * br, sy = 0.f;
        for (int di = 0; di < 2; ++di)
          for (int a8 = 0; a8 < 8; ++a8) {
            const float dd = (di == 0 ? 2.5f : 3.5f) * br;
            const float ox = dd * kRing[2 * a8][0], oy = dd * kRing[2 * a8][1];
            const float cx = bx + ox, cy = by + oy, rr = 1.3f * br;
            if (cx - rr < 0 || cy - rr < 0 || cx + rr > float(cw) || cy + rr > float(ch)) continue;
            double sm = 0, sl = 0;
            int n = 0;
            for (int y = zeroBased(int(cy - rr)); y <= std::min(ch - 1, int(cy + rr)); ++y)
              for (int x = zeroBased(int(cx - rr)); x <= std::min(cw - 1, int(cx + rr)); ++x) {
                const float ex = float(x) + 0.5f - cx, ey = float(y) + 0.5f - cy;
                if (ex * ex + ey * ey > rr * rr) continue;
                const std::size_t i = std::size_t(y) * cw + x;
                const float* p = rgb.data() + 3 * i;
                sm += mk2[i];
                sl += std::log2(std::max(1e-5f, p[0] * kLumaPP[0] + p[1] * kLumaPP[1] + p[2] * kLumaPP[2]));
                ++n;
              }
            if (!n || sm / n < 0.6) continue;
            // reference: the ring around the spot
            double rl = 0;
            for (int q = 0; q < 16; ++q) {
              const int x = std::clamp(int(std::floor(bx + 1.3f * br * kRing[q][0])), 0, cw - 1);
              const int y = std::clamp(int(std::floor(by + 1.3f * br * kRing[q][1])), 0, ch - 1);
              const float* p = rgb.data() + 3 * (std::size_t(y) * cw + x);
              rl += std::log2(std::max(1e-5f, p[0] * kLumaPP[0] + p[1] * kLumaPP[1] + p[2] * kLumaPP[2]));
            }
            const float cost = float(std::fabs(sl / n - rl / 16)) + 0.01f * dd / br;
            if (cost < best) best = cost, sx = ox, sy = oy;
          }
        vals[3] = sx / pxPerMetric, vals[4] = sy / pxPerMetric;
      }
      d.insert(d.end(), vals, vals + 7);
    }
    pl->w = int(d.size() / 7);
    res = pl;
    planeCachePut(key, pl);
  }
  // manual spots first (the user's intent), then the automatic ones (their opacity: heal amount x strength x mask at the centre x confidence)
  const float amount = a.ops.v[kLoHeal] / 100.f;
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < res->w; ++i) {
      const float* v = res->data.data() + std::size_t(i) * 7;
      const bool manual = v[5] < 0.f;
      if (manual != (pass == 0)) continue;
      SpotM s;
      s.x = v[0], s.y = v[1], s.r = v[2], s.ox = v[3], s.oy = v[4];
      s.alpha = manual ? weight : std::clamp(amount * weight * v[5] * std::min(1.f, v[6] * 1.25f), 0.f, 1.f);
      if (s.alpha > 0.004f) out.push_back(s);
    }
}

// ---- frame geometry -----------------------------------------------------------------------------------------------------------------------
namespace {
void framePoint(const FramePlan& P, double x, double y, double& X, double& Y) {
  const Frame* lf = P.local.get();
  const bool win = lf && lf->parentW > 0 && lf->parentH > 0;  // a region source: the metric is the parent picture's (section 44)
  const double L = win ? double(std::max(lf->parentW, lf->parentH)) : double(std::max(P.srcW, P.srcH));
  double sx, sy;
  if (!P.useGeo) {
    sx = x + 0.5, sy = y + 0.5;
  } else {
    const double pxf = double(P.ox0) + (x + 0.5) * double(P.stepX), pyf = double(P.oy0) + (y + 0.5) * double(P.stepY);
    double uu = pxf - double(P.fullW) * 0.5;
    const double vv = pyf - double(P.fullH) * 0.5;
    if (P.flip) uu = -uu;
    const double fx = uu * P.cs + vv * P.sn + double(P.fw) * 0.5, fy = -uu * P.sn + vv * P.cs + double(P.fh) * 0.5;
    const double sW = double(P.srcW), sH = double(P.srcH);
    if (P.rot == 0) sx = fx, sy = fy;
    else if (P.rot == 1) sx = fy, sy = sH - fx;
    else if (P.rot == 2) sx = sW - fx, sy = sH - fy;
    else sx = sW - fy, sy = fx;
  }
  if (win) sx += double(lf->parentX0), sy += double(lf->parentY0);
  X = sx / L, Y = sy / L;
}
}  // namespace

Affine frameAffine(const FramePlan& P) {
  Affine A;
  double X0, Y0, X1, Y1, X2, Y2;
  framePoint(P, 0, 0, X0, Y0);
  framePoint(P, 1, 0, X1, Y1);
  framePoint(P, 0, 1, X2, Y2);
  A.m[0] = X1 - X0, A.m[1] = X2 - X0, A.m[2] = X0;
  A.m[3] = Y1 - Y0, A.m[4] = Y2 - Y0, A.m[5] = Y0;
  return A;
}
double affinePpu(const Affine& A) {
  const double det = std::fabs(A.m[0] * A.m[4] - A.m[1] * A.m[3]);
  return det > 1e-30 ? 1.0 / std::sqrt(det) : 1.0;
}
float haloPpu(const Frame& f, float scale) { return std::max(2048.f * scale, f.srcLong); }
float retouchPpu(const Frame& f, float scale, const FramePlan& P) { return std::min(float(affinePpu(frameAffine(P))), haloPpu(f, scale)); }

RetouchRadii retouchRadii(float iodPx) {
  RetouchRadii r;
  r.iodPx = iodPx;
  r.rf = std::clamp(int(std::lround(kRtFine * iodPx)), 1, 64);
  r.rc = std::clamp(int(std::lround(kRtCoarse * iodPx / 4.f)), 1, 64);
  r.rs = std::clamp(int(std::lround(kRtRef * iodPx / 4.f)), 1, 64);
  r.rb = std::clamp(int(std::lround(kRtBlot * iodPx / 4.f)), 1, 64);
  return r;
}
int retouchPlanesHalo(const RetouchRadii& r) { return std::max({2 * r.rf + 2, 4 * (2 * r.rc + 2) + 4, 4 * (2 * r.rs + 2) + 4}); }
int healHalo(const Frame& f, float ppu) {
  float h = 0.f;
  for (const SpotM& s : f.spots) {
    const float R = s.r * ppu, stride = std::max(1.f, std::round(R / 6.f));
    h = std::max(h, std::hypot(s.ox, s.oy) * ppu + 2.15f * R + stride + 2.f);
  }
  return f.spots.empty() ? 0 : int(std::ceil(h)) + 2;
}

void spotsForWindow(const Frame& f, const Affine& A, int wx, int wy, int ww, int wh, std::vector<SpotPx>& out) {
  out.clear();
  const double det = A.m[0] * A.m[4] - A.m[1] * A.m[3];
  if (std::fabs(det) < 1e-30) return;
  const double i00 = A.m[4] / det, i01 = -A.m[1] / det, i10 = -A.m[3] / det, i11 = A.m[0] / det;
  const double ppu = affinePpu(A);
  for (const SpotM& s : f.spots) {
    const double X = double(s.x) - A.m[2], Y = double(s.y) - A.m[5];
    const double cx = i00 * X + i01 * Y, cy = i10 * X + i11 * Y;
    const float R = float(double(s.r) * ppu);
    if (R < 0.75f || s.alpha <= 0.f) continue;
    if (cx + R < double(wx) - 1 || cy + R < double(wy) - 1 || cx - R > double(wx + ww) + 1 || cy - R > double(wy + wh) + 1) continue;
    SpotPx p;
    p.cx = float(cx - wx), p.cy = float(cy - wy), p.R = R, p.alpha = std::min(1.f, s.alpha);
    p.ox = int(std::lround(i00 * s.ox + i01 * s.oy)), p.oy = int(std::lround(i10 * s.ox + i11 * s.oy));
    p.stride = std::max(1, int(std::lround(R / 6.f)));
    out.push_back(p);
  }
}

// ---- heal pass (twin: local_heal.comp) --------------------------------------------------------------------------------------------------
void healWindow(const std::vector<SpotPx>& spots, ImageF& img) {
  if (spots.empty()) return;
  const int w = img.width, h = img.height;
  const ImageF orig = img;  // every spot reads the input of the pass
  auto at = [&](int x, int y) { return orig.row(std::clamp(y, 0, h - 1)) + 3 * std::size_t(std::clamp(x, 0, w - 1)); };
  auto mean9 = [&](int x, int y, int s, float o[3]) {
    o[0] = o[1] = o[2] = 0.f;
    for (int j = -1; j <= 1; ++j)
      for (int i = -1; i <= 1; ++i) {
        const float* p = at(x + i * s, y + j * s);
        o[0] += p[0], o[1] += p[1], o[2] += p[2];
      }
    o[0] *= (1.f / 9.f), o[1] *= (1.f / 9.f), o[2] *= (1.f / 9.f);
  };
  for (const SpotPx& s : spots) {
    int rx[16], ry[16];
    float dlt[16][3];
    for (int k = 0; k < 16; ++k) {
      rx[k] = int(std::floor(s.cx + 1.15f * s.R * kRing[k][0] + 0.5f));
      ry[k] = int(std::floor(s.cy + 1.15f * s.R * kRing[k][1] + 0.5f));
      float a[3], b[3];
      mean9(rx[k], ry[k], s.stride, a);
      mean9(rx[k] + s.ox, ry[k] + s.oy, s.stride, b);
      for (int c = 0; c < 3; ++c) dlt[k][c] = a[c] - b[c];
    }
    const int x0 = std::max(0, int(std::floor(s.cx - s.R))), x1 = std::min(w - 1, int(std::ceil(s.cx + s.R)));
    const int y0 = std::max(0, int(std::floor(s.cy - s.R))), y1 = std::min(h - 1, int(std::ceil(s.cy + s.R)));
    const float e2 = (0.1f * s.R) * (0.1f * s.R);
    for (int y = y0; y <= y1; ++y) {
      float* row = img.row(y);
      for (int x = x0; x <= x1; ++x) {
        const float dx = float(x) - s.cx, dy = float(y) - s.cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist >= s.R) continue;
        float ws = 0.f, d[3] = {0.f, 0.f, 0.f};
        for (int k = 0; k < 16; ++k) {
          const float ex = float(x - rx[k]), ey = float(y - ry[k]);
          const float wk = 1.f / (ex * ex + ey * ey + e2);
          ws += wk;
          d[0] += wk * dlt[k][0], d[1] += wk * dlt[k][1], d[2] += wk * dlt[k][2];
        }
        const float* sp = at(x + s.ox, y + s.oy);
        const float a = s.alpha * (1.f - sstep(0.6f * s.R, s.R, dist));
        float* p = row + 3 * std::size_t(x);
        for (int c = 0; c < 3; ++c) {
          const float fill = sp[c] + d[c] / ws;
          p[c] = p[c] + (fill - p[c]) * a;
        }
      }
    }
  }
  parallelFor(h, [&](int y) {
    float* r = img.row(y);
    for (int i = 0; i < 3 * w; ++i) r[i] = std::clamp(r[i], 0.f, 1.f);
  });
}

// ---- retouch planes + combine (twins: local_rt_down.comp, local_retouch.comp) -------------------------------------------------------------
namespace {
// amounts of the six retouch W ops at plane index i: smooth, even, redness, under-eye, shine, face light
// (the slot -> op map resolved once per window: the amount planes of the retouch ops that have one)
struct RtMap {
  int n = 0;
  const float* plane[kLocalWindowSlots];
  int op[kLocalWindowSlots];
  RtMap(const Frame& f, const AuxTarget& aux) {
    for (int c = 0; c < kLocalWindowSlots; ++c) {
      const int o = f.chanOp[c];
      if (o < kLoSmooth || o > kLoFaceLight || aux.slot[c] < 0) continue;
      plane[n] = aux.data + std::size_t(aux.slot[c]) * aux.plane, op[n++] = o - kLoSmooth;
    }
  }
};
inline void rtAmounts(const RtMap& m, std::size_t i, float k[6]) {
  for (int c = 0; c < 6; ++c) k[c] = 0.f;
  for (int c = 0; c < m.n; ++c) k[m.op[c]] = m.plane[c][i];
}

// the per-pixel retouch (twin of local_retouch.comp main). Colour works on RELATIVE chroma q = (R - Y, B - Y) / Y (a saturation: the same skin
// in light and in shade has the same q), and only on its mid-scale deviation d = q_b - q_ref (the blotch-scale mean minus the skin reference):
// mean-free over the skin by construction, so the complexion (mean hue, saturation, lightness) does not move; per-pixel colour texture and
// the low-frequency blush (inside q_ref) are kept. Luminance changes keep q, i.e. the saturation.
inline void rtPixel(float p[3], float Lf, float Lc, const float ref[4], const float blot[4], const float kin[6]) {
  const float Y = p[0] * kLumaS[0] + p[1] * kLumaS[1] + p[2] * kLumaS[2];
  const float Yd = std::max(Y, kRtYmin);
  float qr = (p[0] - Y) / Yd, qb = (p[2] - Y) / Yd;
  float k[6];
  for (int c = 0; c < 6; ++c) k[c] = std::clamp(kin[c], 0.f, 1.f);
  const float ks = k[0], ke = k[1], kr = k[2], ku = k[3], kh = k[4], kl = k[5];
  float rY = Lf, rqr = qr, rqb = qb, bqr = qr, bqb = qb;
  if (ref[3] > 1e-6f) rY = ref[0] / ref[3], rqr = ref[1] / ref[3], rqb = ref[2] / ref[3];
  if (blot[3] > 1e-6f) bqr = blot[1] / blot[3], bqb = blot[2] / blot[3];
  const float dr = bqr - rqr, db = bqb - rqb;                          // mid-scale chroma deviation
  const float rs = std::max(std::sqrt(rqr * rqr + rqb * rqb), 0.05f);  // the skin's saturation (relative distances)
  // colours far from the skin (a glasses frame, lips, hair the mask leaks onto) are not blotches: left alone
  const float far = 1.f - sstep(0.6f, 1.2f, std::sqrt((qr - rqr) * (qr - rqr) + (qb - rqb) * (qb - rqb)) / rs);
  // smoothing / even tone: the mid band L_f - L_c, gated off at strong edges (|M| >> 0.08: jaw, eyes, nostrils)
  const float M = Lf - Lc;
  const float g = 1.f / (1.f + (M * 12.5f) * (M * 12.5f));
  float dY = -std::min(0.92f, 0.9f * ks + 0.4f * ke) * g * M;
  if (ku > 0.f) {  // under-eye: lift the low band toward the surrounding skin, its colour to the skin colour (a mid-scale shift: texture kept)
    const float d = rY - (Lf + dY);
    dY += 0.8f * ku * std::clamp(d, 0.f, 0.25f) * sstep(0.f, 0.03f, d);
    qr -= 0.8f * ku * dr, qb -= 0.8f * ku * db;
  }
  if (kh > 0.f) {  // shine: only highlights well above the skin; a monotone compression; pale (specular) ones regain the skin colour
    const float e = Lf + dY - rY, x = std::max(e - 0.05f, 0.f);
    const float pale = 1.f - sstep(0.5f, 0.9f, std::sqrt(bqr * bqr + bqb * bqb) / rs);
    dY -= 0.6f * kh * (0.4f + 0.6f * pale) * x * x / (x + 0.08f);
    const float t = 0.8f * kh * pale * sstep(0.05f, 0.15f, e);
    qr += (rqr - qr) * t, qb += (rqb - qb) * t;
  }
  // even: the mid-scale HUE mottling fully, its saturation part only a little (darker skin is legitimately richer in its shadows: evening the
  // saturation there takes the colour depth); redness: where the mid-scale colour is redder than the skin (blotches), a soft threshold (only the
  // excess over 15 % of the skin saturation). The saturation |q| changes only by the removed radial part (no |mean| < mean |.| loss).
  const float te = ke > 0.f ? 0.75f * ke * far : 0.f;
  const float tr = kr > 0.f ? std::min(1.f, 1.5f * kr * far * std::max(0.f, 1.f - 0.15f / std::max((0.95f * dr + 0.31f * db) / rs, 1e-3f))) : 0.f;
  if (te + tr > 0.f) {
    const float ur = rqr / rs, ub = rqb / rs, dRad = dr * ur + db * ub;
    const float tanR = dr - dRad * ur, tanB = db - dRad * ub;
    const float tt = std::min(te + tr, 1.f), trad = std::min(0.3f * te + tr, 1.f);
    const float m0 = std::sqrt(qr * qr + qb * qb);
    qr -= tt * tanR + trad * dRad * ur, qb -= tt * tanB + trad * dRad * ub;
    const float m1 = std::sqrt(qr * qr + qb * qb), m = std::max(m0 - trad * dRad, 0.f);
    if (m1 > 1e-4f) qr *= m / m1, qb *= m / m1;
  }
  const float Y2 = Y + dY, Y2d = Y >= kRtYmin ? std::max(Y2, 0.f) : Yd;
  float r = Y2 + qr * Y2d, b = Y2 + qb * Y2d;
  float gg = (Y2 - kLumaS[0] * r - kLumaS[2] * b) / kLumaS[1];
  if (kl > 0.f) {  // face light: a broad luminance gain from the coarse base (dodge), chroma by its square root (no orange push on dark skin)
    const float G = 1.f + 0.45f * kl * (1.f - sstep(0.15f, 0.75f, Lc)), sc = std::sqrt(G);
    const float Y3 = Y2 * G;
    r = Y3 + (r - Y2) * sc, gg = Y3 + (gg - Y2) * sc, b = Y3 + (b - Y2) * sc;
  }
  p[0] = std::clamp(r, 0.f, 1.f), p[1] = std::clamp(gg, 0.f, 1.f), p[2] = std::clamp(b, 0.f, 1.f);
}

inline void bilin(const std::vector<float>& g, int lw, int lh, int ch, int x, int y, float* o) {
  const float py = std::clamp((float(y) + 0.5f) / 4.f - 0.5f, 0.f, float(lh - 1));
  const int y0 = std::min(int(py), lh - 1), y1 = std::min(y0 + 1, lh - 1);
  const float ty = py - float(y0);
  const float pxf = std::clamp((float(x) + 0.5f) / 4.f - 0.5f, 0.f, float(lw - 1));
  const int x0 = std::min(int(pxf), lw - 1), x1 = std::min(x0 + 1, lw - 1);
  const float tx = pxf - float(x0);
  for (int c = 0; c < ch; ++c) {
    const float a0 = g[(std::size_t(y0) * lw + x0) * ch + c], a1 = g[(std::size_t(y0) * lw + x1) * ch + c];
    const float b0 = g[(std::size_t(y1) * lw + x0) * ch + c], b1 = g[(std::size_t(y1) * lw + x1) * ch + c];
    const float t0 = a0 + (a1 - a0) * tx, t1 = b0 + (b1 - b0) * tx;
    o[c] = t0 + (t1 - t0) * ty;
  }
}
// bilin's corners and weights for pixel (x, y), shared by the three grids of the combine (the same arithmetic as bilin: bit-identical)
struct BilinAt {
  std::size_t c00, c01, c10, c11;
  float tx, ty;
};
inline BilinAt bilinAt(int lw, int lh, int x, int y) {
  const float py = std::clamp((float(y) + 0.5f) / 4.f - 0.5f, 0.f, float(lh - 1));
  const int y0 = std::min(int(py), lh - 1), y1 = std::min(y0 + 1, lh - 1);
  const float pxf = std::clamp((float(x) + 0.5f) / 4.f - 0.5f, 0.f, float(lw - 1));
  const int x0 = std::min(int(pxf), lw - 1), x1 = std::min(x0 + 1, lw - 1);
  BilinAt b;
  b.ty = py - float(y0), b.tx = pxf - float(x0);
  b.c00 = std::size_t(y0) * lw + x0, b.c01 = std::size_t(y0) * lw + x1, b.c10 = std::size_t(y1) * lw + x0, b.c11 = std::size_t(y1) * lw + x1;
  return b;
}
template <int CH>
inline void bilinW(const float* g, const BilinAt& w, float* o) {
  for (int c = 0; c < CH; ++c) {
    const float a0 = g[w.c00 * CH + c], a1 = g[w.c01 * CH + c], b0 = g[w.c10 * CH + c], b1 = g[w.c11 * CH + c];
    const float t0 = a0 + (a1 - a0) * w.tx, t1 = b0 + (b1 - b0) * w.tx;
    o[c] = t0 + (t1 - t0) * w.ty;
  }
}
}  // namespace

void retouchPlanes(const Frame& f, const ImageF& img, const AuxTarget& aux, const RetouchRadii& R, RtPlanes& out) {
  const int w = img.width, h = img.height;
  const std::size_t N = std::size_t(w) * h;
  out.w = w, out.h = h;
  std::vector<float>& Lf = out.Lf;
  Lf.assign(N, 0.f);
  parallelFor(h, [&](int y) {
    const float* p = img.row(y);
    for (int x = 0; x < w; ++x) Lf[std::size_t(y) * w + x] = kLumaS[0] * p[3 * x] + kLumaS[1] * p[3 * x + 1] + kLumaS[2] * p[3 * x + 2];
  });
  boxRepeat(Lf, w, h, 1, R.rf);
  boxRepeat(Lf, w, h, 1, R.rf);
  const int lw = (w + 3) / 4, lh = (h + 3) / 4;
  out.lw = lw, out.lh = lh;
  std::vector<float>& yy = out.ab;
  std::vector<float>& S = out.S;
  yy.assign(std::size_t(lw) * lh * 2, 0.f), S.assign(std::size_t(lw) * lh * 4, 0.f);
  const RtMap map(f, aux);
  parallelFor(lh, [&](int ly) {
    for (int lx = 0; lx < lw; ++lx) {
      const int x0 = lx * 4, y0 = ly * 4, x1 = std::min(w, x0 + 4), y1 = std::min(h, y0 + 4);
      float acc[3] = {0, 0, 0}, sk[4] = {0, 0, 0, 0};
      for (int y = y0; y < y1; ++y) {
        const float* p = img.row(y);
        for (int x = x0; x < x1; ++x) {
          for (int c = 0; c < 3; ++c) acc[c] += p[3 * x + c];
          float k[6];
          rtAmounts(map, std::size_t(y) * w + x, k);
          const float om = skinWeight(k[0], k[1], k[2], k[4], k[3]);
          const float Y = kLumaS[0] * p[3 * x] + kLumaS[1] * p[3 * x + 1] + kLumaS[2] * p[3 * x + 2], Yd = std::max(Y, kRtYmin);
          sk[0] += om * Y, sk[1] += om * ((p[3 * x] - Y) / Yd), sk[2] += om * ((p[3 * x + 2] - Y) / Yd), sk[3] += om;
        }
      }
      const float inv = 1.f / float((y1 - y0) * (x1 - x0));
      const float yv = (acc[0] * inv) * kLumaS[0] + (acc[1] * inv) * kLumaS[1] + (acc[2] * inv) * kLumaS[2];
      const std::size_t o = std::size_t(ly) * lw + lx;
      yy[o * 2] = yv, yy[o * 2 + 1] = yv * yv;
      for (int c = 0; c < 4; ++c) S[o * 4 + std::size_t(c)] = sk[c] * inv;
    }
  });
  boxRepeat(yy, lw, lh, 2, R.rc);
  for (std::size_t i = 0; i < std::size_t(lw) * lh; ++i) {
    const float m = yy[2 * i], v = std::max(0.f, yy[2 * i + 1] - m * m);
    const float a = v / (v + kRtEps);
    yy[2 * i] = a;
    yy[2 * i + 1] = m - a * m;
  }
  boxRepeat(yy, lw, lh, 2, R.rc);
  out.B = S;  // the blotch-scale mean (small radius) and the skin reference (large radius) of the same weighted sums
  boxRepeat(out.B, lw, lh, 4, R.rb);
  boxRepeat(out.B, lw, lh, 4, R.rb);
  boxRepeat(S, lw, lh, 4, R.rs);
  boxRepeat(S, lw, lh, 4, R.rs);
}

float RtPlanes::Lc(int x, int y) const {
  float a[2];
  bilin(ab, lw, lh, 2, x, y, a);
  return a[0] * Lf[std::size_t(y) * w + x] + a[1];
}

void retouchCombineWindow(const Frame& f, ImageF& img, const AuxTarget& aux, const RetouchRadii& R) {
  RtPlanes P;
  retouchPlanes(f, img, aux, R, P);
  const int w = img.width, h = img.height, lw = P.lw, lh = P.lh;
  const std::vector<float>&Lf = P.Lf, &yy = P.ab, &S = P.S, &B = P.B;
  const RtMap map(f, aux);
  parallelFor(h, [&](int y) {
    float* p = img.row(y);
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      float k[6];
      rtAmounts(map, i, k);
      if (k[0] == 0.f && k[1] == 0.f && k[2] == 0.f && k[3] == 0.f && k[4] == 0.f && k[5] == 0.f) continue;
      float ab[2], ref[4], blot[4];
      const BilinAt bw = bilinAt(lw, lh, x, y);
      bilinW<2>(yy.data(), bw, ab);
      bilinW<4>(S.data(), bw, ref);
      bilinW<4>(B.data(), bw, blot);
      const float Lc = ab[0] * Lf[i] + ab[1];
      rtPixel(p + 3 * x, Lf[i], Lc, ref, blot, k);
    }
  });
}

void retouchDetailWindow(const Frame& f, ImageF& img, const AuxTarget& aux, float scale, const FramePlan& P) {
  const Affine A = frameAffine(P);
  if (!f.spots.empty()) {
    std::vector<SpotPx> sp;
    spotsForWindow(f, A, aux.ox, aux.oy, img.width, img.height, sp);
    healWindow(sp, img);
  }
  if (f.retouchW) {
    const float ppu = std::min(float(affinePpu(A)), haloPpu(f, scale));
    retouchCombineWindow(f, img, aux, retouchRadii(f.iod * ppu));
  }
}

}  // namespace dapple::local
