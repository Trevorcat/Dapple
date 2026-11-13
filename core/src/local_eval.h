#pragma once
// Internal: evaluation of local adjustments (docs/engine/pipeline-architecture.md section 40). The resolved form of one frame
// (`local::Frame`), the low-resolution photo-space planes (brush coverage, raster guided-filter coefficients) with their cache,
// the raster store of the C API, and the CPU kernels of the nodes LocalLinear (point) and LocalDetail (window). The GPU kernels
// (shaders/local.glsl, shaders/local_detail.comp) are twins of the per-pixel code here: keep the arithmetic identical.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dapple/decoder.h"
#include "dapple/image.h"
#include "dapple/local_adjust.h"

namespace dapple {
namespace jsonlite { struct JValue; }
bool localFromValue(const jsonlite::JValue& v, std::vector<LocalAdjustment>& out);  // local_adjust.cpp
class FrameKernels;
}  // namespace dapple

namespace dapple::local {

void logLine(const std::string& s);

// ---- raster store (process-wide; dapple_mask_raster_set) -------------------------------------------------------------------------
struct Raster {
  int w = 0, h = 0;
  std::vector<std::uint8_t> px;  // w * h
  std::uint64_t hash = 0;        // content hash (size + bytes)
};
bool rasterSet(const std::string& id, int w, int h, const std::uint8_t* bytes, int stride);
bool rasterRemove(const std::string& id);
bool rasterHas(const std::string& id);
std::shared_ptr<const Raster> rasterGet(const std::string& id);
std::uint64_t rasterContentHash(const std::string& id);  // 0 = absent

// ---- low-resolution photo-space planes ----------------------------------------------------------------------------------------------
struct Plane {
  int w = 0, h = 0, ch = 1;      // brush: 1 channel (coverage); raster: 4 (a.rgb, b)
  std::vector<float> data;       // w * h * ch
  std::uint64_t key = 0;
  std::size_t bytes() const { return data.size() * sizeof(float); }
};

struct Counters {
  std::uint64_t brushBuilds = 0, brushPrefixHits = 0, rasterBuilds = 0, guideBuilds = 0, planeHits = 0, binds = 0;
};
Counters counters();
void resetPlaneCache();  // tests

// ---- the resolved local adjustments of one frame ------------------------------------------------------------------------------------
enum CompKind : int { kCLinear = 0, kCRadial = 1, kCLum = 2, kCColour = 3, kCBrush = 4, kCRaster = 5 };
struct Comp {
  int kind = 0, op = 0;
  bool invert = false;
  float p[8] = {};               // kind parameters in the photo metric space (see local_eval.cpp: resolve)
  std::shared_ptr<const Plane> plane;
  int planeOffset = 0;           // in floats, into the GPU plane store
};
struct Adj {
  bool invert = false;
  float weight = 1.f;            // strength / 100
  int c0 = 0, nc = 0;            // components [c0, c0 + nc)
  float lin[12] = {};            // stage L: exposure EV, contrast, highlights, shadows, whites, blacks (-1..1), temp, tint (-100..100), saturation,
                                 // vibrance (-1..1), tint amount (0..1), unused
  float tintRgb[3] = {1.f, 1.f, 1.f};  // luminance-normalised colour of the tint (working space)
  float win[kLocalWindowSlots] = {};   // stage W: clarity, texture, sharpen (-1..1), noise (0..1), dehaze (-1..1)
  bool hasLinear = false;
  int srcIndex = 0;              // index in EditParams::local
};
struct Frame {
  std::vector<Adj> adj;
  std::vector<Comp> comp;
  float sx = 1.f, sy = 1.f;      // photo metric: x * sx, y * sy are in units of the long edge
  float guideRef = 1.f;          // raster guide: I = sqrt(clamp(v / guideRef, 0, 4))
  bool anyLinear = false, anyWindow = false, anyRaster = false, needGuided = false;
  bool winUsed[kLocalWindowSlots] = {};
  std::uint64_t descKey = 0, planesKey = 0;
  int planeFloats = 0;           // size of the GPU plane store
  // GPU descriptor block (vec4 units; layout in local.glsl)
  std::vector<float> desc;
  bool active() const { return !adj.empty(); }
};

// Resolves the list for a frame rendered from `src` (any scaled copy of the photo; its aspect defines the metric). `interactive`
// selects the coarse planes. Null when nothing is active (no enabled adjustment with an op and a mask).
// `preview` (UI overlay): every adjustment with a mask, enabled or not, ops or not, at weight 1.
std::shared_ptr<const Frame> bind(const std::vector<LocalAdjustment>& list, const SourceImage& src, bool interactive, bool preview = false);

// The GPU plane store of a frame (concatenated planes, floats): built on a cache miss of Frame::planesKey only.
std::vector<float> planeStore(const Frame& f);

// ---- per-pixel kernels (CPU) ---------------------------------------------------------------------------------------------------------
// Mask of adjustment `a` (weight applied: 0..1) at photo point (u, v) for the stage input pixel `lin` (scene-linear after global gain / HDR)
// and the raster guide pixel `src` (pre-gain source value).
float maskOf(const Frame& f, int a, float u, float v, const float lin[3], const float src[3]);
// Stage L of one pixel: masks on the input, ops in list order; K (5 floats or null) receives the summed stage-W amounts.
void applyLinear(const Frame& f, float u, float v, const float src[3], float px[3], float* K);

// Where LocalLinear writes the stage-W amounts of the work window (planar floats, kLocalWindowSlots planes).
struct AuxTarget {
  float* data = nullptr;
  int ox = 0, oy = 0, w = 0, h = 0;  // the window in the frame
  std::size_t plane = 0;             // floats per plane
  int slot[kLocalWindowSlots] = {-1, -1, -1, -1, -1};  // plane index of each stage-W op (-1: no adjustment uses it, not stored)
};
// The planes a frame needs (only the stage-W ops some adjustment uses) and their slot map.
int auxLayout(const Frame& f, AuxTarget& t);
// The LocalLinear block fragment: n pixels of row y starting at x0 (frame coordinates).
void linearBlock(const Frame& f, const FrameKernels& fk, float* px, int n, int x0, int y, const AuxTarget* aux);

// Radii of LocalDetail at a radii scale (frame long edge / 2048, as the spatial stages).
struct DetailRadii {
  int rt = 1, rs = 1, rg = 2;  // fine box (2 passes), very fine box (1 pass), guided radius on the 1/4 grid
  bool guided = false;
  int halo = 0;                // pixels, multiple of 4
};
DetailRadii detailRadii(const Frame& f, float scale);
// The LocalDetail window node: `img` is the work window at (ox, oy) of a fw x fh frame, `aux` its amount planes.
void detailWindow(const Frame& f, ImageF& img, const AuxTarget& aux, int fw, int fh, float scale);

// One row of the mask of adjustment `index` (0..255) over the output frame of `fk` (UI overlay).
void maskRow(const Frame& f, const FrameKernels& fk, int index, int y, std::uint8_t* out);
// The stage input of a photo point as the range components see it (UI colour / luminance picker): L* 0..100, hue 0..360, sat.
void sampleRange(const float lin[3], float& Lstar, float& hue, float& sat);

}  // namespace dapple::local
