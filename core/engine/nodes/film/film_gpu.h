#pragma once
// Film simulation on the GPU executor (docs/engine/pipeline-architecture.md 8.1, step b4 (2)): the film nodes as compute passes
// appended to a GpuGraph. The tables are the CPU bake (FilmTableRaw / Curves / PrintScan payloads) uploaded as they are, so the
// colour science exists once (docs 3: tables are always baked on the CPU).
#include <functional>
#include <string>
#include <vector>

#include "engine/exec/gpu_executor.h"
#include "engine/nodes/film/film_nodes.h"
#include "engine/nodes/gpu_common.h"

namespace dapple::engine::filmnode {

struct GpuFilmTables {
  hal::ImageH raw;        // RGBA32F (rawN+2)^2, reflect padded
  hal::ImageH lut;        // RGBA32F N^3 print + scan LUT
  hal::BufferH curves;    // dn then d0, [3][curveN] (value, slope)
  hal::BufferH decode;    // 4097 sRGB decode entries
  // enlarger diffusion path (only when the print table carries the split tables): density -> log10 paper exposure LUT, paper density ->
  // display LUT, paper curves ([3][paperN] value + slope)
  hal::ImageH lut1, lut3;  // RGBA32F N^3
  hal::BufferH paper;
  bool split = false;
  int paperN = 0;
  float paperX0 = 0, paperInvDx = 0, dminS[4] = {}, dscaleS[4] = {};
  int rawN = 0, curveN = 0, lutN = 0;
  // asynchronous print LUT update (startGpuPrintLutUpdate): the new image and its copy fence; the old image stays bound until the copy completed
  hal::ImageH lutNext;
  hal::FenceH lutFence;
  bool lutNextActive = false;
  int lutNextN = 0;
  float dminNext[4] = {}, dscaleNext[4] = {}, glareRgbNext[4] = {};
  std::shared_ptr<const PrintTable> printNext;
  // the table constants the passes need (copied at upload so a frame needs no CPU table)
  float mIn[12] = {}, gamma[4] = {}, curveX0 = 0, curveInvDx = 0, dirQ[12] = {}, dirQ0[4] = {}, dmin[4] = {}, dscale[4] = {};
  float glareRgb[4] = {};
};

// Uploads the three table payloads (device local). Bit-identical: texels are copied, never recomputed.
bool uploadGpuTables(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
// Replaces only the print LUT texels (same raw / curve tables, same LUT size, no split path); false when not applicable.
bool updateGpuPrintLut(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
// Fence-and-swap form (wave 8): startGpuPrintLutUpdate records the copy into a NEW LUT image and submits it without waiting; finishGpuPrintLutUpdate (poll each frame)
// swaps the binding when the fence completed and destroys the old image. No waitAll, no staging copy that the frame thread waits for. Returns false when not applicable.
bool startGpuPrintLutUpdate(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
bool finishGpuPrintLutUpdate(gpu::Core& core, GpuFilmTables& out);
void releaseGpuTables(gpu::Core& core, GpuFilmTables& t);

// False (with `why`) only when a blur kernel exceeds the 19 tap table of its pass (dye-cloud blur and grain micro-structure are ported:
// film_dye.comp): the executor then leaves the frame to the CPU. Same checks as recordFilmFrame. Highlight boost and the enlarger diffusion filter are
// ported (film_boost.comp; film_point.comp MODE 6 / 7 + the mixture passes); they need a FilmResolved with `fx` set and tables uploaded
// from a print table that carries the split tables (uploadGpuTables does that on its own).
bool filmGpuSupported(const FilmResolved& res, std::string& why);

// Per-frame resources derived from the plan (grain sampler tables): uploaded when the grain parameters change.
struct GpuFilmPlanRes {
  hal::BufferH grainTab;  // per channel: inverse table + merged-sampler table
  int grainTableN = 0;
};
bool uploadGpuPlan(gpu::Core& core, const FilmResolved& res, GpuFilmPlanRes& out, std::string& err);
void releaseGpuPlan(gpu::Core& core, GpuFilmPlanRes& t);

// Appends the film passes for a w x h frame whose linear ProPhoto source (ungained, RGBA16F or RGBA32F) is `src`. Returns the
// RGBA32F display image (sRGB encoded, unclamped) or -1 (unsupported effect / error in `r.err`).
// Test hook: -1 environment (default: on), 0 / 1 force the interactive-tier merging of the small exposure-mixture components off / on.
void setFilmMergeForTest(int mode);
// film_v3 variant (direct taps = 1, staged column tile = 0, -1 = no override): the warm-up renders a frame each way and keeps the faster one for the device
void setFilmV3Override(int direct);
void setFilmV3Verdict(int direct);
// Measured at warm-up: milliseconds per megapixel of an interactive film frame on this device (0 = not measured). Seeds the interactive size.
void setFilmInteractiveMsPerMpx(float v);
float filmInteractiveMsPerMpx();
gpu::H recordFilmFrame(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pr, gpu::H src,
                       int w, int h);

// ---- windows of a film frame (tiled export; recordFilmFrame is the same code with one window = the frame) ------------------------------------
// A rectangle of the frame in frame coordinates; the display image of a window starts at (x0, y0) of the picture.
struct FilmWin {
  int x0 = 0, y0 = 0, w = 0, h = 0;
};
struct FilmImg {
  gpu::H img = -1;  // RGBA16F display image (sRGB encoded, unclamped); -1 on error (`r.err`)
  int ox = 0, oy = 0;  // frame coordinates of its pixel (0, 0)
};
// The film's input (linear ProPhoto, ungained, RGBA32F) over a window: pixel (i, j) of the window is pixel (i + ox, j + oy) of `img`. The window handed
// to the function is aligned (origin and size multiples of 4 unless it meets the frame's edge). The source may be resident (a frame-sized image: ox = x0)
// or only the window (ox = 0).
struct FilmSrc {
  gpu::H img = -1;
  int ox = 0, oy = 0;
};
using FilmSrcFn = std::function<FilmSrc(const FilmWin&)>;
// The part of a film frame that belongs to the whole picture and is kept between the windows: the level-2 accumulator planes (1/16 of the pixels, RGBA16F)
// of the camera / halation / DIR mixtures, each -1 when its mixture has no component above level 1 or is absent. `any` = the film has spatial effects.
struct FilmPre {
  gpu::H lowPre = -1, lowHalo = -1, lowDir = -1;
  bool any = false;
};
// Size of those planes for a w x h frame.
void filmLowSize(int w, int h, int& lw, int& lh);
// Builds the low planes by walks over windows of the film input (the source of a window comes from `src`; the planes are the only thing that stays).
// Not needed when one window covers the frame (recordFilmWindow then builds them itself).
// The same walks window by window, for a source that is read band by band (each window its own graph and device turn). `FilmLowPlan` says which walks
// the film needs (need[0] = camera mixture input E, [1] = halation input pre(E), [2] = DIR input) and how far a walk's window reaches around its tile;
// filmLowPlan is false when the film has no window form (highlight boost, unsupported effect). A walk: for every tile of kBand px (row-major) the window
// filmLowSourceWindow(tile) of the film input is handed to `src`, recordFilmLowWindow writes the tile's share into the level-2 input plane `p2`
// (l2w x l2h, RGBA16F, any content) and recordFilmLowBuild makes the low plane of the stage from the complete p2 (-1 on error). Walk 1 needs the
// low plane of walk 0 (`lowPre`) and walk 2 those of 0 and 1.
struct FilmLowPlan {
  static constexpr int kBand = 512;
  bool any = false, need[3] = {false, false, false};
  int grow[3] = {0, 0, 0};
  int l2w = 0, l2h = 0;
};
bool filmLowPlan(const FilmResolved& res, int w, int h, FilmLowPlan& out);
FilmWin filmLowSourceWindow(const FilmLowPlan& lp, int stage, int w, int h, const FilmWin& tile);
bool recordFilmLowWindow(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const FilmLowPlan& lp, int stage, int w, int h, const FilmWin& tile,
                         const FilmSrcFn& src, gpu::H p2, gpu::H lowPre, gpu::H lowHalo);
gpu::H recordFilmLowBuild(gpu::GpuRecorder& r, const FilmResolved& res, int stage, int w, int h, gpu::H p2);
bool recordFilmLow(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const FilmSrcFn& src, int w, int h, FilmPre& out);
// The develop / grain / print / scan chain over `win`, with the exposure, the camera / halation mixtures and the DIR field recomputed inside the window
// (grown by the mixtures' reach and aligned: no frame-sized E2 / q). The image is only valid inside `win` shrunk by filmWindowHalo() on the sides that are
// not a frame border (the caller passes a window grown by that halo and clipped to the frame): the values there are those of a whole-frame run.
// fw x fh = the frame. A window that does not cover the frame needs `pre` from recordFilmLow (when the film has mixtures above level 1).
// True when the last film pass of this frame clamps to 0..1 itself (film_pd ENCODE with FilmResolved::clampOut): the executor then skips film_fin when that
// would be a pure clamp.
bool filmEndsClamped(const FilmResolved& res);

FilmImg recordFilmWindow(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pr, const FilmPre& pre, const FilmSrcFn& src,
                         int fw, int fh, const FilmWin& win);
// Reach of the develop / grain / print / scan chain (pixels around a window it reads), and of the exposure mixtures (camera + halation, DIR field) that are
// recomputed inside the window; the two add up. Per node, for NodeBehaviour::halo: filmMixHalo(res, 0) = camera + halation, 1 = DIR.
int filmWindowHalo(const FilmResolved& res);
int filmMixHalo(const FilmResolved& res, int which = -1);
// False for the enlarger diffusion filter (a whole-frame blur after the grain): such a film cannot be exported in tiles.
bool filmWindowable(const FilmResolved& res);

// IEEE half -> float (the film planes are RGBA16F; tests read them back)
float halfToFloat(std::uint16_t h);

// Test hook: a Gaussian mixture of one plane (w x h floats) on the GPU, `half` = the input is the half-resolution plane.
bool gpuMixtureForTest(gpu::Core& core, const float* plane, int w, int h, const void* mixture, bool half, std::vector<float>& out,
                       std::string& err);

}  // namespace dapple::engine::filmnode
