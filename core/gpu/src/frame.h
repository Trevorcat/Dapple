#pragma once
// Internal: turns a FramePlan (what the CPU reference would do) into a pass graph. Point-wise stages are fused into one
// kernel (geometry + gain + tone + lattice + vignette); each spatial stage is a short chain of passes; the last pass
// of the chain also adds the grain and writes the final RGBA8. See docs/engine/gpu-backend.md, "pass graph".
#include <string>

#include "dapple/frame_plan.h"
#include "dapple/gpu.h"
#include "vk_core.h"

namespace dapple::gpu::vk {

struct Rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};

struct SourceBind {  // the image the point pass samples, and where it sits in the (resized) source space
  H img = -1;
  int ox = 0, oy = 0;
};

struct LutBind {
  H tone = -1, lat = -1, lat2 = -1;
  int n = 0, n2 = 0, toneN = 0, toneBase = 0;
  float max1 = 1.5f, max2 = 8.f;
};

// Whole-picture analysis shared by all tiles of an export (dehaze airlight / dark channel, clarity base), on a 1/4 grid.
struct GlobalRes {
  H color = -1;  // RGBA32F low-res colour (dehaze analysis input)
  H dc = -1;     // R32F dark channel (dehaze)
  H yy = -1;     // RGBA32F (Y, Y^2) (clarity analysis input)
  H ab = -1;     // RGBA32F guided filter coefficients (clarity)
  int lw = 0, lh = 0;
};

enum class Mode {
  Whole,         // one frame: analysis from the frame itself, writes RGBA8
  AnalyzeDehaze, // tile sweep 1: point + noise reduction, then average the interior into GlobalRes::color
  AnalyzeClarity,// tile sweep 2: ... + dehaze, then average (Y, Y^2) into GlobalRes::yy
  Final,         // tile sweep 3: everything, RGBA8
};

struct FrameJob {
  const FramePlan* plan = nullptr;
  float resizeSharpen = 0.f;
  Mode mode = Mode::Whole;
  Rect interior;  // pixels of the picture this job produces / analyses
  Rect work;      // interior + halo (clamped to the picture): the size of the working images
  int allocW = 0, allocH = 0;  // allocation size of the working images (a tiled export uses one size for all tiles)
  SourceBind src;
  LutBind lut;
  GlobalRes* glob = nullptr;  // required by Final when dehaze/clarity are on, and by the Analyze modes
  Buffer* gBuf = nullptr;     // 16 floats: dehaze statistics (written by the reduction passes)
  Buffer* partBuf = nullptr;  // partial sums of the reduction
  Buffer* weights = nullptr;  // host-visible filter weight table of the frame slot
};

// Radii of the spatial stages, shared by the builder and the tile halo computation.
struct SpatialRadii {
  int nrLumaR = 0;                 // box radius (2 iterations)
  int nrChromaF = 2, nrChromaRl = 0;
  int dehazeMin = 0, dehazeBox = 0;
  int clarityR = 0;
  int textureR = 0;
  float sharpSigma = 0;
  int sharpBox = 0, sharpGauss = 0;  // box radius (3 iterations) or direct kernel half width
  float sharpAmount = 0;
  int halo = 0;                    // pixels around a tile needed by the local stages, multiple of 4
};
SpatialRadii spatialRadii(const FramePlan& p, float resizeSharpen);

// Appends the passes of one job. `out8` receives the handle of the RGBA8 image (Whole / Final).
bool buildFrame(Engine& e, Graph& g, const FrameJob& j, H* out8, std::string& err);

// Tiled export: after sweep 1 (dehaze = true: GlobalRes::color -> dc + airlight) / sweep 2 (false: yy -> ab).
bool buildGlobalAnalysis(Engine& e, Graph& g, const FrameJob& j, bool dehaze, std::string& err);

}  // namespace dapple::gpu::vk
