#pragma once
// The point family (docs/engine/pipeline-architecture.md section 50): everything of the develop pipeline that acts per pixel, resolved from
// one EditParams, as plain data and free functions. The pipeline it implements:
//
//   linear ProPhoto (scene) -> white balance -> exposure -> [single-image HDR] -> tone (log domain)
//   -> base shoulder curve -> OKLab colour (vibrance/sat/HSL/grading)
//   -> style/pad/grading -> sRGB gamut map -> sRGB encode -> curves -> LUT
//   (geometry - rotate, straighten, crop, flip, region, reshaping warp - is the resampling front end of the first stage)
//
// What resolves what:
//   point::resolve(EditParams, CameraProfile, Lut3D)  -> Params   the clamped edit, curves, white-balance gain, camera, LUT
//   point::resolveFrame(Params, source size, ...)     -> Blocks   geometry, warp, gain, look grain, radii scale, film glue, display (blocks.h)
//   point::hdrBind(Params, source, tone table)        -> Hdr      the single-image HDR block
//   point::bake* (the table nodes)                    -> colour lattice, tone analysis, tone table, analysis, overflow lattice (BakedLut)
//   point::facts(Blocks, FastOptions, scene)          -> FrameFacts the other families resolve from
// The CPU nodes of the family (Resize/Source head, Geometry, Gain, Hdr, ToneColour, the fused head, DisplayStages, Blend) bind a frame
// through point::CpuFrame (nodes/point/cpu.h); the GPU records from Blocks (nodes/point/gpu.cpp, exec/gpu_executor.cpp). Per-pixel kernels:
// nodes/point/kernels.h.
//
// Interactive (baked) path: the tone stage is a function of luminance (1D table, exact); everything after it is evaluated once per
// parameter change on an n^3 lattice of tone-mapped colours and the image is pushed through it (tetrahedral interpolation). It tracks the
// exact path within a fraction of a Delta E (tests/core_tests.cpp "baked path").
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"
#include "dapple/lut.h"
#include "dapple/tone.h"
#include "engine/graph/node.h"
#include "engine/nodes/point/blocks.h"
#include "engine/nodes/point/frame_options.h"

namespace dapple::engine {
struct NodeDesc;
struct BuildCtx;
}  // namespace dapple::engine

namespace dapple {

// The baked point stages of one parameter set (the HostTable payloads the executors read).
struct BakedLut {
  int n = 0;
  std::vector<float> rgb;       // n^3 * 3 encoded display values, index ((b * n + g) * n + r) * 3
  std::vector<float> toneMove;  // curve of EditParams::toneTarget (empty = none), for exact fallbacks
  std::vector<float> tone;      // luminance ratio of the tone stage over log2(Y) (kernels.h, LutSampler)
  ToneAnalysis analysis;        // tonal analysis of the render with these point-wise parameters
  std::string key;              // what it was baked from (point::pointKey + n)
  std::string colourKey;        // what `rgb` alone depends on (point::colourKey): equal keys -> the lattice is reusable
};

// Process-wide counters of the bake pieces (tests, edit-bench): how many lattices were evaluated vs. reused and how
// many tone tables were built. Cheap relaxed atomics.
struct BakeCounters {
  std::atomic<unsigned long long> latticeEvaluated{0}, latticeReused{0}, toneTables{0};
};
BakeCounters& bakeCounters();

}  // namespace dapple

