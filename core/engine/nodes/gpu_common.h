#pragma once
// Shared pieces of the GPU node implementations (step b2): the recorder the node `record` functions append to, resolved
// parameter blocks derived from the CPU reference's FramePlan, and the separable filter helpers (box / weight table /
// minimum, with an O(1)-per-pixel running-sum box for larger radii).
//
// Each main node has one record function; they are registered in nodes/gpu_nodes.cpp (gpuImplFor) next to the shader names
// they use, and called in order by the frame builder (exec/gpu_frame.cpp) until the Planner hands out stages.
#include <array>
#include <cstdint>
#include <string>

#include "dapple/frame_plan.h"
#include "engine/exec/gpu_scheduler.h"
#include "engine/hal/hal.h"

namespace dapple::engine::gpu {

struct WRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};

constexpr Format kF16 = Format::RGBA16F;
constexpr Format kF32 = Format::RGBA32F;
constexpr Format kR32 = Format::R32F;
constexpr Format kU8 = Format::RGBA8;

inline int cdiv(int a, int b) { return (a + b - 1) / b; }

// Radii of the spatial stages, shared by the builder and the tile halo computation.
// Workgroup shape of the 2D kernels on a device: 16 x (inv/16), inv = min(256, maxComputeWorkGroupInvocations) as a power of two.
void workgroupShape(const hal::DeviceCaps& c, int& lx, int& ly, int& rwg);

// Radii of the spatial stages, shared by the builder and the tile halo computation.
struct SpatialRadii {
  int nrLumaR = 0;  // box radius (2 iterations)
  int nrChromaF = 2, nrChromaRl = 0;
  int dehazeMin = 0, dehazeBox = 0;
  int clarityR = 0;
  int textureR = 0;
  float sharpSigma = 0;
  int sharpBox = 0, sharpGauss = 0;  // box radius (3 iterations) or direct kernel half width
  float sharpAmount = 0;
  int halo = 0;  // pixels around a tile needed by the local stages, multiple of 4
};
SpatialRadii spatialRadii(const FramePlan& p, float resizeSharpen);
void grainArgs(const FramePlan& P, float& amp, float& period);

// The image the point pass samples, and where it sits in the (resized) source space.
struct SourceBind {
  H img = -1;
  int ox = 0, oy = 0;
  // portrait reshaping (section 45): the buffer that holds the field (the HDR buffer: field after the HDR grid) and its float offset
  H warp = -1;
  int warpOff = 0;
};
// Uniform block tail of the shaders that include warp.glsl
struct WarpU {
  std::int32_t wI[4];
  float wB[4], wC[4];
};
void warpUniform(const FramePlan& P, const SourceBind& s, WarpU& u);
struct LutBind {
  H tone = -1, lat = -1, lat2 = -1;
  H hdr = -1;  // single-image HDR field (SSBO), -1 = none
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

// Where a node's pass ends: every kernel that can end the pipeline adds the grain and writes the final RGBA8 instead of the
// RGBA16F working image (the Grain + Pack8 epilogue slot of the recipes, pipeline-architecture.md 5.3).
struct Epilogue {
  bool last = false, grain = false;
  float amp = 0, period = 1;
  H out8 = -1;  // the RGBA8 image when `last`
};

// What a record function appends to. One per job (frame or tile).
struct GpuRecorder {
  hal::Device& dev;
  GpuGraph& g;
  std::string& err;
  hal::BufferH weights;       // host-visible filter weight table of the frame slot
  hal::BufferH taps;          // host-visible tap tables of the film mixtures (vec4 units: tapNext)
  std::uint32_t tapNext = 0;
  hal::BufferH gBuf, partBuf; // dehaze statistics + reduction partials
  int W = 0, H_ = 0;          // size of the working images of this job
  int allocW = 0, allocH = 0; // allocation size of the working images (a tiled export uses one size for all tiles)
  int boxScanMin = -1;        // radius from which the running-sum box is used (-1: DAPPLE_GPU_BOX_SCAN_MIN or 12; 0: never)

  // Workgroup shape of the 2D kernels (spec constants 16 / 17) and of the reduction (spec constant 18). 16 x 16 and 256 where the
  // device allows 256 invocations; GLES 3.1 only guarantees 128 (16 x 8, reduce 128), docs/engine/gles-shader-patches.md.
  int lx = 16, ly = 16, rwg = 256;
  std::uint32_t gx(int w) const { return std::uint32_t(cdiv(w, lx)); }
  std::uint32_t gy(int h) const { return std::uint32_t(cdiv(h, ly)); }

  GpuRecorder(hal::Device& d, GpuGraph& gr, std::string& e);

  // Adds the workgroup spec constants of the kernels that have them (see lx / ly / rwg).
  hal::PipelineH pl(const char* shader, std::initializer_list<hal::SpecConst> spec);
  H dummy(Format f);
  H weightsH();
  // working image with the job's allocation size when it is the work size (or its 1/2, 1/4 grids)
  H tr(Format f, int w, int h);

