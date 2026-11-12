#pragma once
// Automatic film exposure, split into the stages of a film workflow (docs/engine/pipeline-architecture.md section 18):
//   A. film (camera) exposure  EditParams::filmKeyEv   - latitude placement only (scene percentiles against the stock's
//      toe / shoulder); no reference to the digital render's brightness.
//   B. print exposure          EditParams::filmPrintEv - the brightness decision: solved so that the print's median L* equals the
//      scene's key (the develop render's median), with the target limited to the paper's usable range.
//   C. paper to screen         the existing print / scan stage; filmPrintRange() reports the paper's white / black as displayed.
// (v4 = mode Window below: A and B solved per image and stock.) The default since v5 is mode Meter: one exposure
// decision per image, shared by all stocks (stage 1 metering, stage 2 film exposure), and a print exposure that is a per
// stock + paper constant calibrated once (stage 3); DAPPLE_FILM_MATCH_V4=1 selects the v4 behaviour.
// DAPPLE_FILM_MATCH_FULL=1 = the original single full match on the film EV (v2), and
// DAPPLE_FILM_MATCH_PULL=<f> = the 70 %-pull + latitude window version (v3) for A/B.
//
// Historical background (modes Pull / Full):
//
// The spectral film model puts an 18 % grey scene at its own reference lightness, so a bright (high-key) photo comes out
// of any stock a lot darker than the develop pipeline renders it (measured on the CLI: L* 72-78 -> 46-48 for every stock).
// matchFilmExposure() finds the EV to add on top of `exposure + filmExposure` (EditParams::filmKeyEv) so that the median
// lightness (L*) of the film render equals that of the normal develop render of the same edit on the 256 px proxy. The
// film's own tone curve is untouched, so highlight roll-off and contrast character stay the stock's: only its exposure moves.
// The user's `filmExposure` stays an offset on top of the matched exposure.
//
// The full match is too much for a high-key scene (it turns light into flat grey) and for reversal film (white washes out),
// so the key is only pulled kFilmKeyPull of the way, and the result is limited to the stock's own exposure latitude: the
// window of film EVs in which the scene's shadows (p2) stay above the toe of the stock's green density curve and its
// highlights (p99.5) stay below the shoulder. See docs/engine/pipeline-architecture.md section 18.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/lut.h"

