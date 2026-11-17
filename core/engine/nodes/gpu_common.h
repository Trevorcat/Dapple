#pragma once
// Shared pieces of the GPU node implementations: the recorder the record functions append to, what a job binds (GpuJob: the frame's blocks,
// source, tables, side planes, film plane), the calls a node / a fused group is recorded with (NodeCall / GroupCall), and the separable filter
// helpers (box / weight table / minimum, with an O(1)-per-pixel running-sum box for larger radii).
//
// Each family's gpu.cpp defines its GpuImpl (declared at the end of this header) and the list engine/nodes/node_list.inc registers it; the GPU
// executor calls them through gpuImplFor(type) in the plan's order.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <span>

#include "engine/nodes/point/blocks.h"
#include "engine/exec/gpu_scheduler.h"
#include "engine/hal/hal.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_blocks.h"
#include "engine/nodes/resize/resize.h"

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

// Workgroup shape of the 2D kernels on a device: 16 x (inv/16), inv = min(256, maxComputeWorkGroupInvocations) as a power of two.
void workgroupShape(const hal::DeviceCaps& c, int& lx, int& ly, int& rwg);

// The image the point pass samples, and where it sits in the (resized) source space.
struct SourceBind {
  H img = -1;
  int ox = 0, oy = 0;
  // portrait reshaping (section 45): the buffer that holds the field (the HDR buffer: field after the HDR grid) and its float offset
  H warp = -1;
  int warpOff = 0;
  // the level-2 reduction of the source picture (reduce_l2.h) as an image of l2w x l2h, and the factors from the frame's source grid to it (the film's
  // quarter-scale input, section 59); -1: none
  H l2 = -1;
  int l2w = 0, l2h = 0;
  float l2kx = 0.f, l2ky = 0.f;
};
// Uniform block tail of the shaders that include warp.glsl
struct WarpU {
  std::int32_t wI[4];
  float wB[4], wC[4];
};
void warpUniform(const point::Warp& w, const SourceBind& s, WarpU& u);
struct LutBind {
  H tone = -1, lat = -1, lat2 = -1;
  H hdr = -1;  // single-image HDR field (SSBO), -1 = none
  int n = 0, n2 = 0, toneN = 0, toneBase = 0;
  float max1 = 1.5f, max2 = 8.f;
};
// The display stages after a film on the device: curves ([4][n] buffer) and the look LUT (RGBA32F n^3); identity when both are off.
struct DisplayBind {
  H curves = -1, lut = -1;
  int lutN = 0;          // 0: no look LUT
  unsigned curveMask = 0;
  int curveSamples = 0;
  float lutMix = 0.f;
  float domMin[3] = {0, 0, 0}, domMax[3] = {1, 1, 1};
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
  // A record function sets this with `err` when the frame asks for something this backend does not port (the CPU renders the frame, the
  // GPU stays in use: GpuReason::FilmEffect); otherwise a failed record is a transient failure.
  bool unsupported = false;

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

// ---- what a job binds (a whole frame, or one tile of one sweep) ------------------------------------------------------------------
struct GpuJob {
  const point::Blocks* frame = nullptr;  // geometry, warp, gain, HDR, film glue, display
  const FrameBlocks* blocks = nullptr;   // the families' resolved blocks
  SourceBind src;
  LutBind lut;
  int W = 0, Hh = 0, ox = 0, oy = 0;     // working image size and its origin in the picture
  int gridW = 0, gridH = 0;              // the 1/4 grid of the frame-global solves
  // local adjustments (docs section 40): descriptor + plane store SSBOs, the side planes the head writes (-1: none)
  H ldesc = -1, lplanes = -1, aux0 = -1, aux1 = -1;
  // a film frame: the film plane, the frame coordinates of its pixel (0, 0), the display stages, the strength of the blend
  H film = -1;
  int filmOx = 0, filmOy = 0;
  // skin protection (film_skin.h): the mask plane (float SSBO, point::Film::skin) and the film input of a window of the frame (img, the offset of its pixel for the
  // window's (0, 0)); set by the executor when the frame asks for it
  H skin = -1;
  std::function<bool(int x0, int y0, int w, int h, H& img, int& ox, int& oy)> filmRef;
  bool filmClamped = false;              // the film plane is already clamped to 0..1
  DisplayBind disp;
  float strength = 1.f;
};
// A fused group (GpuImpl::group): its node types in order, the epilogue of its pass, the working image. Returns the new working image.
struct GroupCall {
  const GpuJob& job;
  std::span<const NodeType> nodes;
  Epilogue ep;
  H D;
};
// A single node (GpuImpl::node): its resolved block, the epilogue, the working image, its solve (GpuImpl::solve result, or -1).
struct NodeCall {
  const GpuJob& job;
  const void* block;
  Epilogue ep;
  H D;
  H solved;
};

// ---- node record functions (nodes/<family>/gpu.cpp) --------------------------------------------------------------------------
struct PointNode {
  const point::Blocks* frame;  // geometry, warp, gain, HDR
  SourceBind src;
  LutBind lut;
  int W, Hh, ox, oy;  // working image size and its origin in the picture
  Epilogue ep;
  // local adjustments (docs section 40): descriptor and plane store SSBOs (point_local when >= 0), the stage-W amount images (or -1)
  H ldesc = -1, lplanes = -1, aux0 = -1, aux1 = -1;
  const vignette::Block* vig = nullptr;  // the Vignette block of the frame (null: no vignette, e.g. the normal branch of a film blend)
};
bool recordPoint(GpuRecorder& r, const PointNode& n, H D);

struct NrLumaNode { const nr::LumaBlock* b; int W, Hh, ox, oy; Epilogue ep; };
bool recordNrLuma(GpuRecorder& r, const NrLumaNode& n, H D);
struct NrChromaNode { const nr::ChromaBlock* b; int W, Hh, ox, oy; Epilogue ep; };
bool recordNrChroma(GpuRecorder& r, const NrChromaNode& n, H D);

// dehaze analysis of the low-resolution colour grid: dark channel + airlight statistics (into gBuf). Returns the dark channel.
H recordDehazeAnalysis(GpuRecorder& r, const dehaze::Block& b, H col, int lw, int lh);
struct DehazeNode { const dehaze::Block* b; int W, Hh, ox, oy, gridW, gridH; H dc; Epilogue ep; };
bool recordDehazeApply(GpuRecorder& r, const DehazeNode& n, H D);

// guided filter coefficients (a, b) of the clarity base from the (Y, Y^2) grid
H recordClarityAnalysis(GpuRecorder& r, int radius, H yy, int lw, int lh);
struct DetailNode {
  const engine::detail::Block* b;
  int W, Hh, ox, oy, gridW, gridH;
  H ab;  // clarity coefficients (or -1)
  Epilogue ep;
};
bool recordDetail(GpuRecorder& r, const DetailNode& n, H D);

// LocalDetail (local adjustments, stage W): returns the new working image (D itself when it is the last pass), -1 on error
struct LocalDetailNode {
  const localadj::Block* b;
  int W, Hh, ox, oy;
  H aux0, aux1;  // the amounts the point pass wrote
  Epilogue ep;
};
H recordLocalDetail(GpuRecorder& r, const LocalDetailNode& n, H D);
// The scene input of a film frame with the local adjustments applied (stage L; geometry gather included), RGBA32F `out` of n.W x n.Hh at
// (n.ox, n.oy); writes the stage-W amounts into aux0 / aux1 when both are >= 0.
struct LocalFilmInNode {
  const point::Blocks* frame;  // geometry, warp, gain
  SourceBind src;
  int W, Hh, ox, oy;
  H ldesc, lplanes, aux0, aux1;
  bool quarter = false;  // the film input at a quarter of the picture's scale (n.W x n.Hh pixels = the picture's ceil(W / 4)): src.l2 sampled through the geometry
};
bool recordLocalFilmIn(GpuRecorder& r, const LocalFilmInNode& n, H out);

// Lanczos-3 resize planning and passes
struct SrcWin {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};
SrcWin lanczosWindow(const resize::Block& b, const WRect& dst);  // resize::sourceWindow
// Adds the two Lanczos passes producing rect `dst` of the resized picture from the source window `win` held in `src`.
H recordResize(GpuRecorder& r, H src, const SrcWin& win, const resize::Block& b, const WRect& dst);

// float RGB rows -> RGBA16F image (the resident source upload)
bool recordConvert(GpuRecorder& r, hal::BufferH staging, H dst, int w, int h, int dx, int dy);

}  // namespace dapple::engine::gpu

