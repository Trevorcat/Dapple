#pragma once
// Spatial and stochastic effects of the film pipeline (functional spec docs/film/spec/03-spatial-effects.md):
// highlight boost, diffusion filters, lens blur, in-emulsion scatter, halation, DIR inhibitor diffusion,
// grain, glare, scanner blur and unsharp mask. Architecture notes: docs/film/architecture.md section 2.
//
// Planar float32 planes for everything spatial; the point-wise stages keep using the baked-table kernels of
// film_kernels_impl.inc. All blurs are separable FIR Gaussians; Gaussians wider than ~3 px run on a box-filtered
// half-resolution pyramid (see applyMixture), so the cost per pixel is O(1) in the filter radius.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "film_internal.h"
#include "film_kernels.h"

namespace dapple::film::detail {

// ---- parameter-derived plan ------------------------------------------------------------------------------

struct GaussComp {
  double sigma;  // pixels at full resolution
  double w;
};
// out = w0 * in + sum_k w_k * G_{sigma_k} * in   (per channel)
struct Mixture {
  double w0 = 1.0;
  std::vector<GaussComp> comps;
  bool identity() const { return comps.empty() && w0 == 1.0; }
};
Mixture mixIdentity();
Mixture mixGauss(double sigma);                           // pure Gaussian
Mixture mixExpTail(double l);                             // 2D exponential surrogate: 3 Gaussians (spec 03 s3.2)
Mixture mixConvolve(const Mixture& a, const Mixture& b);  // a * b (variances add)
Mixture mixAffine(const Mixture& a, double wIdentity, double wMix);  // wIdentity * delta + wMix * a

struct GrainChannelPlan {
  bool active = false;
  int tableN = 0;                 // inverse table over D in [0, dTop]
  float invScale = 0;             // index = D * invScale
  std::vector<float> inv;         // (tableN + 2) * 4: sub-layer curve values (raw) for l = 0..2
  float dminL[3] = {}, invDmaxL[3] = {}, nL[3] = {}, scaleL[3] = {};  // per sub-layer
  float u = 0, dminCh = 0;
  int layers = 3;                 // 3 sub-layers, or 1 for the single-layer path
  std::vector<float> merged;       // (tableN + 2) * 4: mean, sqrt(variance), skewness, expected particle count of the sum of the layers
  float lambdaSwitch = 4.f;       // merged sampler above this expected particle count
  double dyeSigma[3] = {};        // px
};
struct GrainPlan {
  bool active = false;
  bool microActive = false;
  double microBlur = 0, microStd = 0;
  double blurSigma = 0;
  GrainChannelPlan ch[3];
};

struct SpatialPlan {
  int W = 0, H = 0;
  double pixelUm = 0;
  std::uint32_t seed = 0;
  // exposure stage
  bool boost = false;
  double boostM = 0, boostA = 0, boostP = 0;
  bool preMix = false, haloMix = false;
  Mixture pre[3];   // camera diffusion filter + lens blur
  Mixture halo[3];  // scatter + halation
  // development
  bool dirDiffuse = false;
  Mixture dir[3];
  GrainPlan grain;
  // print / scan
  bool enlargerDiffusion = false;
  Mixture enl[3];
  bool glare = false;
  double glareMu = 0, glareSigma = 0, glareBlur = 0, glareMean = 0;
  float glareRgb[3] = {1, 1, 1};
  double scanBlur = 0, unsharpSigma = 0, unsharpAmount = 0;
  bool scanStage() const { return glare || scanBlur > 0 || (unsharpSigma > 0 && unsharpAmount > 0); }
  bool any() const {
    return boost || preMix || haloMix || dirDiffuse || grain.active || enlargerDiffusion || glare || scanStage();
  }
};

// Diffusion-filter mixtures (camera / enlarger), per channel. Returns false when the filter has no effect.
bool buildDiffusionMixture(const DiffusionFilterParams& f, double pixelUm, Mixture out[3]);

// Builds everything that depends on the image size (pixel size) and the spatial parameters. `film` supplies the
// density curves for the grain model. `printPath`: true when a print is made (glare applies).
//
// Scale-aware grain (docs/engine/pipeline-architecture.md 14.1 Q8). `full` describes the full-resolution render this frame is
// a preview of: its pixel pitch (`pixelUm`, > 0) and the scanner stage it applies to the grain (lens blur, unsharp mask: the
// preview skips them, but they shape the grain the final shows). When this frame's pitch is coarser (k = pitch / pixelUm > 1)
// the grain is planned so that it is statistically what an area-average (box) downscale by k of the full-resolution grain
// would be: the pixel-unit grain blur is divided by k and the particle count is divided by the variance factor of
// box-averaging the full-resolution grain after its blur and scanner stage (grainBoxFactor). nullptr or k <= 1: the plan is
// exactly the one of the frame itself.
struct GrainFullRes {
  double pixelUm = 0;
  double scanBlur = 0, unsharpSigma = 0, unsharpAmount = 0;
  double glareBlur = 0;  // the stochastic print glare is averaged the same way
};
SpatialPlan makeSpatialPlan(const FilmStock& film, const FilmParams& digested, int W, int H, bool printPath,
                            const FilmTables& tables, std::string* err, const GrainFullRes* full = nullptr);

// Variance of the area-average (box of k full-resolution pixels per axis, phase 0) of white noise passed through
// grain blur sigma, scanner lens blur and unsharp mask (sigma, amount), relative to the same box average of the
// unblurred noise (1 for no filtering). 2-D: square of the 1-D factor. Discrete taps as the renderer uses them.
double grainBoxFactor(double k, double grainBlur, double scanBlur, double unsharpSigma, double unsharpAmount);

// ---- kernels (compiled per instruction set, film_spatial_kernels.inc) ----------------------------------------

struct SpatialKernels {
  const char* name;
  // dst[i] = sum_{k=-r..r} w[|k|] src[i + k]
  void (*firRow)(const float* src, float* dst, int n, const float* w, int r);
  // dst[i] (+)= sum_k w[|k|] mid[i + k * stride]
  void (*firCol)(const float* mid, std::ptrdiff_t stride, float* dst, int n, const float* w, int r, int accumulate);
  void (*scaleRow)(const float* src, float* dst, int n, float w);               // dst = w * src
  void (*down2Row)(const float* r0, const float* r1, float* dst, int nOut, int nIn);
  void (*up2Row)(const float* tmp, float* dst, int nLow, int nHi);              // tmp padded by 2; dst += cubic x2
  void (*lerpRows)(const float* a, const float* b, const float* c, const float* d, const float* w4, float* dst, int n);
  // Grain for one channel row: sub-layer fields g0..g2 (sumOnly: only g0 = sum of the three).
  void (*grainRow)(const GrainChannelPlan& gc, const float* d, float* g0, float* g1, float* g2, int n, int x0, int y,
                   std::uint32_t streamBase, int sumOnly);
  // Log-normal field exp(mu + sigma z), z ~ N(0,1) per pixel.
  void (*logNormalRow)(float* dst, int n, int x0, int y, std::uint32_t stream, float mu, float sigma);
  // p[i] = 2^(k p[i]) in place, polynomial exp2 (max rel. error 3.3e-6, |k p| clamped to 125).
  void (*exp2Scale)(float* p, int n, float k);
  // Highlight boost of one row (see boostConstants); polynomial exp.
  void (*boostRow)(float* r, int n, float fx0, float fX, float fa, float fk);
};

extern const SpatialKernels sBase;
#if defined(DAPPLE_FILM_HAVE_AVX2)
extern const SpatialKernels sAvx2;
#endif
const SpatialKernels& selectSpatialKernels();

// ---- planes and the Gaussian mixture engine ---------------------------------------------------------------

struct Plane {
  float* p = nullptr;
  int w = 0, h = 0;
  std::ptrdiff_t stride = 0;
  float* row(int y) const { return p + std::ptrdiff_t(y) * stride; }
};

// Bump allocator whose chunks persist between renders (first touch is the only page-fault cost).
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena();
  float* alloc(std::size_t floats);
  Plane plane(int w, int h);
  void reset();
  struct Mark {
    std::size_t cur, used;
  };
  Mark mark() const { return chunks_.empty() ? Mark{0, 0} : Mark{cur_ < chunks_.size() ? cur_ : chunks_.size() - 1, chunks_[cur_ < chunks_.size() ? cur_ : chunks_.size() - 1].used}; }
  void release(Mark m) {
    if (chunks_.empty()) return;
    for (std::size_t j = m.cur + 1; j < chunks_.size(); ++j) chunks_[j].used = 0;
    chunks_[m.cur].used = m.used;
    cur_ = m.cur;
  }
  std::size_t capacityBytes() const;
  std::size_t freeAll();  // returns the chunks to the system (memory trim); the arena stays usable

