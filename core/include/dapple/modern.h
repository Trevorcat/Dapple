#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace dapple {

// The "modern" rendering family (docs/engine/pipeline-architecture.md section 37): digital renderings that use the whole tonal
// range of the display. A photo's rendering base is either ONE film stock or ONE modern rendering, chosen in the same selector:
// EditParams::filmStock (JSON key "film") takes the ids below besides the stock ids; EditParams::filmStrength (0..100) is the
// strength (0 = the standard rendering, 100 = the full rendering). The film code never sees these ids (they are no stock).
enum class ModernKind { None = 0, Clean, Vivid, Soft };

ModernKind modernKind(const std::string& filmStockId);  // "modern_clean" / "modern_vivid" / "modern_soft", else None
inline bool isModernId(const std::string& id) { return modernKind(id) != ModernKind::None; }

struct ModernInfo {
  const char* id;
  const char* name;  // tong tou / xian ming / rou he (Chinese UI names)
  const char* tags;
};
const std::array<ModernInfo, 3>& modernRenderings();
std::string modernJson();  // {"modern":[{"id","name","tags"}...]}

// What distinguishes the three renderings (one model, a few numbers). All are the values at strength 100.
struct ModernRecipe {
  float contrast;   // slope of the tone curve at mid-grey, relative to the end-point-stretched identity (>= 1)
  float chroma;     // chroma gain of mid-tones (1 = none)
  float blueDeep;   // OKLab L drop of saturated blue / cyan (deep, not neon)
  float skinGuard;  // 0..1, how much of the chroma gain is withheld in the skin window
  float clarity;    // added to EditParams::clarity (local contrast), -100..100 units
  float texture;    // added to EditParams::texture
  float hdr;        // minimum EditParams::hdr (scene-adaptive base compression, only acts when the scene range needs it)
  float lift;       // OKLab L added in the mid-tones (airy rendering), negative = deeper
  float tintMag;    // OKLab magnitude of the highlight bias, applied to near-neutral highlights only (clean / warm whites)
  float tintHue;    // its hue angle in degrees
};
const ModernRecipe& modernRecipe(ModernKind k);

// Robust end points of the scene luminance (the model's (a)): `Y` are post-gain, scene-linear luminance samples (ProPhoto luma,
// user exposure NOT included), `mid` the mid-grey luminance in the same domain. Returns the black and white points after the guards
// (see modern.cpp). Sorts / reorders `Y`.
struct ModernEnds {
  float black = 0.f, white = 1.f;
  float p50 = 0.f;
  bool stretchLimited = false;  // the low-contrast guard moved the end points away from the data
  float span = 1.f;             // encoded span (white - black) after the guard
};
ModernEnds modernEnds(float* Y, std::size_t n, float mid);

// Where the mass of the picture is (section 37): E = encoded luminance samples (after the user's tone edits), pb / pw the encoded end
// points. wHi: a large share of the picture in the upper tones (t > 0.72 of the end point range), wLo: in the lower tones (t < 0.22);
// gap: a low-saturation scene whose middle (t 0.3 .. 0.7) is empty (fog, snow, overcast): 0 .. 1 each. S = saturation proxy per sample.
struct ModernDist {
  float wHi = 0.f, wLo = 0.f, gap = 0.f;
};
ModernDist modernDistribution(const float* E, const float* S, std::size_t n, float pb, float pw);

// The luminance tone curve in encoded display units: t = encoded scene position between the end points (0..1), pivot (tm, ym).
// shoulderK 0..1: how much of the shoulder lift is kept (0 = the upper piece is straight; used when a large share of the picture is in the upper tones).
float modernCurve(float t, float tm, float ym, float contrast, float shoulderK = 1.f);

// sRGB OETF / inverse (x >= 0).
float modernEncode(float lin);
float modernDecode(float enc);

}  // namespace dapple
