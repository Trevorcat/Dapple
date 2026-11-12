// Pass graph of one frame (or one tile of an export). The structure follows core/src/spatial.cpp applySpatial():
//   point (fused) -> luma NR -> chroma NR -> dehaze -> clarity + texture + sharpen -> grain
// with the large-scale analysis (dehaze airlight / dark channel, clarity base) on a 1/4 grid that, for a tiled export,
// is assembled from all tiles first (GlobalRes) so every tile applies the same global numbers.
#include "frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dapple::gpu::vk {

namespace {

constexpr VkFormat kF16 = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kF32 = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kR32 = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kU8 = VK_FORMAT_R8G8B8A8_UNORM;

int cdiv(int a, int b) { return (a + b - 1) / b; }
int lroundi(float v) { return int(std::lround(v)); }

struct PointU {
  float gain[4];
  std::int32_t a[4], b[4];
  float geoA[4], geoB[4], geoC[4];
  std::int32_t c[4], d[4];
  float e[4];
};
struct SepU {
  std::int32_t size[4], radii[4];
};
struct DownU {
  std::int32_t a[4], b[4], c[4];
};
struct NrU {
  std::int32_t dims[4];
  float nr[4];
};
struct NrCU {
  std::int32_t dims[4], low[4];
  float nr[4];
};
struct DehU {
  std::int32_t dims[4], low[4];
  float dh[4], grain[4];
};
struct ReduceU {
  std::int32_t s[4];
};
struct AbU {
  std::int32_t s[4];
  float eps[4];
};
struct DetU {
  std::int32_t dims[4], low[4];
  float gains[4], sharp[4], grain[4];
};

struct Ctx {
  Engine& e;
  Graph& g;
  const FrameJob& j;
  const FramePlan& P;
  const SpatialRadii R;
  std::string& err;
  int W, Hh;
  H d8 = -1, d16 = -1, d32 = -1, dR = -1, wbuf = -1, gbuf = -1, pbuf = -1;

  Ctx(Engine& e_, Graph& g_, const FrameJob& j_, std::string& err_)
      : e(e_), g(g_), j(j_), P(*j_.plan), R(spatialRadii(*j_.plan, j_.resizeSharpen)), err(err_), W(j_.work.w()), Hh(j_.work.h()) {}

  H dummy(VkFormat f) {
    H& h = f == kU8 ? d8 : f == kF16 ? d16 : f == kF32 ? d32 : dR;
    if (h < 0) h = g.external(e.dummyImage(f));
    return h;
  }
  Pipeline* pl(const char* n, std::initializer_list<std::pair<std::uint32_t, std::int32_t>> spec) {
    Pipeline* p = e.pipeline(n, spec, err);
    return p;
  }
  // working image with the job's allocation size when it is the work size (or its 1/2, 1/4 grids)
  H tr(VkFormat f, int w, int h) {
    std::uint32_t aw = 0, ah = 0;
    if (j.allocW > 0) {
      if (w == W && h == Hh) {
        aw = std::uint32_t(j.allocW);
        ah = std::uint32_t(j.allocH);
      } else {
        for (int f2 : {2, 4})
          if (w == cdiv(W, f2) && h == cdiv(Hh, f2)) {
            aw = std::uint32_t(cdiv(j.allocW, f2));
            ah = std::uint32_t(cdiv(j.allocH, f2));
          }
      }
    }
    return g.transient(f, std::uint32_t(w), std::uint32_t(h), aw, ah);
  }
  H weights() {
    if (wbuf < 0) wbuf = g.externalBuf(j.weights);
    return wbuf;
  }

