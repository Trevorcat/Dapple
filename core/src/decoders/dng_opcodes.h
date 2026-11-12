#pragma once
// DNG opcode lists (DNG specification 1.6, chapter 7 "Opcode List Processing"), the subset phones use:
//   GainMap (9)            lens shading, per Bayer plane, OpcodeList2 (applied on the mosaic before demosaic)
//   FixVignetteRadial (3)  radial gain, OpcodeList1/2 (mosaic) or OpcodeList3 (demosaiced image)
//   WarpRectilinear (1)    radial + tangential distortion (and lateral CA with per-plane sets), OpcodeList3 (after demosaic)
// Everything else is reported as skipped (optional opcodes silently, required ones with a note: the decode never fails because of
// an opcode, a photo without its correction is better than no photo). Pure functions, no LibRaw dependency: tests/ drives them with
// synthetic lists. See docs/engine/pipeline-architecture.md section 27.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dapple/image.h"

namespace dapple::dng {

enum OpcodeId : std::uint32_t {
  kWarpRectilinear = 1, kWarpFisheye = 2, kFixVignetteRadial = 3, kFixBadPixelsConstant = 4, kFixBadPixelsList = 5,
  kTrimBounds = 6, kMapTable = 7, kMapPolynomial = 8, kGainMap = 9, kDeltaPerRow = 10, kDeltaPerColumn = 11,
  kScalePerRow = 12, kScalePerColumn = 13, kWarpRectilinear2 = 14,
};

struct Opcode {
  std::uint32_t id = 0, version = 0, flags = 0;
  std::vector<std::uint8_t> params;  // big-endian parameter area
  bool optional() const { return (flags & 1u) != 0; }
};

struct OpcodeList {
  std::vector<Opcode> ops;
  std::string error;  // non-empty when the list was truncated / malformed (ops holds what parsed before the fault)
};

// `data` is the OpcodeListN tag payload (big-endian, whatever the file's byte order).
OpcodeList parseOpcodeList(const void* data, std::size_t len);

struct GainMap {
  int top = 0, left = 0, bottom = 0, right = 0;  // area, active-area pixel coordinates, [top, bottom) x [left, right)
  int plane = 0, planes = 1, rowPitch = 1, colPitch = 1;
  int pointsV = 0, pointsH = 0, mapPlanes = 1;
  double spacingV = 0, spacingH = 0, originV = 0, originH = 0;  // relative to the image height / width
  std::vector<float> gain;                                          // [pointsV][pointsH][mapPlanes]

  // Gain at a pixel of an image of size w x h: the map point (i, j) sits at relative position origin + i * spacing, the pixel at
  // y / (h - 1) (first and last pixel at 0 and 1); bilinear inside the map, edge replication outside (spec chapter 7).
  float at(int y, int x, int w, int h, int mapPlane) const;
};
bool parseGainMap(const Opcode& op, GainMap& out);

struct FixVignette {
  double k[5] = {0, 0, 0, 0, 0}, cx = 0.5, cy = 0.5;
  // gain at pixel (x, y) of a w x h image: 1 + k0 r^2 + ... + k4 r^10, r = distance to the centre / farthest-pixel distance
  float at(int x, int y, int w, int h) const;
};
bool parseFixVignette(const Opcode& op, FixVignette& out);

struct WarpRect {
  int sets = 1;  // 1 (all planes) or one per plane
  double kr[3][4] = {}, kt[3][2] = {};
  double cx = 0.5, cy = 0.5;
  // source position (spec: x', y') of destination pixel (x, y) of a w x h image for plane set s
  void source(int s, int w, int h, double x, double y, double& sx, double& sy) const;
};
bool parseWarpRect(const Opcode& op, WarpRect& out, int imagePlanes = 3);

struct ApplyReport {
  std::vector<std::string> applied, skipped;
  std::string summary() const;  // "GainMap x4, FixVignetteRadial; skipped: WarpFisheye"
};

// Mosaic (OpcodeList1/2 stage, one plane): 16-bit raw values; black[] is the black level pattern (period bpr x bpc, indexed by active-area
// row / column), gains act on the linearised value (v - black): v' = black + (v - black) * g, clipped to [0, maxValue].
struct MosaicView {
  std::uint16_t* px = nullptr;
  std::size_t pitch = 0;  // elements per buffer row
  int top = 0, left = 0;  // active area origin inside the buffer
  int width = 0, height = 0;
  std::vector<float> black = {0.f};
  int bpr = 1, bpc = 1;
  float maxValue = 65535.f;
};
void applyMosaic(const OpcodeList& list, MosaicView& view, ApplyReport& rep);

// Demosaiced image (OpcodeList3 stage). WarpRectilinear replaces `img` (same size, edge-clamped outside the source: the
// DefaultCrop removes that border).
void applyImage(const OpcodeList& list, ImageF& img, ApplyReport& rep);

// True when every opcode is a GainMap whose gains are all exactly 1 (Samsung writes such no-op maps into LinearRaw files).
bool isIdentityGainList(const OpcodeList& list);

ImageF warpRectilinear(const ImageF& src, const WarpRect& w);
ImageF cropImage(const ImageF& src, int x0, int y0, int w, int h);

}  // namespace dapple::dng