namespace dapple::engine::gpu {
// The families' GPU implementations (registered by engine/nodes/node_list.inc; null = none).
const GpuImpl* noGpu();
const GpuImpl* pointGpu();         // nodes/point/gpu.cpp: the point recipe (Resize / Geometry + Gain + Hdr + LocalLinear + ToneColour + Vignette)
const GpuImpl* toneColourGpu();    // the same recipe; reads the colour lattice
const GpuImpl* displayGpu();       // DisplayStages + Blend after a film: the film finish (curves, look LUT, blend with the normal branch)
const GpuImpl* epilogueGpu();      // Grain, Pack8: the epilogue slot of the last pass
const GpuImpl* nrLumaGpu();        // nodes/nr/gpu.cpp
const GpuImpl* nrChromaGpu();
const GpuImpl* dehazeGpu();        // nodes/dehaze/gpu.cpp
const GpuImpl* detailGpu();        // nodes/detail/gpu.cpp
const GpuImpl* localDetailGpu();   // nodes/local/gpu.cpp
const GpuImpl* resizeGpu();        // nodes/resize/gpu.cpp (the source resize is recorded by the executor: recordResize)
const GpuImpl* filmGpu();          // nodes/film/film_gpu.cpp: the film stage records the film nodes as a whole
}  // namespace dapple::engine::gpu