  // one direction of a separable filter. kind: 0 rgba16f -> luma r32f, 1 rgba32f -> min r32f, 2 rgba32f, 3 r32f
  H sep(int kind, H in, int w, int h, int dir, int op, const std::array<int, 4>& rad, VkFormat outFmt) {
    static const char* names[3][4] = {{"sep_d2y", "sep_c2y", "sep_c", "sep_y"},
                                      {"sep_d2y_s", "sep_c2y_s", "sep_c_s", "sep_y_s"},
                                      {"sep_d2y_v", "sep_c2y_v", "", "sep_y_v"}};
    int rmax = 0;
    for (int c = 0; c < 4; ++c) rmax = std::max(rmax, std::min(rad[c], 64));
    // Small radii: 16 x 16 groups with a small tile. Large radii: 64 along the axis x 4 lines; vertical passes of single
    // channel images use 16 along x 16 lines with 4 outputs per thread.
    const bool small = rmax <= 8;
    const bool bigV = !small && dir == 1 && kind != 2;
    const int variant = small ? 1 : bigV ? 2 : 0;
    const int lx = (small || bigV) ? 16 : 64, ly = (small || bigV) ? 16 : 4;
    const int outsAlong = bigV ? 64 : lx;  // outputs along the axis per group
    const int perp = ly;                  // lines per group
    Pipeline* p = pl(names[variant][kind], {{0, dir}, {1, lx}, {2, ly}, {3, op}});
    if (!p) return -1;
    H out = tr(outFmt, w, h);
    SepU u{};
    u.size[0] = w;
    u.size[1] = h;
    for (int c = 0; c < 4; ++c) u.radii[c] = std::min(rad[c], 64);
    u.size[2] = rmax;
    const std::uint32_t gx = dir == 0 ? std::uint32_t(cdiv(w, outsAlong)) : std::uint32_t(cdiv(w, perp));
    const std::uint32_t gy = dir == 0 ? std::uint32_t(cdiv(h, perp)) : std::uint32_t(cdiv(h, outsAlong));
    g.dispatch(op == 2 ? "min" : op == 1 ? "gauss" : "box", p, {Graph::R(in), Graph::W(out), Graph::R(weights())}, gx, gy, &u, sizeof u);
    return out;
  }

  // `iters` x (horizontal, vertical). `first` = kind of the first pass, `work` = kind of the following ones.
  H blur(int first, int work, H in, int w, int h, const std::array<int, 4>& rad, int iters, int op, VkFormat outFmt) {
    H cur = in;
    for (int i = 0; i < iters; ++i) {
      H t = sep(i == 0 ? first : work, cur, w, h, 0, op, rad, outFmt);
      if (t < 0) return -1;
      cur = sep(work, t, w, h, 1, op, rad, outFmt);
      if (cur < 0) return -1;
    }
    return cur;
  }

  H dehazeAnalysis(H col, int lw, int lh, H partH, H gH);
  H clarityAnalysis(H yy, int lw, int lh);
  H down(H D, int f, int mode, H dst, const Rect& win /*in D coordinates*/, int offX, int offY, int cntX, int cntY) {
    Pipeline* p = pl("down", {{0, mode}});
    if (!p) return -1;
    DownU u{};
    u.a[0] = win.x1;
    u.a[1] = win.y1;
    u.a[2] = win.x0;
    u.a[3] = win.y0;
    u.b[0] = cntX;
    u.b[1] = cntY;
    u.b[2] = offX;
    u.b[3] = offY;
    u.c[0] = f;
    g.dispatch("down", p, {Graph::R(D), Graph::RW(dst)}, std::uint32_t(cdiv(cntX, 16)), std::uint32_t(cdiv(cntY, 16)), &u, sizeof u);
    return dst;
  }
};

// Dehaze analysis of the low-resolution colour grid: dark channel (min over rgb, min filter, smoothing), then the
// airlight by four reduction passes (written to `gH`). Returns the dark channel.
H dehazeAnalysisImpl(Ctx& c, H col, int lw, int lh, H partH, H gH) {
  const SpatialRadii& R = c.R;
  const int rm = R.dehazeMin, rb = R.dehazeBox;
  H t = c.sep(1, col, lw, lh, 0, 2, {rm, rm, rm, rm}, kR32);
  if (t < 0) return -1;
  t = c.sep(3, t, lw, lh, 1, 2, {rm, rm, rm, rm}, kR32);
  if (t < 0) return -1;
  const H dc = c.blur(3, 3, t, lw, lh, {rb, 0, 0, 0}, 2, 0, kR32);
  if (dc < 0) return -1;
  const int groups = cdiv(lw * lh, 4096);
  ReduceU u{};
  u.s[0] = lw;
  u.s[1] = lh;
  u.s[2] = groups;
  for (int mode = 0; mode < 4; ++mode) {
    Pipeline* p = c.pl("reduce", {{0, mode}});
    if (!p) return -1;
    c.g.dispatch("reduce", p, {Graph::R(dc), Graph::R(col), Graph::RW(gH), Graph::RW(partH)}, (mode & 1) ? 1u : std::uint32_t(groups), 1, &u, sizeof u);
  }
  return dc;
}

// Guided filter coefficients (a, b) of the clarity base from the (Y, Y^2) grid.
H clarityAnalysisImpl(Ctx& c, H yy, int lw, int lh) {
  const int r = c.R.clarityR;
  const H m = c.blur(2, 2, yy, lw, lh, {r, r, r, r}, 1, 0, kF32);
  if (m < 0) return -1;
  const H ab0 = c.tr(kF32, lw, lh);
  Pipeline* pa = c.pl("gf_ab", {});
  if (!pa) return -1;
  AbU u{};
  u.s[0] = lw;
  u.s[1] = lh;
  u.eps[0] = 0.045f * 0.045f;
  c.g.dispatch("gf_ab", pa, {Graph::R(m), Graph::W(ab0)}, std::uint32_t(cdiv(lw, 16)), std::uint32_t(cdiv(lh, 16)), &u, sizeof u);
  return c.blur(2, 2, ab0, lw, lh, {r, r, r, r}, 1, 0, kF32);
}

H Ctx::dehazeAnalysis(H col, int lw, int lh, H partH, H gH) { return dehazeAnalysisImpl(*this, col, lw, lh, partH, gH); }
H Ctx::clarityAnalysis(H yy, int lw, int lh) { return clarityAnalysisImpl(*this, yy, lw, lh); }

void grainArgs(const FramePlan& P, float& amp, float& period) {
  period = std::max(1.f, 1.6f * P.spatialScale);
  amp = P.grain * 0.13f * (period > 1.01f ? 1.5f : 1.f);
}

}  // namespace

