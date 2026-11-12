#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/frame_plan.h"
#include "dapple/image.h"
#include "dapple/lut.h"
#include "dapple/tone.h"

namespace dapple {

// CPU reference implementation of the develop pipeline:
//
//   linear ProPhoto (scene) -> white balance -> exposure -> tone (log domain)
//   -> base shoulder curve -> OKLab colour (vibrance/sat/HSL/grading)
//   -> style/pad/grading -> sRGB gamut map -> sRGB encode -> curves -> LUT -> vignette
//   -> noise reduction -> dehaze -> clarity/texture/sharpen -> grain -> 8 bit
//   (geometry - rotate, straighten, crop, flip - is a resampling front end of the first stage)
//
// The GPU implementations for Android/iOS must match this output; it is the
// ground truth used by regression tests.
// ---- interactive (baked) path ----------------------------------------------------------------------
//
// All point-wise stages (white balance gain and exposure are applied outside; then tone, colour, style,
// grading, gamut map, sRGB encode, curves, LUT, fade) depend only on the parameters and on the pixel's
// own colour. The tone stage is a function of luminance (1D table, exact); everything after it is
// evaluated once per parameter change on an n^3 lattice of tone-mapped colours and the image is then
// pushed through it (tetrahedral interpolation), see renderer.cpp. It tracks the direct path within a
// fraction of a Delta E (tests/core_tests.cpp "baked path").
struct BakedLut {
  int n = 0;
  std::vector<float> rgb;   // n^3 * 3 encoded display values, index ((b * n + g) * n + r) * 3
  std::vector<float> toneMove;  // curve of EditParams::toneTarget (empty = none), for exact fallbacks
  std::vector<float> tone;  // luminance ratio of the tone stage over log2(Y) (see renderer.cpp)
  ToneAnalysis analysis;   // tonal analysis of the render with these point-wise parameters
  std::string key;         // what it was baked from (Renderer::pointKey + n)
  std::string colourKey;   // what `rgb` alone depends on (Renderer::colourKey): equal keys -> the lattice is reusable
};

// Process-wide counters of the bake pieces (tests, edit-bench): how many lattices were evaluated vs. reused and how
// many tone tables were built. Cheap relaxed atomics.
struct BakeCounters {
  std::atomic<unsigned long long> latticeEvaluated{0}, latticeReused{0}, toneTables{0};
};
BakeCounters& bakeCounters();

enum class SpatialQuality {
  Off,          // no neighbourhood stages
  Interactive,  // everything but grain; noise reduction only when `noise` is set
  Full,
};

// Which part of the (geometry-transformed) picture to render and at what size.
struct RenderRegion {
  float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;  // normalised, in the output space of the whole picture
  int outLongEdge = 0;                              // long edge of the region image (0 = natural scale)
  bool active() const { return outLongEdge > 0 || x0 > 0.f || y0 > 0.f || x1 < 1.f || y1 < 1.f; }
};

struct FastOptions {
  SpatialQuality spatial = SpatialQuality::Full;
  bool noise = true;  // Interactive only: also run noise reduction
  RenderRegion region;
  // Film (engine nodes): the interactive tier keeps the film grain while a grain-family slider is dragged (14 Q8); the
  // grain of a frame that previews a larger render is scale-aware against `fullLongEdge`; `frameIndex` seeds video grain.
  bool keepGrain = false;
  int fullLongEdge = 0;
  std::uint32_t frameIndex = 0;
  // Film, interactive frames only: a print table that is not cached yet is baked coarsely (17^3 instead of 33^3) while a table slider moves; the
  // frame after the drag bakes the exact one (docs/engine/pipeline-architecture.md section 26).
  bool coarseFilm = false;
};

class FrameKernels;  // engine block fragments of the per-pixel pipeline (src/frame_kernels.h)

class Renderer {
  friend class FrameKernels;
 public:
  // `lut` may be null. It must outlive the renderer.
  Renderer(const EditParams& params, const CameraProfile& camera, const Lut3D* lut);

  // Widens the limits of the tonal-class move (EditParams::toneTarget); > 1 for previews.
  void setToneReach(float reach) { toneReach_ = reach; }

  Image8 render(const SourceImage& source) const;

  // White balance x exposure gain of the Gain node (what the film folds into its input matrix; engine SessionCtx::gainFn).
  static void inputGain(const EditParams& params, const CameraProfile& camera, bool sceneReferred, float out[3]);
  static void whiteBalanceGain(const EditParams& clamped, float out[3]);

  // Optional tiny (<= 256 px) copy of the source used for the tonal analysis; saves re-scaling the
  // whole source on every call. Must outlive the renderer.
  void setToneProxy(const SourceImage* proxy) { toneProxy_ = proxy; }

  // Everything the lattice depends on (point-wise parameters, camera, LUT identity), without n.
  std::string pointKey() const;

  // True when EditParams::filmStock names a stock of the installed film library (film.h defaultLibrary()), i.e.
  // the film simulation replaces tone + colour + look. Otherwise the stock is ignored.
  bool filmActive() const;

