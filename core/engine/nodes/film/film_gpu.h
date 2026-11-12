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
  int rawN = 0, curveN = 0, lutN = 0;
  // the table constants the passes need (copied at upload so a frame needs no CPU table)
  float mIn[12] = {}, gamma[4] = {}, curveX0 = 0, curveInvDx = 0, dirQ[12] = {}, dirQ0[4] = {}, dmin[4] = {}, dscale[4] = {};
  float glareRgb[4] = {};
};

// Uploads the three table payloads (device local). Bit-identical: texels are copied, never recomputed.
bool uploadGpuTables(gpu::Core& core, const FilmTablesHandle& tb, GpuFilmTables& out, std::string& err);
void releaseGpuTables(gpu::Core& core, GpuFilmTables& t);

// Per-frame resources derived from the plan (grain sampler tables): uploaded when the grain parameters change.
struct GpuFilmPlanRes {
  hal::BufferH grainTab;  // per channel: inverse table + merged-sampler table
  int grainTableN = 0;
};
bool uploadGpuPlan(gpu::Core& core, const FilmResolved& res, GpuFilmPlanRes& out, std::string& err);
void releaseGpuPlan(gpu::Core& core, GpuFilmPlanRes& t);

// Appends the film passes for a w x h frame whose linear ProPhoto source (ungained, RGBA16F or RGBA32F) is `src`. Returns the
// RGBA32F display image (sRGB encoded, unclamped) or -1 (unsupported effect / error in `r.err`).
gpu::H recordFilmFrame(gpu::GpuRecorder& r, const FilmResolved& res, const GpuFilmTables& tb, const GpuFilmPlanRes& pr, gpu::H src,
                       int w, int h);

// Test hook: a Gaussian mixture of one plane (w x h floats) on the GPU, `half` = the input is the half-resolution plane.
bool gpuMixtureForTest(gpu::Core& core, const float* plane, int w, int h, const void* mixture, bool half, std::vector<float>& out,
                       std::string& err);

}  // namespace dapple::engine::filmnode
