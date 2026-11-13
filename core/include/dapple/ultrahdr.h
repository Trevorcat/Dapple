#pragma once
// Ultra HDR (JPEG_R, gain-map JPEG): an SDR base JPEG plus a gain map stored as a second JPEG, MPF index, XMP with the hdrgm
// namespace (Ultra HDR v1) and the ISO 21496-1 metadata block. Written from the public specifications; docs/engine/pipeline-architecture.md
// section 34. Everything here is pure data (no engine): the session side (c_api.cpp) renders the two renditions.

#include <cstdint>
#include <string>
#include <vector>

#include "dapple/image.h"
#include "dapple/result.h"

namespace dapple::uhdr {

using Bytes = std::vector<std::uint8_t>;

constexpr float kMaxHeadroomStops = 3.f;  // the cap of the headroom H (stops above SDR white)
constexpr float kWorthPeak = 1.5f;        // the 99.9th percentile of the bright rendition must reach this (0.58 stop) for an HDR export to be worth it
constexpr float kOffset = 1.f / 64.f;     // OffsetSDR = OffsetHDR (Ultra HDR default)

// Metadata of the gain map. Gains are log2 (stops). Single channel.
struct Meta {
  float gainMin = 0.f, gainMax = 0.f, gamma = 1.f;
  float offsetSdr = kOffset, offsetHdr = kOffset;
  float capacityMin = 0.f, capacityMax = 0.f;  // HDRCapacityMin / Max: the display headroom (stops) at which the HDR rendition is shown in full
};

struct GainMap {
  int width = 0, height = 0;
  Bytes data;  // 8 bit, width * height, 0 = gainMin, 255 = gainMax
  Meta meta;
  float maxGain = 0.f;  // largest log2(HDR / SDR) before encoding
};

// ---- the HDR rendition -------------------------------------------------------------------------------------------------
// `bright` is the same edit rendered with the exposure lowered by H stops (nothing is changed but exposure), so that the highlights the SDR
// shoulder squeezes lie below it. bright linear * 2^H is the scene luminance in units of SDR white (1 = SDR white, mid-tones lie below it).
// HDR linear = SDR linear + excess, excess = (Yb - 1)+ * smoothstep(0, 1, (Yb - 1)+) in the colour direction of the bright rendition (the video policy of
// section 30.8: the part above SDR white is added to the display referred result, with the hue of the source). Everything with Yb <= 1 stays bit for bit
// the SDR rendition.
float srgbToLinear(float e);
float linearToSrgb(float l);
// Peak of the bright rendition (99.9th percentile of its linear luminance times 2^H), in SDR white units.
float brightPeak(const Image8& bright, float H);
// H from the peak: ceil(2 log2 peak) / 2 clamped to 0.5 .. kMaxHeadroomStops; 0 when the peak is below kWorthPeak (no headroom worth encoding).
float autoHeadroom(float peak);
// Linear HDR rendition (3 floats per pixel, 1 = SDR white) of two renditions of the same size.
ImageF hdrLinear(const Image8& sdr, const Image8& bright, float H);
// Gain map from the renditions (same size, usually a quarter of the base picture): luminance gain log2((Yh + o) / (Ys + o)), 3x3 minimum then a
// [1 2 1] smoothing so the up-scaled map does not light the dark side of an edge.
GainMap makeGainMap(const Image8& sdr, const Image8& bright, float H);
// Decoder side (what a viewer does at full display headroom): linear HDR from the SDR picture and the gain map (bilinear up-scale).
ImageF reconstruct(const Image8& sdr, const GainMap& gm);

// ---- container ---------------------------------------------------------------------------------------------------------
// Grayscale baseline JPEG of the gain map (quality 1..100).
Bytes encodeGray(const std::uint8_t* data, int width, int height, int quality);
// base JPEG + APP1 XMP + APP2 ISO + APP2 MPF + the gain-map JPEG. The base stream (every byte of the original) is kept: only segments are added after the
// leading APPn segments.
Result<Bytes> assemble(const Bytes& baseJpeg, const GainMap& gm, int gainQuality = 92);

struct Parsed {
  std::size_t primaryEnd = 0;       // size of the primary JPEG (SOI..EOI)
  std::size_t gainOffset = 0, gainSize = 0;
  // MPF index as written (offsets relative to the MPF header, entry 1)
  std::uint32_t mpfCount = 0, mpfPrimarySize = 0, mpfGainSize = 0, mpfGainOffset = 0, mpfGainOffsetAbs = 0;
  std::string primaryXmp, gainXmp;
  bool primaryHasIso = false, gainHasIso = false;
  Meta meta;       // from the gain map's XMP
  Meta isoMeta;    // from the ISO block
  int gainWidth = 0, gainHeight = 0, gainComponents = 0;
  Bytes base;      // the primary with the added segments removed (the original base JPEG)
  Bytes gainJpeg;
  std::string gainSemantic;  // Item:Semantic of the second directory entry
  std::size_t directoryLength = 0;  // Item:Length of the gain map entry
};
Result<Parsed> parse(const Bytes& file);

// ISO 21496-1 binary block (after the URN) for a single channel map; parseIso reads it back.
Bytes isoBlock(const Meta& m);
bool parseIso(const std::uint8_t* p, std::size_t n, Meta& m);

}  // namespace dapple::uhdr