 private:
  struct Chunk {
    float* p;
    std::size_t cap, used;
  };
  std::vector<Chunk> chunks_;
  std::size_t cur_ = 0;
};

// dst = w0 * src + sum_k w_k G_{sigma_k} * src (reflect boundary), per plane. src and dst must differ.
void applyMixture(const SpatialKernels& K, Arena& arena, const Plane& src, const Plane& dst, const Mixture& mix);

// Same, for mixtures without an identity part whose components are all wide (sigma >= 3.2 px): the input is given already box-averaged
// to half resolution ((w+1)/2 x (h+1)/2) and `dst` (full resolution) receives the upsampled result. Uses a 1.6 px floor for the
// low-resolution sigma (smooth fields such as the DIR inhibitor).
bool mixtureFitsHalf(const Mixture& mix);
void applyMixtureHalf(const SpatialKernels& K, Arena& arena, const Plane& half, const Plane& dst, const Mixture& mix);

// ---- a mixture in row bands (streaming export, docs/engine/pipeline-architecture.md 42.6) ----------------------------------------
// applyMixture / applyMixtureHalf evaluated for ranges of output rows from ranges of input rows, bit-identical to the whole-plane
// functions: the pyramid levels >= 2 (a sixteenth of the pixels and less) are built once from one streamed pass over the input (feed,
// rows in order) and kept (only the level-2 accumulator remains); the full- and half-resolution levels are evaluated per band.
class MixtureBands {
 public:
  MixtureBands();
  ~MixtureBands();
  MixtureBands(MixtureBands&&) noexcept;
  MixtureBands& operator=(MixtureBands&&) noexcept;
  // W x H = the output (full-resolution) plane; half = the input is the half-resolution plane ((W+1)/2 x (H+1)/2, applyMixtureHalf).
  void setup(const Mixture& mix, int W, int H, bool half);
  int inputW() const;
  int inputH() const;
  bool needsPrepare() const;  // the pyramid reaches level 2: feed the whole input once before rows()
  bool prepared() const;
  // the streamed pass: input rows [y0, y1) in order (y0 = the rows fed so far), rowAt(y) valid for y in [y0, y1) during the call
  void feed(const SpatialKernels& K, int y0, int y1, const std::function<const float*(int)>& rowAt);
  // input rows output rows [a, b) read
  void inputRange(int a, int b, int& ia, int& ib) const;
  // output rows [a, b): rowAt(y) for the input rows of inputRange, outRow(y) the destination rows
  void rows(const SpatialKernels& K, int a, int b, const std::function<const float*(int)>& rowAt, const std::function<float*(int)>& outRow) const;
  std::size_t wholeBytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_;
};