namespace dapple {

// Fraction of the median-L* match that is applied (ev_key = kFilmKeyPull * ev_match).
constexpr float kFilmKeyPull = 0.7f;
// Slope threshold of toe / shoulder, as a fraction of the stock's mid gamma; margins (log10 H) kept inside the latitude.
constexpr float kFilmEdgeSlopeFrac = 0.2f;
constexpr float kFilmToeMarginLogH = 0.15f;       // 0.5 stop
constexpr float kFilmShoulderMarginLogH = 0.15f;  // 0.5 stop (negatives)
constexpr float kFilmShoulderMarginReversalLogH = 0.05f;  // reversal film (0.15 / 0.10 / 0.05 / 0.0 compared in section 18)
// Highlight statistic: pixels at the sensor clipping point (any channel of the source >= kFilmClipLevel) are ignored by
// p99.5; when more than kFilmClipMaxFrac of the frame is clipped, p90 of all pixels is used instead.
constexpr float kFilmClipLevel = 0.98f;
constexpr float kFilmClipMaxFrac = 0.25f;
// Used when a stock's curve data cannot be evaluated.
constexpr float kFilmFallbackMaxEv = 2.f;

// Stage A: negatives are placed with the shadows (p2) kFilmNegativeBiasEv above the toe margin (the usual overexposure bias),
// never past the shoulder window; slides put the unclipped highlight at the shoulder margin.
constexpr float kFilmNegativeBiasEv = 0.5f;
// Stage B: the target key must stay inside the paper's density range: Dmin + kPaperWhiteMarginD ... Dmax - kPaperBlackMarginD.
constexpr float kPaperWhiteMarginD = 0.05f;
constexpr float kPaperBlackMarginD = 0.15f;
constexpr float kFilmMaxPrintEv = 4.f;

enum class FilmMatchMode { Meter, Window, Pull, Full };  // v5 (default), v4, v3, v2

// v5. The photographer's exposure compensation (EV_scene) is clamped to this range; reversal film is exposed kFilmReversalBiasEv
// from its box speed (negatives kFilmNegativeBiasEv); the grey card calibrating the print has this scene luminance.
constexpr float kFilmSceneEvMin = -2.f;
constexpr float kFilmSceneEvMax = 2.5f;
constexpr float kFilmReversalBiasEv = -0.3f;
constexpr float kFilmGreyCard = 0.184f;
// Nominal slope of the film + print system around mid-grey (display light grows as scene light ^ gamma; 1.25-1.5 per EV measured on the
// eight showcase stocks between -1 and +1 EV, less towards the shoulder): the compensation is the display key divided by it.
constexpr float kFilmSystemGamma = 1.15f;
constexpr float kFilmShadowFloorY = 0.004f;  // floor of a pixel's luminance in the metering average
// Per-stock usability guarantee: the share of scene pixels that must fall inside the stock's [toe, shoulder]; a stock whose range
// fits keeps the shared exposure, a narrow one is moved just enough (docs section 18). The user's film_latitude moves it.
constexpr float kFilmRangeFraction = 0.80f;
constexpr float kFilmRangeFractionMin = 0.55f, kFilmRangeFractionMax = 0.98f;  // film_latitude -1 / +1
constexpr float kFilmShootEdgeFraction = 0.55f;  // film_shoot +-1: the EV where the in-range share drops to this
constexpr float kFilmShootMaxEv = 3.f;           // ... never further than this from the automatic exposure
constexpr float kFilmPrintDarkL = 30.f, kFilmPrintLightL = 85.f;  // film_print -1 / +1: median L* relative to the paper white
constexpr float kFilmSafeMedianMin = 33.f, kFilmSafeMedianMax = 86.f;  // film_shoot never leaves this median for a slide
constexpr float kFilmMatteBlackY = 0.02f;                           // film_black -1: relative luminance of the lifted black

// Which edge of the latitude window decided the result.
enum class FilmBound { None, Toe, Shoulder, Fallback };
// Which edge of the paper's range limited the print target.
enum class PrintBound { None, PaperWhite, PaperBlack, SolverLimit };

struct FilmMatchInfo {
  float ev = 0.f;          // result (filmKeyEv)
  float evMatch = 0.f;     // the full median match (before the pull and the window)
  float evKey = 0.f;       // kFilmKeyPull * evMatch
  float evLo = 0.f, evHi = 0.f;  // latitude window (valid when latitudeOk)
  bool latitudeOk = false;       // the stock's curves gave a window
  bool conflict = false;         // evLo > evHi: the scene exceeds the latitude
  FilmBound bound = FilmBound::None;
  float clipFrac = 0.f;
  // v5: metering and per-stock diagnostics
  float meterY = 0.f;                    // averaging meter reading (before the develop exposure)
  float evMeter = 0.f;                   // exposure that puts the reading at 18.4 % grey
  float evScene = 0.f;                   // exposure compensation for the scene's key (stock independent)
  float keyL0 = 0.f;                     // intended key: median L* of the develop render at exposure 0
  float printConst = 0.f;                // the stock + paper print exposure constant
  float sharedEv = 0.f;                  // box speed + EV_scene + type convention, before the per-stock rescue
  float fracShared = 0.f, fracFinal = 0.f;  // in-range fraction at the shared / final film EV
  float fracBelowToe = 0.f, fracAboveShoulder = 0.f;  // share of the scene lost at the final EV
  float shootEv = 0.f;                   // film EV after film_shoot
  float printDelta = 0.f;                // film EV moved from the shared exposure (negatives are printed down by it)
  float stopsBelowToe = 0.f, stopsAboveShoulder = 0.f;  // p2 / p99.5 beyond the stock's toe / shoulder at the chosen EV
  float p2 = 0.f, p50 = 0.f, p995 = 0.f;  // scene-linear luminance after the develop exposure (18.4 % grey = 0.184)
  float targetL = 0.f;     // median L* of the normal render (the proxy, spatial stages off)
  float filmL = 0.f;       // median L* of the film render at `ev`
  float filmL0 = 0.f;      // ... and at 0 EV (what an unmatched film gives)
  int evals = 0;           // film evaluations on the proxy
  // stage B
  float printEv = 0.f;     // print exposure (positive = brighter print)
  float keyL = 0.f;        // the print target after the paper limit
  float printL = 0.f;      // median L* of the print at printEv
  float paperLoL = 0.f, paperHiL = 0.f;  // usable range of the paper in L*
  PrintBound printBound = PrintBound::None;
  int printEvals = 0;
};

// Both exposure stages of one edit.
struct FilmExposure {
  float filmEv = 0.f;   // EditParams::filmKeyEv
  float printEv = 0.f;  // EditParams::filmPrintEv
};

// Median CIE L* of an sRGB-encoded float image (BT.709 luminance of the linearised values).
float medianLightness(const ImageF& srgbEncoded);

// The EV (|ev| <= 4) at which filmL(ev), the median lightness of a film render exposed ev stops up, equals `target`. filmL is
// monotone; 2-5 evaluations (secant from a nominal 14 L*/EV slope, tolerance 0.35 L*).
float solveFilmKeyEv(const std::function<float(float)>& filmL, float target, FilmMatchInfo* info = nullptr);

// Exposure latitude of a stock, from its own green-channel density curve (log10 H, 0 = 18.4 % grey at film EV 0).
//   midGamma: mean |dD/dlogH| over logH in [-0.3, 0.3]; toe / shoulder: the logH below / above 0 where |slope| first falls
//   under kFilmEdgeSlopeFrac * midGamma (the end of the measured data when it never does).
struct FilmLatitude {
  bool ok = false;
  bool reversal = false;
  float midGamma = 0.f, toe = 0.f, shoulder = 0.f;
  float stops() const { return (shoulder - toe) / 0.30103f; }
};
FilmLatitude filmLatitude(const film::FilmStock& s);

// Scene-linear luminance percentiles (ProPhoto Y of v * gain): shadow p2, median, highlight p99.5.
struct SceneStats {
  float p2 = 0.f, p50 = 0.f, p995 = 0.f;  // p995: highlight statistic (see kFilmClipLevel); p90 of all when mostly clipped
  float clipFrac = 0.f;                   // fraction of pixels at the sensor clipping point
  float logMean = 0.f;                    // meter reading: median luminance of the counted pixels
  std::vector<float> logH0;               // sorted log10(Y / 0.184) of the pixels counted (unclipped; all when mostly clipped)
};
SceneStats sceneLumaStats(const ImageF& linear, const float gain[3]);

// Latitude window of film EVs for the scene: p99.5 at or below shoulder - margin, p2 at or above toe + margin
// (log10 H = log10(Y / 0.184) + EV * log10(2)). Returns false when the stock or the scene has no usable data.
bool filmExposureWindow(const FilmLatitude& lat, const SceneStats& st, float& evLo, float& evHi,
                        float shoulderMarginReversal = kFilmShoulderMarginReversalLogH);

struct FilmMatchOptions {
  FilmMatchMode mode = FilmMatchMode::Meter;
  float pull = kFilmKeyPull;  // mode Pull
  bool latitude = true;       // mode Pull / Full: apply the window (Full sets pull 1, latitude false)
  float shoulderMarginReversal = kFilmShoulderMarginReversalLogH;
  float negativeBiasEv = kFilmNegativeBiasEv;  // env DAPPLE_FILM_MATCH_NEG_BIAS (experiments)
  // env: DAPPLE_FILM_MATCH_FULL=1, and for experiments DAPPLE_FILM_MATCH_PULL=<f>, DAPPLE_FILM_MATCH_REV_MARGIN=<logH>
  // DAPPLE_FILM_MATCH_FULL=1: the old behaviour (100 % match, no window) for A/B; DAPPLE_FILM_MATCH_PULL=<f>: mode Pull
  static FilmMatchOptions fromEnv();
};

// v5 stage 1: the averaging meter's exposure (puts `st0.logMean`, measured before the develop exposure, at 18.4 % grey) and the
// compensation for the scene's intended key: EV_scene = clamp(log2(Y(keyL) / 0.184), kFilmSceneEvMin, kFilmSceneEvMax), keyL =
// median L* of the develop render at exposure 0. Neither depends on the stock.
float meterEv(const SceneStats& st0);
float sceneKeyEv(float keyL);
// v5 diagnostics: stops of the scene (p2 below the toe, p99.5 above the shoulder) at film EV `ev` on top of the stats' gain.
void scenePlacement(const FilmLatitude& lat, const SceneStats& st, float ev, float* belowToe, float* aboveShoulder);
// v5 stage 3: the print exposure (EV, positive = brighter) at which an 18.4 % grey card exposed at box speed + the type convention
// prints to L* 50 relative to the paper white (paper "none": scan lamp). Calibrated once per (stock, paper), cached, image independent.
float filmPrintConstant(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& film, const std::string& paper,
                        float deltaEv = 0.f);
// The same for a negative whose film EV was moved by `deltaEv` from the shared exposure: the lab prints a denser / thinner
// negative down / up (calibrated on a 0.5 EV grid and interpolated).
float filmPrintConstantAt(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& film, const std::string& paper,
                          float deltaEv);

// Share of the scene's pixels inside the stock's [toe, shoulder] at film EV `ev` (on top of the stats' own gain).
float inRangeFraction(const SceneStats& st, const FilmLatitude& lat, float ev);
// The in-range fraction target for film_latitude l (-1..1): 0.55 .. 0.80 .. 0.98.
float rangeFractionFor(float latitude);

// Everything v5 needs from a frame, stock independent except the stock itself.
struct MeterInputs {
  std::shared_ptr<const film::FilmLibrary> lib;
  std::string film, paper;  // paper resolved ("none" = scan)
  SceneStats st0, stFull;   // before / after the develop exposure (the diagnostics and the range use stFull)
  float keyL0 = 0.f;        // median L* of the develop render at exposure 0
  float shoot = 0.f, print = 0.f, latitude = 0.f;  // EditParams::filmShoot / filmPrint / filmLatitude
  float negativeBiasEv = kFilmNegativeBiasEv;
  // median L* of the film render at (film EV, print EV); only needed for film_print != 0
  std::function<float(float, float)> medianL;
};
// v5 (modes Meter): shared metering, per-stock usability rescue, film_shoot, print constant (re-calibrated for a moved negative),
// film_print. Never throws; fills `info`.
FilmExposure solveMeter(const MeterInputs& in, FilmMatchInfo* info = nullptr);

// What the (future) control pad labels itself with for the current image and stock: actual values at -1, 0, +1.
struct FilmPadInfo {
  bool ok = false;
  float shootEv[3] = {0, 0, 0};       // film EV at film_shoot -1, 0, +1
  float printL[3] = {0, 0, 0};        // median L* of the print at film_print -1, 0, +1 (needs MeterInputs::medianL)
  float printEv[3] = {0, 0, 0};
  float blackL[3] = {0, 0, 0};        // L* of the paper black at film_black -1, 0, +1
  float latitude[3] = {0, 0, 0};      // in-range fraction at film_latitude -1, 0, +1
  float sharedEv = 0.f, finalEv = 0.f, fractionAtFinal = 0.f;
  float whiteL = 0.f;
};
FilmPadInfo filmPadInfo(const MeterInputs& in);
// The same for an edit (develop pipeline + 256 px proxy); `ok` false without an active film or in a legacy mode.
FilmPadInfo filmPadFor(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy);


// The final filmKeyEv from the full match: pull, then the latitude window of `stock` (null = unknown). Fills `info`.
// Negative stocks keep the shadows when the scene exceeds the latitude (highlights roll off), reversal stocks keep the
// highlights.
float resolveFilmKeyEv(const film::FilmStock* stock, const SceneStats& st, float evMatch, const FilmMatchOptions& opt,
                       FilmMatchInfo* info = nullptr);

// Stage A: the film EV from the scene's percentiles and the stock's latitude only (null / unusable curves: 0, logged once).
float chooseFilmEv(const film::FilmStock* stock, const SceneStats& st, const FilmMatchOptions& opt, FilmMatchInfo* info = nullptr);

// Paper (or scan) as displayed: L* of the paper white (Dmin) and black (Dmax) reached by an over / under exposed patch of the
// stock printed with extreme print exposures, and the usable target range inside the density margins. `paper` "none" = scan the
// film itself. Cached per (stock, paper); never throws; ok = false when the stock / paper is unknown.
struct FilmPrintRange {
  bool ok = false;
  float lWhite = 0.f, lBlack = 0.f;  // L* of paper white / black
  float yWhite = 0.f, yBlack = 0.f;  // relative luminance (1 = perfect white diffuser under the viewing light)
  float loL = 0.f, hiL = 0.f;        // usable range: black raised by kPaperBlackMarginD, white lowered by kPaperWhiteMarginD
  float maxPrintEv = kFilmMaxPrintEv;  // scan / projection (no print): the lamp may not lift the clear film above that white
};
FilmPrintRange filmPrintRange(const std::shared_ptr<const film::FilmLibrary>& lib, const std::string& film, const std::string& paper);

// Stage B: the print EV (|ev| <= kFilmMaxPrintEv) at which printL(ev), the median L* of the print, equals `target` clamped into
// `range`. Fills printEv / keyL / printL / paper range / printBound / printEvals of `info`.
float solvePrintEv(const std::function<float(float)>& printL, float target, const FilmPrintRange& range, FilmMatchInfo* info = nullptr);

// The paper-to-screen mapping of stage C (scanner white / black correction of the film model: the print's white and black
// reference map to display white / black, the print exposure keeps mid-grey where it was) is on in the develop pipeline when
// the Split match is in use. Off with DAPPLE_FILM_MATCH_OFF, DAPPLE_FILM_MATCH_FULL / _PULL (the previous behaviour) and
// DAPPLE_FILM_PAPER_MAP_OFF.
bool filmPaperMappingEnabled();
// Sets the scanner white / black correction with the paper (clear film) as reference when the mapping is enabled.
// v5 keeps the paper's black at its true relative luminance (white scaling only); v4 (mode Window) also stretched the black.
bool filmPaperMapStretchesBlack();
inline void applyFilmPaperMapping(film::FilmParams& p, float blackMix = 0.f) {
  const bool on = filmPaperMappingEnabled();
  p.scanner.whiteCorrection = p.scanner.paperReference = on;
  p.scanner.blackCorrection = on && filmPaperMapStretchesBlack();
  p.scanner.blackMix = on ? double(blackMix < -1.f ? -1.f : blackMix > 1.f ? 1.f : blackMix) : 0.0;
}

// Everything the match depends on, as a string: the edit minus the film's user offset, strength, grain and halation, minus
// the neighbourhood stages (the match is a function of the tonal pipeline and the stock/paper/print filtration only).
// Empty when no film is active. Equal keys -> equal EV for the same proxy.
std::string filmMatchKey(const EditParams& p);

// `proxy`: the <= 256 px copy of the source (any size works; the cost is a few film evaluations of that frame).
// Returns {0, 0} when the edit has no active film. Never throws, never returns a non-finite value (clamped to +-4 EV).
// `opt` null = FilmMatchOptions::fromEnv().
FilmExposure matchFilmExposure(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy,
                        FilmMatchInfo* info = nullptr, const FilmMatchOptions* opt = nullptr);

}  // namespace dapple