SpatialRadii spatialRadii(const FramePlan& P, float resizeSharpen) {
  SpatialRadii r;
  const float scale = P.spatialScale;
  int haloL = 0, haloC = 0, haloD = 0;
  if (P.nrLuma > 0) {
    r.nrLumaR = std::max(1, lroundi((0.8f + 1.8f * P.nrLuma) * scale));
    haloL = 2 * r.nrLumaR + 1;
  }
  if (P.nrColor > 0) {
    const float rFull = (1.5f + 5.f * P.nrColor) * scale;
    r.nrChromaF = rFull >= 6.f ? 4 : 2;
    r.nrChromaRl = std::max(1, lroundi(rFull / float(r.nrChromaF)));
    haloC = (2 * r.nrChromaRl + 3) * r.nrChromaF;
  }
  if (P.dehaze > 0) {
    r.dehazeMin = std::max(1, lroundi(2.5f * scale));
    r.dehazeBox = std::max(1, lroundi(3.f * scale));
  }
  if (P.clarity != 0) r.clarityR = std::max(2, lroundi(0.02f * 2048.f * scale / 4.f));
  if (P.texture != 0) {
    r.textureR = std::max(1, lroundi(2.f * scale));
    haloD = std::max(haloD, 2 * r.textureR + 1);
  }
  r.sharpAmount = P.sharpen > 0 || resizeSharpen > 0 ? P.sharpen + resizeSharpen : 0.f;
  if (r.sharpAmount > 0) {
    r.sharpSigma = std::max(0.4f, P.sharpRadius * scale * 0.5f);
    if (r.sharpSigma > 2.6f) {
      r.sharpBox = std::max(1, lroundi(std::sqrt(r.sharpSigma * r.sharpSigma * 4.f / 3.f + 0.25f) - 0.5f));
      haloD = std::max(haloD, 3 * r.sharpBox + 1);
    } else {
      r.sharpGauss = std::max(1, int(std::ceil(r.sharpSigma * 3.f)));
      haloD = std::max(haloD, r.sharpGauss + 1);
    }
  }
  r.halo = (haloL + haloC + haloD + 8 + 3) / 4 * 4;
  return r;
}

