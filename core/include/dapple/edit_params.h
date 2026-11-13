#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "dapple/result.h"

namespace dapple {

// A curve control point, both coordinates in [0, 255] like Lightroom.
using CurvePoint = std::pair<float, float>;

enum HslBand { kRed, kOrange, kYellow, kGreen, kAqua, kBlue, kPurple, kMagenta, kHslBandCount };

struct HslAdjust {
  float hue = 0.f;         // -100..100
  float saturation = 0.f;  // -100..100
  float luminance = 0.f;   // -100..100
};

struct ToneZoneGrade {
  float hue = 0.f;         // 0..360 degrees
  float saturation = 0.f;  // 0..100 (0 = this zone is not tinted)
  float luminance = 0.f;   // -100..100
};

// The single, renderer-independent description of an edit. Slider ranges and
// semantics deliberately mirror Lightroom/Camera Raw so presets map 1:1 and an
// AI model can be given the same vocabulary photographers already know.
// Every value defaults to "no change".
struct EditParams {
  // White balance relative to as-shot.
  float temperature = 0.f;  // -100 (cooler) .. 100 (warmer)
  float tint = 0.f;         // -100 (greener) .. 100 (more magenta)

  // Tone
  float exposure = 0.f;    // EV, -5..5
  float contrast = 0.f;    // -100..100
  float highlights = 0.f;  // -100..100
  float shadows = 0.f;     // -100..100
  float whites = 0.f;      // -100..100
  float blacks = 0.f;      // -100..100
  float hdr = 0.f;         // 0..100 single-image HDR (engine only: shadows lifted / highlights held on the scene-linear base layer, core/src/hdr_field.h)

  // Parametric curve (Lightroom "Region" curve)
  float parametricShadows = 0.f;     // -100..100
  float parametricDarks = 0.f;
  float parametricLights = 0.f;
  float parametricHighlights = 0.f;
  float parametricShadowSplit = 25.f;  // 0..100
  float parametricMidtoneSplit = 50.f;
  float parametricHighlightSplit = 75.f;

  // Parametric curves of the individual channels [R,G,B][shadows, darks, lights, highlights]
  // (the master `parametric*` above is the RGB curve). Same bell shapes and splits.
  std::array<std::array<float, 4>, 3> channelParametric{};

  // Point curves; empty == identity
  std::vector<CurvePoint> toneCurve;
  std::vector<CurvePoint> toneCurveRed;
  std::vector<CurvePoint> toneCurveGreen;
  std::vector<CurvePoint> toneCurveBlue;

  // Colour
  float vibrance = 0.f;    // -100..100
  float saturation = 0.f;  // -100..100
  std::array<HslAdjust, kHslBandCount> hsl{};

  // Colour grading / split toning
  ToneZoneGrade gradeShadows;
  ToneZoneGrade gradeMidtones;
  ToneZoneGrade gradeHighlights;
  ToneZoneGrade gradeGlobal;  // applied to every pixel
  float gradeBlend = 50.f;    // 0..100 smoothness of the zone cross-over
  float gradeBalance = 0.f;   // -100..100, positive = highlights weigh more

  // Tonal class (see tone.h). Moves the image's tonal distribution towards one of the
  // nine classes key{low,mid,high} x range{short,mid,long} with a tone curve in perceptual
  // space that protects deep shadows and highlights. Empty target = the photo's own
  // (closest) class, i.e. a gentle "enhance in its own character".
  std::string toneTarget;   // "" or e.g. "low-long", "mid-mid", "high-short"
  float toneStrength = 0.f; // 0..100, 0 = off

  // Colour style ("look", see look.h): one of nine engine-side recipes, blended in by
  // lookStrength (0 = no style at all) and steered by a bounded 2D colour pad
  // (x: pale -> rich, y: warm -> cool). Without a palette override the style's own default
  // point is used.
  std::string look = "std";
  float lookStrength = 100.f;  // 0..100
  bool hasPalette = false;
  float paletteX = 0.5f, paletteY = 0.5f;  // each [0.04, 0.96]; used when hasPalette

  // Effects (local-contrast ones act on luminance with an edge-aware base layer)
  float clarity = 0.f;   // -100..100 mid-frequency local contrast
  float texture = 0.f;   // -100..100 fine detail contrast
  float dehaze = 0.f;    // -100..100 (negative adds haze)
  float vignette = 0.f;  // -100..100 (negative darkens corners; follows the crop)
  float grain = 0.f;     // 0..100 film grain

  // Detail. Defaults are the "develop" baseline for RAW input (sharpen 40, chroma NR 25);
  // for display-referred input (camera JPEG, already sharpened) these defaults are the
  // no-op level and only the excess is applied.
  float sharpen = 40.f;       // 0..150
  float sharpRadius = 10.f;   // 5..30 (tenths of a pixel at 2048 px, 0.5..3.0)
  float nrLuma = 0.f;         // 0..100
  float nrColor = 25.f;       // 0..100