// Single separable Gaussian on one plane (tests, small filters). dst may not alias src.
void gaussianBlurPlane(const SpatialKernels& K, const Plane& src, const Plane& dst, double sigma);

// Half-kernel taps of the reference Gaussian: sigma < 3: truncated FIR, r = floor(3 sigma + 0.5); otherwise r = ceil(4 sigma).
// w[0..r], normalised so that w[0] + 2 sum w[k] = 1. r == 0 means identity.
void gaussTaps(double sigma, std::vector<float>& w, int& r);

// Highlight boost constants (spec 03 s4.1). Returns false when the parameters are invalid.
bool boostConstants(double M, double A, double P, double X, double& x0, double& a, double& k);

struct SpatialTimings {
  double exposureMs = 0, preMs = 0, haloMs = 0, dirMs = 0, finalMs = 0, totalMs = 0;
};

// The whole spatial render. `in`: linear ProPhoto, `out`: display sRGB-encoded.
struct SpatialRenderArgs {
  const KView* view;
  const FilmTables* tables;
  const KernelSet* ks;
  const SpatialKernels* sk;
  const SpatialPlan* plan;
  Arena* arena;
  float scale;  // 2^EV (user + auto)
  int tileW = 0, tileH = 0;  // tile size hint in pixels (0 = default 256 x 64); the result does not depend on it
};
void renderSpatial(const SpatialRenderArgs& a, const ImageF& in, ImageF& out, SpatialTimings* t);

}  // namespace dapple::film::detail
