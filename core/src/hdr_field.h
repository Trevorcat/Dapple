#pragma once
// Single-image HDR ("单张 HDR"): the look of an exposure-bracket merge from one RAW on an SDR screen (round 2: scene adaptive).
//
// Method (clean-room, from the published ideas of bilateral-grid base / detail separation, Paris & Durand 2006 and Chen et al. 2007,
// and of log-domain contrast compression, Durand & Dorsey 2002; the constants are this project's own):
//   * scene-linear luminance after white balance and exposure is taken to the log2 domain (EV);
//   * the BASE layer is a bilateral smoothing of the log luminance, evaluated in a coarse (space x log-luminance) grid built once per
//     source from a ~512 px area-average proxy (one spatial scale of 3.5 cells = 5 % of the long edge, range sigma 0.75 EV): edge
//     preserving by construction (a pixel only mixes with places of similar brightness), so no halo. Everything finer than the base is
//     DETAIL and is never compressed;
//   * the amount is scene adaptive: R = p99 - p1 of the base EV over the frame is compared with the range T the tone curve of THIS frame
//     shows well (the EV span around mid-grey where the curve keeps >= 60 % of its peak slope, Renderer::hdrBind). Only the excess
//     E = R - T is compressed; a scene with R <= T is not touched at all (the node is not even in the graph);
//   * the compression is an asymmetric curve with fixed ends on the base: black (p1) and mid-grey (the scene key) do not move; between
//     them the shadows are lifted by a bump t (1-t)^2 (zero slope change at the key, peak at a third of the way up from black) and
//     above the key the highlights are pulled by a shoulder u^2 up to p99 and held beyond; the peaks are limited so that the base map
//     keeps a slope >= 0.4 (never inverts, never flattens);
//   * the detail (pixel EV - base EV) is multiplied by 1 / slope of that map where the slope is < 1 (local contrast of everything finer
//     than the base does not drop) and faded out toward the noise floor;
//   * the gain is a multiplier on the pixel (colour ratios kept) except for a chroma correction in a cube-root space: a darkening keeps
//     60 % of the colourfulness it would lose, a lifted shadow is pulled toward grey near the noise floor; near-clipped highlights are
//     not darkened (clipped stays white).
// The CPU reference of the per-pixel part is hdr::apply; point.comp is its line-by-line GLSL twin.
#include <cstdint>
#include <memory>
#include <vector>

#include "dapple/image.h"

namespace dapple {

struct SourceImage;

struct HdrField {
  int gw = 0, gh = 0, nb = 0;
  float lmin = -13.f, lstep = 0.5f;  // log2 luminance of bin z = lmin + z * lstep (exposure-free domain)
  float key0 = -2.5f;                // mean log2 luminance of the proxy (exposure-free)
  float lo0 = -8.f, hi0 = -1.f;      // p1 / p99 of the base EV over the proxy (exposure-free)
  std::vector<float> g;              // ((z * gh + y) * gw + x) * 2 = {normalised sum of L, normalised weight}
  std::uint64_t id = 0;              // content id (device cache key)
};

// Resolved per frame (all EV in the anchored scene domain: after white balance and exposure).
struct HdrParams {
  float s = 0.f;      // strength 0..1
  float ev = 0.f;     // log2 of the exposure multiplier (uniform part of the gain)
  float key = -2.5f;  // fixed mid point of the curve (EV)
  float yn = 0.002f;  // noise floor, linear
  float yclip = 1.f;  // luminance of a clipped sensor value after the gain
  float lo = -8.f, hi = -1.f;  // black and highlight ends of the base range (EV)
  float ls = 0.f, hp = 0.f;    // peak lift of the shadows / pull at the top of the range (EV, strength included)
  bool active() const { return ls + hp > 1e-3f; }
};

namespace hdr {

constexpr float kLumR = 0.2880402f, kLumG = 0.7118741f, kLumB = 0.0000857f;  // ProPhoto luminance (as point.comp)
constexpr float kFloor = 1.2e-4f;                                              // exposure-free luminance floor of the grid domain
constexpr float kPrior = 0.003f;                                               // weight of the pixel's own level in the base estimate
constexpr float kLiftCap = 0.25f, kPullCap = 0.30f;  // peak / range: keeps the slope of the base map >= 0.3
constexpr float kShare = 0.45f;                      // share of the excess one end takes at strength 1
constexpr float kKappa = 0.6f;                       // share of the colourfulness a darkening keeps (cube-root space)
constexpr int kProxyEdge = 512;

// Builds the field of a source (cached process-wide by the identity of the source pixels and the white balance). Null when the
// image is empty.
std::shared_ptr<const HdrField> fieldFor(const SourceImage& src, const float wb[3]);
// Same without the cache (tests, synthetic inputs): `lum` = exposure-free luminance proxy, pw x ph.
std::shared_ptr<HdrField> buildField(const std::vector<float>& lum, int pw, int ph);

// `targetRange` T: EV range the frame's tone curve shows well (Renderer::hdrBind derives it from the tone table).
float sliceBase(const HdrField& f, float u, float v, float lc0);  // base EV (exposure-free) at source position (u, v) for a pixel of log luminance lc0
HdrParams resolve(const HdrField& f, float strength01, float expoLog2, float iso, const float gain[3], float targetRange);

// CPU reference: multiplies rgb (anchored linear, after the gain) by the HDR gain at normalised source position (u, v) in [0,1].
void apply(const HdrField& f, const HdrParams& p, float u, float v, float rgb[3]);
// The gain in EV for a pixel of anchored log luminance L whose base is b (shared with the GLSL twin): exposed for tests.
float gainEv(const HdrParams& p, float L, float b);
// Base EV -> EV gain (the curve with fixed ends) and its slope d(b + gain)/db, for tests.
float baseGain(const HdrParams& p, float b, float* slope = nullptr);

}  // namespace hdr
}  // namespace dapple
