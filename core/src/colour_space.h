#pragma once
// Colour spaces of display-referred sources (JPEG / PNG): identified from explicit tags first (cICP, sRGB, cHRM + gAMA), then from a
// matrix / TRC ICC profile (small parser, no CMS), else sRGB with a note. The result is a per-channel transfer function plus the 3x3 that
// moves the linear source RGB into the engine's linear ProPhoto (D50) working space (Bradford where the source white is not D50).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "color.h"

namespace dapple::color {

// Transfer curve of one channel: encoded [0,1] -> linear.
struct Trc {
  enum Kind { Srgb, Gamma, Bt709, Linear, Table, Param } kind = Srgb;
  float gamma = 2.2f;
  std::vector<float> table;  // Table: samples of the curve over [0,1]
  float p[7] = {1, 1, 0, 0, 0, 0, 0};  // Param: g a b c d e f
  int paramType = 0;                    // ICC parametricCurveType function 0..4
  float eval(float x) const;
};

struct SourceSpace {
  std::string name = "sRGB";
  bool isSrgb = true;  // exactly the engine's historic sRGB path (matrix and curve): output is bit-identical to it
  bool fallback = false;  // identification failed: sRGB assumed
  std::string note;       // one log line when something is worth saying (non-sRGB space, fallback), else empty
  Trc trc[3];
  Mat3 toProPhoto = srgbToProPhoto();
  // Tables [3][n] for n = 256 (8 bit) or 65536 (16 bit): encoded code value -> linear.
  std::vector<float> lut(int channel, int n) const;
};

// What a container (PNG chunks / JPEG segments) says about its colour.
struct ColourTags {
  bool hasCicp = false;
  int cicpPrimaries = 2, cicpTransfer = 2, cicpMatrix = 0, cicpFullRange = 1;
  bool hasSrgbChunk = false;
  double gamma = -1;  // gAMA / 100000 (file gamma, i.e. 0.45455 for sRGB-like)
  bool hasChrm = false;
  double chrm[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // white x y, red x y, green x y, blue x y
  std::vector<std::uint8_t> icc;               // raw ICC profile (decompressed)
};

SourceSpace srgbSpace();
// Tags first, then the profile, else sRGB (fallback = true when a tag or profile was present but not usable).
SourceSpace resolveSourceSpace(const ColourTags& tags);
// A matrix / TRC RGB profile. Returns false with a reason when it is not one.
bool spaceFromIcc(const std::uint8_t* data, std::size_t size, SourceSpace& out, std::string& why);

}  // namespace dapple::color