namespace dapple::engine::point {

constexpr int kCurveSamples = 1024;

// A tone curve sampled over [0, 1] (master / red / green / blue curves, the tonal-class move).
struct Curve1D {
  std::vector<float> table;  // kCurveSamples samples over [0,1]
  bool identity = true;
  float operator()(float x) const {
    if (identity) return x;
    const float f = std::clamp(x, 0.f, 1.f) * (kCurveSamples - 1);
    const int i = std::min(int(f), kCurveSamples - 2);
    const float t = f - i;
    return table[i] + t * (table[i + 1] - table[i]);
  }
};

struct Curves {
  Curve1D master, red, green, blue;
};

// The point family's parameters of one edit: the clamped EditParams (a film frame never renders in P3), the display curves, the
// white-balance gain, and what the bakes read besides the edit. `camera` and `lut` (may be null) must outlive it, as must the proxies.
struct Params {
  EditParams p;
  const CameraProfile* camera = nullptr;
  const Lut3D* lut = nullptr;
  Curves curves;
  float wb[3] = {1.f, 1.f, 1.f};      // white-balance gain (whiteBalanceGain)
  float toneReach = 1.f;              // widens the limits of the tonal-class move (EditParams::toneTarget); > 1 for previews
  // Optional tiny (<= 256 px) copy of the source used for the tonal analysis (saves re-scaling the whole source on every bake).
  const SourceImage* toneProxy = nullptr;
  const SourceImage* modernProxy = nullptr;  // the scene end points of the modern rendering (= toneProxy unless setAnalysisSources)
  const SourceImage* hdrProxy = nullptr;     // streaming export: the 512 px copy the HDR field is built from
};

Params resolve(const EditParams& params, const CameraProfile& camera, const Lut3D* lut);

// The tone proxy (also the modern rendering's end-point source).
inline void setToneProxy(Params& r, const SourceImage* proxy) { r.toneProxy = r.modernProxy = proxy; }
// Streaming export (section 42): the source handed to the render carries the size only, its picture-wide analyses read these instead,
// each the exact input the analysis would derive from the full picture: `tone` = resizeToLongEdge(full, 256) (tonal analysis), `modern`
// = the stride samples of the modern rendering's end points laid out as an image (modernGridSamples), `hdr` = resizeToLongEdge(full, 512)
// (single-image HDR field). Null keeps the default.
inline void setAnalysisSources(Params& r, const SourceImage* tone, const SourceImage* modern, const SourceImage* hdr) {
  r.toneProxy = tone;
  r.modernProxy = modern;
  r.hdrProxy = hdr;
}

// ---- gains ----------------------------------------------------------------------------------------------------------------------
// White balance x exposure gain of the Gain node (what the film folds into its input matrix; engine SessionCtx::gainFn).
void inputGain(const EditParams& params, const CameraProfile& camera, bool sceneReferred, float out[3]);
void whiteBalanceGain(const EditParams& clamped, float out[3]);
// Per-channel linear gain the pipeline applies to the source before tone mapping / the film (white balance x develop exposure x the
// camera's baseline exposure), for scene statistics.
void sceneGain(const Params& r, const SourceImage& source, float gain[3]);

// ---- film glue ------------------------------------------------------------------------------------------------------------------
// True when EditParams::filmStock names a stock of the installed film library (film.h defaultLibrary()), i.e. the film simulation
// replaces tone + colour + look. Otherwise the stock is ignored.
bool filmActive(const EditParams& p);
// True when a film at full strength replaces the normal render: the colour lattice is then a 3-float placeholder.
bool latticeFilmOnly(const Params& r);

// ---- keys -----------------------------------------------------------------------------------------------------------------------
// Everything the lattice depends on (point-wise parameters, camera, LUT identity), without n.
std::string pointKey(const Params& r);
// What the colour lattice (BakedLut::rgb) depends on: pointKey without the tone-stage and white-balance parameters (exposure, contrast,
// highlights, shadows, whites, blacks, tonal-class move, temperature, tint), which act through the exact 1D tone table and the gain only.
std::string colourKey(const Params& r);

// ---- table bakes (the engine's table nodes: ColourLattice, ToneAnalysisPre, ToneTable, Analysis, LatticeOverflow) -------------------
// All of them at once (n 17..65). When `colourFrom` has r's colourKey, its lattice (and its n) is reused and only the tone table and the
// analysis are rebuilt. `fastAnalysis`: the tonal analysis is taken through the lattice instead of the exact pipeline (interactive frames).
std::shared_ptr<const BakedLut> bake(const Params& r, const SourceImage& source, int n = 33, const BakedLut* colourFrom = nullptr,
                                     bool fastAnalysis = false);
// The n^3 colour lattice (n clamped 17..65; 3 floats when latticeFilmOnly). Does not depend on the tone stage.
std::vector<float> bakeColourLattice(const Params& r, const SourceImage& source, int n);
// Tonal analysis and tonal-class move. `fast` takes the analysis through `*rgb` (the lattice of size n; moved into a scratch BakedLut and
// back, so it is unchanged on return); otherwise through the exact pipeline, and only when the move is on -- `has` is false when nothing
// was analysed.
struct TonePre {
  ToneAnalysis analysis;
  std::vector<float> toneMove;
  bool has = false;
};
TonePre bakeTonePre(const Params& r, const SourceImage& source, std::vector<float>* rgb, int n, bool fast);
// Fills out.tone and out.toneMove (the 1D luminance table of the tone stage).
void bakeToneTable(const Params& r, const SourceImage& source, const TonePre& pre, BakedLut& out);
// The tonal analysis of the render: pre.analysis when available, else the exact one.
ToneAnalysis bakeAnalysis(const Params& r, const SourceImage& source, const TonePre& pre);
// Second, coarser lattice over [0, maxV] (same layout as BakedLut::rgb) for tone-mapped colours beyond the range of BakedLut; `base`
// supplies the tone analysis so nothing is re-analysed. Empty when unsupported.
std::vector<float> bakeOverflow(const Params& r, const SourceImage& source, const BakedLut& base, int n, float maxV);

// ---- the table nodes (CpuImpl::bake; nodes/point/bake.cpp) --------------------------------------------------------------------------
// Payloads: ColourLattice / LatticeOverflow std::vector<float>, ToneAnalysisPre TonePre, ToneTable ToneTableOut, Analysis ToneAnalysis.
struct ToneTableOut {
  std::vector<float> tone, toneMove;
};
struct TableEnv {
  const Params& point;
  const SourceImage& src;
};
// The per-frame parameters of a table node's CpuImpl::bake (`resolved`). n = lattice size; fast / maxV as the node needs.
struct TableBakeCtx {
  const TableEnv* env = nullptr;
  int n = 0;
  bool fast = false;
  float maxV = 0.f;
};
const CpuImpl& latticeCpu();
const CpuImpl& tonePreCpu();
const CpuImpl& toneTableCpu();
const CpuImpl& analysisCpu();
const CpuImpl& overflowCpu();

// ---- the frame ----------------------------------------------------------------------------------------------------------------------
// The blocks of a frame of a srcW x srcH source (geometry, warp, gain, look grain, radii scale, film glue, display stages); the HDR block
// is bound separately (hdrBind, it needs the tone table). `src`: the source the frame binds (region-source radii scale, reshaping field;
// its pixels may be a resized copy of srcW x srcH); null: neither. `exactVariant`: the exact variant (no region; the look grain is settled
// after the film stage, CpuFrame::finalizeSpatial).
Blocks resolveFrame(const Params& r, int srcW, int srcH, bool sceneReferred, const FastOptions& opt, const SourceImage* src = nullptr,
                    bool exactVariant = false);
// EditParams::hdr resolved for this source (field built / cached, parameters): leaves `out` inactive when the strength is 0, the source is
// display-referred or a film stock is selected. `tone`: the frame's 1D tone table (BakedLut::tone); the range the curve shows well is
// derived from it (null: 8 EV).
void hdrBind(const Params& r, const SourceImage& source, Hdr& out, const std::vector<float>* tone = nullptr);
// NodeBehaviour of the Geometry node (resolved: Blocks; identity: no resampling; inputRect: sourceWindow) and of the Hdr node (identity: the
// HDR block is inactive). Neither has a halo.
const NodeBehaviour& geometryBehaviour();
const NodeBehaviour& hdrBehaviour();
// The Hdr node is wired when the edit asks for HDR on a scene-referred source without a film (whether it runs is the bound block's).
bool hdrWanted(const BuildCtx& c);

// ---- the family's nodes (nodes.cpp), registered in engine/nodes/node_list.inc ---------------------------------------------------------
NodeDesc sourceDesc();
NodeDesc geometryDesc();
NodeDesc gainDesc();
NodeDesc hdrDesc();
NodeDesc toneColourDesc();
NodeDesc toneColourExactDesc();
NodeDesc displayDesc();
NodeDesc blendDesc();
NodeDesc pack8Desc();
NodeDesc latticeDesc();
NodeDesc overflowDesc();
NodeDesc tonePreDesc();
NodeDesc toneTableDesc();
NodeDesc analysisDesc();
// The source pixels ([x0, x1) x [y0, y1), clamped to the source) the head reads for output rect `out`: the corner pixel centres through the
// geometry, plus the resampling support (and the reshaping field's reach).
Rect sourceWindow(const Blocks& b, const Rect& out);
// Output size of a frame of `source` (geometry / region applied).
void fastSize(const Params& r, const SourceImage& source, const RenderRegion& region, int& w, int& h);

// ---- the modern rendering (section 37) ------------------------------------------------------------------------------------------
// The post-gain scene luminance that a modern rendering (EditParams::filmStock = modern_*) shows as display white (1 = not modern or no
// stretch): the Ultra HDR rendition measures the highlight headroom against it.
float modernSdrWhite(const Params& r, const SourceImage& source);
// Pixel (x, y) of the full picture W x H that the modern rendering's end points sample at grid cell (i, j) (grid of modernGridSize()).
void modernGridSize(int W, int H, int& gw, int& gh);
void modernGridPixel(int W, int H, int i, int j, int& x, int& y);

}  // namespace dapple::engine::point
