#pragma once
// Film simulation on the GPU executor (docs/engine/pipeline-architecture.md 8.1, step b4 (2)): the film nodes as compute passes
// appended to a GpuGraph. The tables are the CPU bake (FilmTableRaw / Curves / PrintScan payloads) uploaded as they are, so the
// colour science exists once (docs 3: tables are always baked on the CPU).
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
  // the table constants the passes need (copied at upload so a frame needs no CPU table)
  float mIn[12] = {}, gamma[4] = {}, curveX0 = 0, curveInvDx = 0, dirQ[12] = {}, dirQ0[4] = {}, dmin[4] = {}, dscale[4] = {};
  float glareRgb[4] = {};
};

// Uploads the three table payloads (device local). Bit-identical: texels are copied, never recomputed.
bool uploadGpuTables(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
// Replaces only the print LUT texels (same raw / curve tables, same LUT size, no split path); false when not applicable.
bool updateGpuPrintLut(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
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
gpu::H recordFilmFrame(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pr, gpu::H src,
                       int w, int h);

// ---- whole-frame stage + windows (tiled export; recordFilmFrame is both for one window = the frame) ----------------------------------------
// The whole-frame stage: exposure, (highlight boost), the camera / halation mixtures and the DIR inhibitor field. `any` = false: the film has
// no spatial effect and `disp` is the finished display image of the frame.
struct FilmPre {
  gpu::H e2 = -1, q = -1, disp = -1;
  bool any = false;
};
bool recordFilmPre(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, gpu::H src, int w, int h, FilmPre& out);
// A rectangle of the frame in frame coordinates; the display image of a window starts at (x0, y0) of the picture.
struct FilmWin {
  int x0 = 0, y0 = 0, w = 0, h = 0;
};
struct FilmImg {
  gpu::H img = -1;  // RGBA16F display image (sRGB encoded, unclamped); -1 on error (`r.err`)
  int ox = 0, oy = 0;  // frame coordinates of its pixel (0, 0)
};
// The develop / grain / print / scan chain over `win`. The image is only valid inside `win` shrunk by filmWindowHalo() on the sides that are
// not a frame border (the caller passes a window grown by the halo and clipped to the frame): the values there are those of a whole-frame run.
// `pre` carries the frame-sized E2 / q images (externals of a persistent copy in a tiled export). fw x fh = the frame.
// True when the last film pass of this frame clamps to 0..1 itself (film_pd ENCODE with FilmResolved::clampOut): the executor then skips film_fin when that
// would be a pure clamp.
bool filmEndsClamped(const FilmResolved& res);

FilmImg recordFilmWindow(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pr, const FilmPre& pre, int fw, int fh,
                         const FilmWin& win);
int filmWindowHalo(const FilmResolved& res);
// False for the enlarger diffusion filter (a whole-frame blur after the grain): such a film cannot be exported in tiles.
bool filmWindowable(const FilmResolved& res);

// IEEE half -> float (the film planes are RGBA16F; tests read them back)
float halfToFloat(std::uint16_t h);

// Test hook: a Gaussian mixture of one plane (w x h floats) on the GPU, `half` = the input is the half-resolution plane.
bool gpuMixtureForTest(gpu::Core& core, const float* plane, int w, int h, const void* mixture, bool half, std::vector<float>& out,
                       std::string& err);

}  // namespace dapple::engine::filmnode
