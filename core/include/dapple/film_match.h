#pragma once
// Film exposure that keeps the photo's key.
//
// The spectral film model puts an 18 % grey scene at its own reference lightness, so a bright (high-key) photo comes out
// of any stock a lot darker than the develop pipeline renders it (measured on the CLI: L* 72-78 -> 46-48 for every stock).
// matchFilmExposure() finds the EV to add on top of `exposure + filmExposure` (EditParams::filmKeyEv) so that the median
// lightness (L*) of the film render equals that of the normal develop render of the same edit on the 256 px proxy. The
// film's own tone curve is untouched, so highlight roll-off and contrast character stay the stock's: only its exposure moves.
// The user's `filmExposure` stays an offset on top of the matched exposure.
#include <functional>
#include <string>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"

namespace dapple {

struct FilmMatchInfo {
  float ev = 0.f;          // result
  float targetL = 0.f;     // median L* of the normal render (the proxy, spatial stages off)
  float filmL = 0.f;       // median L* of the film render at `ev`
  float filmL0 = 0.f;      // ... and at 0 EV (what an unmatched film gives)
  int evals = 0;           // film evaluations on the proxy
};

// Median CIE L* of an sRGB-encoded float image (BT.709 luminance of the linearised values).
float medianLightness(const ImageF& srgbEncoded);

// The EV (|ev| <= 4) at which filmL(ev), the median lightness of a film render exposed ev stops up, equals `target`. filmL is
// monotone; 2-5 evaluations (secant from a nominal 14 L*/EV slope, tolerance 0.35 L*).
float solveFilmKeyEv(const std::function<float(float)>& filmL, float target, FilmMatchInfo* info = nullptr);

// Everything the match depends on, as a string: the edit minus the film's user offset, strength, grain and halation, minus
// the neighbourhood stages (the match is a function of the tonal pipeline and the stock/paper/print filtration only).
// Empty when no film is active. Equal keys -> equal EV for the same proxy.
std::string filmMatchKey(const EditParams& p);

// `proxy`: the <= 256 px copy of the source (any size works; the cost is a few film evaluations of that frame).
// Returns 0 when the edit has no active film. Never throws, never returns a non-finite value (clamped to +-4 EV).
float matchFilmExposure(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy,
                        FilmMatchInfo* info = nullptr);

}  // namespace dapple