  // One direction of a separable filter. kind: 0 rgba16f -> luma r32f, 1 rgba32f -> min r32f, 2 rgba32f, 3 r32f.
  // op: 0 box, 1 weight table, 2 minimum.
  H sep(int kind, H in, int w, int h, int dir, int op, const std::array<int, 4>& rad, Format outFmt);
  // iters x (horizontal, vertical). `first` = kind of the first pass, `work` = kind of the following ones.
  H blur(int first, int work, H in, int w, int h, const std::array<int, 4>& rad, int iters, int op, Format outFmt);
  // Area average by an integer factor of a window of D into a (larger) low-resolution grid.
  H down(H D, int f, int mode, H dst, const WRect& win, int offX, int offY, int cntX, int cntY);

 private:
  H d8_ = -1, d16_ = -1, d32_ = -1, dR_ = -1, wbuf_ = -1;
};

// ---- node record functions (one per node, nodes/<node>/gpu.cpp) --------------------------------------------------------
struct PointInputs {
  H src, tone, lat, lat2, D, out8;
};
struct PointNode {
  const FramePlan* plan;
  SourceBind src;
  LutBind lut;
  int W, Hh, ox, oy;  // working image size and its origin in the picture
  Epilogue ep;
  // local adjustments (docs section 40): descriptor and plane store SSBOs (point_local when >= 0), the stage-W amount images (or -1)
  H ldesc = -1, lplanes = -1, aux0 = -1, aux1 = -1;
};
bool recordPoint(GpuRecorder& r, const PointNode& n, H D);

struct NrLumaNode { const FramePlan* plan; const SpatialRadii* R; int W, Hh, ox, oy; Epilogue ep; };
bool recordNrLuma(GpuRecorder& r, const NrLumaNode& n, H D);
struct NrChromaNode { const FramePlan* plan; const SpatialRadii* R; int W, Hh, ox, oy; Epilogue ep; };
bool recordNrChroma(GpuRecorder& r, const NrChromaNode& n, H D);

// dehaze analysis of the low-resolution colour grid: dark channel + airlight statistics (into gBuf). Returns the dark channel.
H recordDehazeAnalysis(GpuRecorder& r, const SpatialRadii& R, H col, int lw, int lh);
struct DehazeNode { const FramePlan* plan; int W, Hh, ox, oy, gridW, gridH; H dc; Epilogue ep; };
bool recordDehazeApply(GpuRecorder& r, const DehazeNode& n, H D);

// guided filter coefficients (a, b) of the clarity base from the (Y, Y^2) grid
H recordClarityAnalysis(GpuRecorder& r, const SpatialRadii& R, H yy, int lw, int lh);
struct DetailNode {
  const FramePlan* plan;
  const SpatialRadii* R;
  int W, Hh, ox, oy, gridW, gridH;
  H ab;  // clarity coefficients (or -1)
  Epilogue ep;
};
bool recordDetail(GpuRecorder& r, const DetailNode& n, H D);

// LocalDetail (local adjustments, stage W): returns the new working image (D itself when it is the last pass), -1 on error
struct LocalDetailNode {
  const FramePlan* plan;
  int W, Hh, ox, oy;
  H aux0, aux1;  // the amounts the point pass wrote
  Epilogue ep;
};
H recordLocalDetail(GpuRecorder& r, const LocalDetailNode& n, H D);
// The scene input of a film frame with the local adjustments applied (stage L; geometry gather included), RGBA32F `out` of n.W x n.Hh at
// (n.ox, n.oy); writes the stage-W amounts into aux0 / aux1 when both are >= 0.
struct LocalFilmInNode {
  const FramePlan* plan;
  SourceBind src;
  int W, Hh, ox, oy;
  H ldesc, lplanes, aux0, aux1;
};
bool recordLocalFilmIn(GpuRecorder& r, const LocalFilmInNode& n, H out);

// Lanczos-3 resize planning and passes
struct SrcWin {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};
SrcWin lanczosWindow(int srcW, int srcH, int dstW, int dstH, const WRect& dst);
// Adds the two Lanczos passes producing rect `dst` of the resized picture from the source window `win` held in `src`.
H recordResize(GpuRecorder& r, H src, const SrcWin& win, int srcW, int srcH, int dstW, int dstH, const WRect& dst);

// float RGB rows -> RGBA16F image (the resident source upload)
bool recordConvert(GpuRecorder& r, hal::BufferH staging, H dst, int w, int h, int dx, int dy);

}  // namespace dapple::engine::gpu

namespace dapple::engine::gpu {
// Ports of a node as graph handles (docs 4.2: the executor owns the images). Point: out[0] = D. In-place spatial nodes:
// in[0] = D. Analysis nodes: in[0] = low-resolution input, out[0] = result.
struct GpuPorts {
  H in[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  H out[4] = {-1, -1, -1, -1};
};
}  // namespace dapple::engine::gpu