  // What the colour lattice (BakedLut::rgb) depends on: pointKey() without the tone-stage and white-balance
  // parameters (exposure, contrast, highlights, shadows, whites, blacks, tonal-class move, temperature, tint),
  // which act through the exact 1D tone table and the input gain only.
  std::string colourKey() const;

  // Bakes the point-wise stages (see BakedLut). `n` 17..65. When `colourFrom` has this renderer's colourKey(), its
  // lattice (and its n) is reused and only the tone table and the analysis are rebuilt: a tone / white-balance drag
  // costs no colour evaluations, whatever the lattice size.
  // `fastAnalysis`: the tonal analysis is taken through the lattice instead of the exact pipeline (interactive
  // frames; the final frame always analyses exactly).
  std::shared_ptr<const BakedLut> bake(const SourceImage& source, int n = 33, const BakedLut* colourFrom = nullptr,
                                       bool fastAnalysis = false) const;

  // ---- the steps of bake() as separate pieces (the engine's table nodes, core/engine). bake() performs the same steps
  // in the same order; the pieces exist so that a result cache can reuse each one on its own key.
  // True when a film at full strength replaces the normal render: the colour lattice is then a 3-float placeholder.
  bool latticeFilmOnly() const;
  // The n^3 colour lattice (n clamped 17..65; 3 floats when latticeFilmOnly()). Does not depend on the tone stage.
  std::vector<float> bakeColourLattice(const SourceImage& source, int n) const;
  // Tonal analysis and tonal-class move. `fast` takes the analysis through `*rgb` (the lattice of size n; moved into a
  // scratch BakedLut and back, so it is unchanged on return); otherwise through the exact pipeline, and only when the
  // move is on (toneStrength > 0) -- `has` is false when nothing was analysed.
  struct TonePre {
    ToneAnalysis analysis;
    std::vector<float> toneMove;
    bool has = false;
  };
  TonePre bakeTonePre(const SourceImage& source, std::vector<float>* rgb, int n, bool fast) const;
  // Fills out.tone and out.toneMove (the 1D luminance table of the tone stage).
  void bakeToneTable(const SourceImage& source, const TonePre& pre, BakedLut& out) const;
  // The tonal analysis of the render: pre.analysis when available, else the exact one.
  ToneAnalysis bakeAnalysis(const SourceImage& source, const TonePre& pre) const;

  // Read-only description of what renderBakedRgba() would do for a source of srcW x srcH (GPU backend, tests).
  FramePlan framePlan(int srcW, int srcH, bool sceneReferred, const FastOptions& opt) const;
  // Second, coarser lattice over [0, maxV] (same layout as BakedLut::rgb, n^3 * 3) for tone-mapped colours that lie
  // beyond the range of BakedLut; `base` supplies the tone analysis so nothing is re-analysed. Empty when unsupported.
  std::vector<float> bakeOverflow(const SourceImage& source, const BakedLut& base, int n, float maxV) const;

  // Output size of render*() for `source` (geometry / region applied).
  void fastSize(const SourceImage& source, const RenderRegion& region, int& w, int& h) const;

  // Renders through a lattice from bake(). Returns a malloc()ed RGBA8 buffer (alpha 255) of
  // fastSize() pixels; the caller free()s it. Null on failure.
  std::uint8_t* renderBakedRgba(const SourceImage& source, const BakedLut& lut, const FastOptions& opt, int& w,
                                int& h) const;
  // Same, float sRGB-encoded 0..1 (tests, analysis).
  ImageF renderBakedFloat(const SourceImage& source, const BakedLut& lut, const FastOptions& opt) const;

  // Same as render() but keeps float sRGB-encoded values (0..1), for tests. The output size
  // follows the geometry (crop / rotate / straighten). `spatial` = false skips the neighbourhood
  // stages (noise reduction, dehaze, clarity, texture, sharpening, grain), used by the tone analysis.
  ImageF renderFloat(const SourceImage& source, bool spatial = true) const;

  // Per-channel linear gain the pipeline applies to the source before tone mapping / the film (white balance x develop
  // exposure x the camera's baseline exposure), for scene statistics.
  void sceneGain(const SourceImage& source, float gain[3]) const;

 private:
  struct Curve1D {
    std::vector<float> table;  // samples over [0,1]
    bool identity = true;
    float operator()(float x) const;
  };

  EditParams p_;
  const CameraProfile& camera_;
  const Lut3D* lut_;
  Curve1D master_, red_, green_, blue_;
  float wbGain_[3]{1, 1, 1};
  float toneReach_ = 1.f;
  const SourceImage* toneProxy_ = nullptr;

  struct PointOp;
  PointOp makePointOp(const SourceImage& source, const BakedLut* reuse) const;
  void displayStages(float e3[3]) const;  // curves + LUT on an encoded display value (film path)
  std::uint8_t* renderBakedImpl(const SourceImage& source, const BakedLut& lut, const FastOptions& opt, int& w, int& h,
                                ImageF* outFloat) const;
};

}  // namespace dapple
