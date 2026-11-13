#pragma once
// Internals shared by the film node translation units (not part of the engine API).
#include <memory>
#include <string>
#include <vector>

#include "engine/nodes/film/film_nodes.h"
#include "film_internal.h"
#include "film_kernels.h"
#include "film_spatial.h"

namespace dapple::engine::filmnode {

namespace fd = dapple::film::detail;

// Table payloads: a FilmTables with only the part of the node populated.
struct PayloadBase {
  virtual ~PayloadBase() = default;
  unsigned long long gen = 0;  // creation order inside the process (frame trace: which table a frame showed, and whether the generation ever goes backwards)
};
struct RawTable : PayloadBase { fd::FilmTables t; };     // mIn, rawN, raw
struct CurvesTable : PayloadBase { fd::FilmTables t; };  // curveN.., dn, d0, gamma, dirQ, dirQ0
struct PrintTable : PayloadBase { fd::FilmTables t; };   // lutN, dmin, dscale, lut, srgbDecode, glareRgb, (split tables)
template <class T>
const T* payloadAs(const void* p) {
  return static_cast<const T*>(static_cast<const PayloadBase*>(p));
}

struct Taps {
  std::vector<float> w;
  int r = 0;
  void set(double sigma) {
    if (sigma > 0) fd::gaussTaps(sigma, w, r);
    else {
      w.assign(1, 1.f);
      r = 0;
    }
  }
};

// Everything derived from the resolved film parameters and the frame size that more than one node needs. Immutable; cached.
struct FilmPlanData {
  dapple::film::FilmParams params;   // digested
  fd::SpatialPlan plan;
  // tile-pass configuration (see film_spatial_render.cpp FinalCfg)
  Taps scan, unsharp, grain, glare, micro;
  Taps dye[3][3];
  int Hs = 0, Hg = 0, Hd = 0, rgl = 0;
  std::uint32_t streamGrain[3] = {}, streamMicro[3] = {}, streamGlare = 0;
  bool scanStage = false;
  bool dirHalf = false;
  bool split = false;       // enlarger diffusion path: FilmPrintExposure / FilmEnlargerBlur / split FilmPrintScan
  std::string error;
};
std::shared_ptr<const FilmPlanData> planOf(const FilmResolved& r);

// KView over the table payloads; `gain` (optional) is folded into the input matrix like FilmSimulator::render does.
fd::KView viewOf(const RawTable* raw, const CurvesTable* cur, const PrintTable* print, const float* gain, float mInStorage[12]);

inline int reflectIdx(int i, int n) {
  if (n == 1) return 0;
  const int period = 2 * n;
  i %= period;
  if (i < 0) i += period;
  return i < n ? i : period - 1 - i;
}

// grain / glare noise seed of a video frame (frame 0 = a still keeps seed 0, i.e. the FilmSimulator's pattern)
inline std::uint32_t filmSeed(std::uint32_t frameIndex) {
  std::uint32_t h = frameIndex * 0x9E3779B1u + 0x7F4A7C15u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

inline Rect grow(Rect r, int d) { return {r.x0 - d, r.y0 - d, r.x1 + d, r.y1 + d}; }
inline Rect shift(Rect r, int dx, int dy) { return {r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}; }
inline int rw(const Rect& r) { return r.x1 - r.x0; }
inline int rh(const Rect& r) { return r.y1 - r.y0; }

// Planar / interleaved accessors over ImageView (absolute frame coordinates).
inline float* planeRow(const ImageView& v, int c, int y, int x) {
  return v.data + std::size_t(c) * v.planeStrideFloats + std::size_t(y - v.origin.y0) * v.strideFloats + std::size_t(x - v.origin.x0);
}
inline float* packedRow(const ImageView& v, int y, int x) {
  return v.data + std::size_t(y - v.origin.y0) * v.strideFloats + std::size_t(3) * std::size_t(x - v.origin.x0);
}
inline Rect rectOf(const ImageView& v) { return {v.origin.x0, v.origin.y0, v.origin.x0 + v.width, v.origin.y0 + v.height}; }
inline fd::Plane planeOf(const ImageView& v, int c) {
  fd::Plane p;
  p.p = v.data + std::size_t(c) * v.planeStrideFloats;
  p.w = v.width;
  p.h = v.height;
  p.stride = std::ptrdiff_t(v.strideFloats);
  return p;
}

// Workspace of the whole-frame nodes (FilmBlurExposure / FilmDirField). A frame driver that owns its own workspace sets this
// (thread-local) so concurrent frames do not serialise on the process-wide one.
extern thread_local fd::Arena* g_frameArena;
// The process-wide workspace of the whole-frame nodes, freed (memory trim before a streaming export). Bytes returned.
std::size_t releaseNodeWorkspace();

// ---- node run functions (film_cpu.cpp) ------------------------------------------------------------------------------------------
Status runExposure(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runBlurExposure(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runDirField(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runDevelop(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runGrain(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runPrintExposure(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runEnlargerBlur(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runPrintScan(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runGlare(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);
Status runScan(const CpuCtx&, const void*, std::span<const ImageView>, std::span<const ImageView>);

// ---- tables (film_tables.cpp) -----------------------------------------------------------------------------------------------------
Status bakeRaw(const void* resolved, std::span<const void* const> inputs, void** payloadOut);
Status bakeCurves(const void* resolved, std::span<const void* const> inputs, void** payloadOut);
Status bakePrintScan(const void* resolved, std::span<const void* const> inputs, void** payloadOut);
void freePayload(void*);
// The three tables from the process-wide cache (per-table keys); false when a bake failed.
bool tablesFor(const FilmResolved& r, FilmTablesHandle& out);
// Counters of the background print tables (wave 8): frames that used a stale table while the worker bakes, tables the worker finished, requests made.
void filmAsyncTableStats(unsigned long long* staleUses, unsigned long long* baked, unsigned long long* requested);

}  // namespace dapple::engine::filmnode
