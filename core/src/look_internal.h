#pragma once
// Internal: engine-side recipes of the nine colour styles (see dapple/look.h).

namespace dapple {

// A style acts in OKLab on top of the user's colour edits. All parts are scaled by lookStrength
// (0 = identity). Tints are chroma offsets (a, b) of the given OKLab hue, faded out towards black.
struct LookRecipe {
  float satMul = 1.f;         // chroma multiplier
  float contrast = 1.f;       // slope at mid grey of a smooth S curve on OKLab L (ends fixed)
  float dL = 0.f;             // OKLab L offset
  float tintMag = 0.f, tintHue = 60.f;    // overall tint (the "sepia" of the prototype)
  float shadowMag = 0.f, shadowHue = 0.f; // split tone: shadows
  float highMag = 0.f, highHue = 0.f;     // split tone: highlights
  float hueRotDeg = 0.f;      // rotation of all hues (OKLab degrees)
  float gray = 0.f;           // 1 = fully desaturated
  float fade = 0.f;           // lifted black point in display-referred space (0..1)
  float grain = 0.f;          // film grain added (0..100 scale)
};

const LookRecipe& lookRecipe(int index);

}  // namespace dapple