  // Geometry. Order: rotate 90 -> straighten (rotates, then the crop is the largest box of the
  // chosen ratio that has no empty corners) -> flip (final, horizontal mirror).
  std::string crop = "orig";  // orig, 1:1, 4:5, 3:4, 16:9, 9:16
  float straighten = 0.f;     // -45..45 degrees, positive = image turns clockwise
  int rotate = 0;             // 0, 90, 180, 270 (clockwise)
  bool flip = false;

  // Spectral film simulation (film.h, docs/film/README.md). With a stock set, the film replaces the tone, colour
  // and look stages (it takes linear ProPhoto after white balance and exposure, outputs display sRGB); curves,
  // LUT, vignette, crop and the detail stages still apply. "" = off: the output is then identical to a build
  // without the simulator.
  std::string filmStock;       // stock id (FilmLibrary names, e.g. "kodak_portra_400"), "" = off
  std::string filmPaper;       // print paper id; "" = default for the stock; "none" = scan the film itself
  float filmStrength = 100.f;  // 0..100, blend of the finished film render against the normal pipeline
  float filmExposure = 0.f;    // EV, -3..3, exposure at the camera on top of `exposure` (print not re-balanced)
  float filmWarmth = 0.f;      // -100..100, print filtration (yellow): positive = warmer print
  float filmTint = 0.f;        // -100..100, print filtration (magenta): positive = more magenta
  float filmGrain = 100.f;     // 0..200, grain amount (100 = the stock's reference grain, 0 = none, 200 = twice the amplitude)
  float filmHalation = 100.f;  // 0..200, halation amount (100 = reference; the scatter mix saturates at 100)
  // Internal, not serialised and not a slider: EV the film is exposed on top of `exposure + filmExposure` so that its
  // mid-tone lightness equals the normal develop render's (matchFilmExposure, film_match.h). Set by the C layer / CLI.
  float filmKeyEv = 0.f;
  // Internal, not serialised: print exposure in stops (positive = brighter print), the darkroom's brightness decision
  // (film_match.h). Paper exposure 2^-EV; for a film with no print (slides) the scan / projection lamp gain 2^EV.
  float filmPrintEv = 0.f;
  // Automatic film exposure overrides (docs/engine/pipeline-architecture.md section 18), normalised -1..1, 0 = automatic:
  // filmShoot moves the film EV between the under / over edge of the stock's usable range, filmPrint the print / scan exposure
  // between a dark and a light median, filmBlack the paper black (-1 matte, 0 true Dmax, +1 display black), filmLatitude the
  // in-range fraction the stock is rescued to (0.55 .. 0.80 .. 0.98).
  float filmShoot = 0.f, filmPrint = 0.f, filmBlack = 0.f, filmLatitude = 0.f;
  // Film white balance (胶片白平衡), 0..1: 0 = the digital white balance (the scene white reaches the film as its own reference white), 1 =
  // the stock's own balance sees the scene's real light (degree of adaptation D = 1 - filmWb).
  float filmWb = 0.f;
  // Internal, not serialised: CIE xy of the scene's as-shot illuminant (the C layer sets it from the RAW's white balance; 0 = unknown).
  float filmSceneX = 0.f, filmSceneY = 0.f;

  // Optional creative look (3D LUT, .cube); applied in sRGB at the end.
  std::string lutPath;
  float lutAmount = 100.f;  // 0..100

  // Brings every value into its legal range (NaN -> default, unknown crop -> "orig", rotate to a
  // multiple of 90, unknown look -> "std"). toJson/fromJson and the renderer call it.
  void clamp();

  // Combine two edits: numeric sliders add (as offsets from their defaults), curves/LUT/crop/look
  // from `delta` win when set.
  // Used to stack "preset look" + "per-photo automatic correction".
  EditParams& operator+=(const EditParams& delta);
};

// Lossless JSON (subset) for debugging, persistence and as the schema an AI
// model is asked to fill in.
std::string toJson(const EditParams& params);

// Only the keys that differ from EditParams{} ("diff vs default"); fromJson of it restores `params`.
std::string toJsonDiff(const EditParams& params);

// JSON describing every slider key: [{"key","min","max","default","group"} ...]. Used by the C
// API (dapple_params_schema_json) so a UI can verify it speaks the same vocabulary.
std::string paramsSchemaJson();

// Inverse of toJson. The flat contract keys (docs/design/requirements.md 6.3: "exposure" in UI
// units = EV * 20, "temp", "hsl_red_h", "cv_rgb_H", "gr_shadows_s", "crop", "rot", "flip", ...)
// are canonical; legacy keys ("temperature", nested "hsl"/"parametric", "gradeShadows", "exposureEv")
// are still read. Values are clamped to their range. Tolerant: unknown keys are ignored, missing keys keep
// their defaults, numbers may be quoted, and {"params": {...}} (as produced by
// dapple_xmp_to_json) is unwrapped. Fails only on malformed JSON / non-object.
Result<EditParams> fromJson(const std::string& json);

const char* hslBandName(int band);

}  // namespace dapple