bool buildFrame(Engine& e, Graph& g, const FrameJob& j, H* out8, std::string& err) {
  Ctx c(e, g, j, err);
  const FramePlan& P = c.P;
  const SpatialRadii& R = c.R;
  const int W = c.W, Hh = c.Hh;
  const bool finalMode = j.mode == Mode::Whole || j.mode == Mode::Final;
  const bool whole = j.mode == Mode::Whole;

  const bool nrL = P.nrLuma > 0, nrC = P.nrColor > 0, dz = P.dehaze != 0;
  const bool clar = P.clarity != 0, tex = P.texture != 0, shp = R.sharpAmount > 0;
  const bool detail = clar || tex || shp;
  const bool grain = P.grain > 0 && finalMode;
  float gAmp = 0, gPer = 1;
  grainArgs(P, gAmp, gPer);

  // which stage ends the chain
  enum { SPoint, SNrL, SNrC, SDz, SDetail } last = SPoint;
  if (nrL) last = SNrL;
  if (nrC) last = SNrC;
  if (dz) last = SDz;
  if (detail) last = SDetail;
  auto isLast = [&](int s) { return finalMode && last == s; };

  const H D = c.tr(kF16, W, Hh);
  H o8 = -1;
  if (finalMode) o8 = c.tr(kU8, W, Hh);
  auto outUse = [&](bool lastStage) { return lastStage ? Graph::W(o8) : Graph::R(c.dummy(kU8)); };
  *out8 = o8;
  const std::uint32_t gx = std::uint32_t(cdiv(W, 16)), gy = std::uint32_t(cdiv(Hh, 16));
  const int ox = j.work.x0, oy = j.work.y0;

  // ---- point-wise (fused) ----
  {
    const int geo = !P.useGeo ? 0 : (P.exact ? 1 : 2);
    Pipeline* p = c.pl("point", {{0, geo}, {10, isLast(SPoint) ? 1 : 0}, {11, isLast(SPoint) && grain ? 1 : 0}});
    if (!p) return false;
    PointU u{};
    for (int k = 0; k < 3; ++k) u.gain[k] = P.gain[k];
    u.a[0] = P.srcW;
    u.a[1] = P.srcH;
    u.a[2] = W;
    u.a[3] = Hh;
    u.b[0] = j.src.ox;
    u.b[1] = j.src.oy;
    u.b[2] = ox;
    u.b[3] = oy;
    u.geoA[0] = P.cs;
    u.geoA[1] = P.sn;
    u.geoA[2] = P.fw;
    u.geoA[3] = P.fh;
    u.geoB[0] = P.ox0;
    u.geoB[1] = P.oy0;
    u.geoB[2] = P.stepX;
    u.geoB[3] = P.stepY;
    u.geoC[0] = P.fullW;
    u.geoC[1] = P.fullH;
    u.geoC[2] = P.vignette;
    u.c[0] = P.rot;
    u.c[1] = P.flip ? 1 : 0;
    u.c[2] = j.lut.n;
    u.c[3] = j.lut.toneN;
    u.d[0] = j.lut.toneBase;
    u.d[1] = j.lut.n2;
    u.e[0] = j.lut.max1;
    u.e[1] = j.lut.max2;
    u.e[2] = gAmp;
    u.e[3] = gPer;
    g.dispatch("point", p, {Graph::R(j.src.img), Graph::R(j.lut.tone), Graph::R(j.lut.lat), Graph::R(j.lut.lat2), Graph::W(D), outUse(isLast(SPoint))}, gx, gy, &u, sizeof u);
  }

  // ---- luma noise reduction ----
  if (nrL) {
    const H Yb = c.blur(0, 3, D, W, Hh, {R.nrLumaR, 0, 0, 0}, 2, 0, kR32);
    if (Yb < 0) return false;
    Pipeline* p = c.pl("nr_luma", {{10, isLast(SNrL) ? 1 : 0}, {11, isLast(SNrL) && grain ? 1 : 0}});
    if (!p) return false;
    NrU u{};
    u.dims[0] = W;
    u.dims[1] = Hh;
    u.dims[2] = ox;
    u.dims[3] = oy;
    const float a = P.nrLuma, sigma = 0.006f + 0.05f * a;
    u.nr[0] = std::min(1.f, std::pow(a, 0.75f) * 1.15f);
    u.nr[1] = 1.f / (sigma * sigma);
    u.nr[2] = gAmp;
    u.nr[3] = gPer;
    g.dispatch("nr_luma", p, {Graph::RW(D), Graph::R(Yb), outUse(isLast(SNrL))}, gx, gy, &u, sizeof u);
  }

  // ---- chroma noise reduction (local low-resolution grid, origin aligned to the factor) ----
  if (nrC) {
    const int f = R.nrChromaF, lw = cdiv(W, f), lh = cdiv(Hh, f);
    const H low = c.tr(kF32, lw, lh);
    c.down(D, f, 2, low, Rect{0, 0, W, Hh}, 0, 0, lw, lh);
    const int rl = R.nrChromaRl;
    const H bl = c.blur(2, 2, low, lw, lh, {rl, rl, std::max(1, rl / 2), 0}, 2, 0, kF32);
    if (bl < 0) return false;
    Pipeline* p = c.pl("nr_chroma", {{10, isLast(SNrC) ? 1 : 0}, {11, isLast(SNrC) && grain ? 1 : 0}});
    if (!p) return false;
    NrCU u{};
    u.dims[0] = W;
    u.dims[1] = Hh;
    u.dims[2] = ox;
    u.dims[3] = oy;
    u.low[0] = lw;
    u.low[1] = lh;
    u.low[2] = f;
    u.nr[0] = std::min(1.f, P.nrColor * 1.4f);
    u.nr[1] = 1.f / (0.05f * 0.05f);
    u.nr[2] = gAmp;
    u.nr[3] = gPer;
    g.dispatch("nr_chroma", p, {Graph::RW(D), Graph::R(bl), outUse(isLast(SNrC))}, gx, gy, &u, sizeof u);
  }

  const bool tileGlobals = !whole && j.glob;
  auto interiorWin = [&]() { return Rect{j.interior.x0 - j.work.x0, j.interior.y0 - j.work.y0, j.interior.x1 - j.work.x0, j.interior.y1 - j.work.y0}; };
  // dimensions of the 1/4 grid the dehaze / clarity analyses live on
  const int gridW = tileGlobals ? j.glob->lw : cdiv(W, 4), gridH = tileGlobals ? j.glob->lh : cdiv(Hh, 4);

  // ---- dehaze ----
  if (dz) {
    H dcH = -1;
    H gH = c.g.externalBuf(j.gBuf);
    if (P.dehaze > 0) {
      if (j.mode == Mode::AnalyzeDehaze) {
        // sweep 1: only collect the interior's low-resolution colour
        const Rect win = interiorWin();
        c.down(D, 4, 0, j.glob->color, win, j.interior.x0 / 4, j.interior.y0 / 4, cdiv(j.interior.w(), 4), cdiv(j.interior.h(), 4));
        return true;
      }
      if (whole) {
        const H col = c.tr(kF32, gridW, gridH);
        c.down(D, 4, 0, col, Rect{0, 0, W, Hh}, 0, 0, gridW, gridH);
        dcH = c.dehazeAnalysis(col, gridW, gridH, g.externalBuf(j.partBuf), gH);
        if (dcH < 0) return false;
      } else {
        dcH = j.glob->dc;
      }
    }
    Pipeline* p = c.pl("dehaze_apply", {{0, P.dehaze > 0 ? 0 : 1}, {10, isLast(SDz) ? 1 : 0}, {11, isLast(SDz) && grain ? 1 : 0}});
    if (!p) return false;
    DehU u{};
    u.dims[0] = W;
    u.dims[1] = Hh;
    u.dims[2] = ox;
    u.dims[3] = oy;
    u.low[0] = gridW;
    u.low[1] = gridH;
    u.low[2] = 4;
    u.dh[0] = 0.7f * P.dehaze;
    u.dh[1] = 0.5f * -P.dehaze;
    u.dh[2] = 0.88f;
    u.grain[0] = gAmp;
    u.grain[1] = gPer;
    g.dispatch("dehaze_apply", p, {Graph::RW(D), Graph::R(dcH >= 0 ? dcH : c.dummy(kR32)), Graph::R(gH), outUse(isLast(SDz))}, gx, gy, &u, sizeof u);
  }

  // ---- clarity / texture / sharpen ----
  if (detail || j.mode == Mode::AnalyzeClarity) {
    H abH = -1;
    if (clar) {
      if (j.mode == Mode::AnalyzeClarity) {
        c.down(D, 4, 1, j.glob->yy, interiorWin(), j.interior.x0 / 4, j.interior.y0 / 4, cdiv(j.interior.w(), 4), cdiv(j.interior.h(), 4));
        return true;
      }
      if (whole) {
        const H yy = c.tr(kF32, gridW, gridH);
        c.down(D, 4, 1, yy, Rect{0, 0, W, Hh}, 0, 0, gridW, gridH);
        abH = c.clarityAnalysis(yy, gridW, gridH);
        if (abH < 0) return false;
      } else {
        abH = j.glob->ab;
      }
    }
    if (j.mode != Mode::AnalyzeClarity) {
      H texH = -1, shH = -1;
      if (tex) {
        texH = c.blur(0, 3, D, W, Hh, {R.textureR, 0, 0, 0}, 2, 0, kR32);
        if (texH < 0) return false;
      }
      if (shp) {
        if (R.sharpBox > 0) {
          shH = c.blur(0, 3, D, W, Hh, {R.sharpBox, 0, 0, 0}, 3, 0, kR32);
        } else {
          // direct Gaussian: weights w[0..R] (symmetric, normalised) in the slot's table
          const int Rg = R.sharpGauss;
          float* w = static_cast<float*>(j.weights->mapped);
          double sum = 0;
          for (int i = -Rg; i <= Rg; ++i) sum += std::exp(-0.5 * double(i * i) / (double(R.sharpSigma) * R.sharpSigma));
          for (int i = 0; i <= Rg; ++i) w[i] = float(std::exp(-0.5 * double(i * i) / (double(R.sharpSigma) * R.sharpSigma)) / sum);
          shH = c.blur(0, 3, D, W, Hh, {Rg, 0, 0, 0}, 1, 1, kR32);
        }
        if (shH < 0) return false;
      }
      Pipeline* p = c.pl("detail", {{0, clar ? 1 : 0}, {1, tex ? 1 : 0}, {2, shp ? 1 : 0}, {10, isLast(SDetail) ? 1 : 0}, {11, isLast(SDetail) && grain ? 1 : 0}});
      if (!p) return false;
      DetU u{};
      u.dims[0] = W;
      u.dims[1] = Hh;
      u.dims[2] = ox;
      u.dims[3] = oy;
      u.low[0] = gridW;
      u.low[1] = gridH;
      u.low[2] = 4;
      u.gains[0] = P.clarity > 0.f ? P.clarity * 1.1f : P.clarity * 0.85f;
      u.gains[1] = P.texture > 0.f ? P.texture * 1.5f : P.texture * 0.9f;
      u.gains[2] = P.texture > 0.f ? 1.f : 0.f;
      u.gains[3] = R.sharpAmount;
      u.sharp[0] = 0.15f;
      u.sharp[1] = 0.0025f;
      u.grain[0] = gAmp;
      u.grain[1] = gPer;
      g.dispatch("detail", p,
                 {Graph::RW(D), Graph::R(texH >= 0 ? texH : c.dummy(kR32)), Graph::R(shH >= 0 ? shH : c.dummy(kR32)), Graph::R(abH >= 0 ? abH : c.dummy(kF32)), outUse(isLast(SDetail))},
                 gx, gy, &u, sizeof u);
    }
  }
  if (j.mode == Mode::AnalyzeDehaze || j.mode == Mode::AnalyzeClarity) return true;  // nothing to analyse for this job
  return true;
}

bool buildGlobalAnalysis(Engine& e, Graph& g, const FrameJob& j, bool dehaze, std::string& err) {
  Ctx c(e, g, j, err);
  const GlobalRes& G = *j.glob;
  if (dehaze) {
    const H gH = g.externalBuf(j.gBuf), pH = g.externalBuf(j.partBuf);
    const H dc = c.dehazeAnalysis(G.color, G.lw, G.lh, pH, gH);
    if (dc < 0) return false;
    g.copyImage(dc, G.dc, std::uint32_t(G.lw), std::uint32_t(G.lh));
  } else {
    const H ab = c.clarityAnalysis(G.yy, G.lw, G.lh);
    if (ab < 0) return false;
    g.copyImage(ab, G.ab, std::uint32_t(G.lw), std::uint32_t(G.lh));
  }
  return true;
}

}  // namespace dapple::gpu::vk
