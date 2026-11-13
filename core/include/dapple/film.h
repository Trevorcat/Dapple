#pragma once
// Spectral film simulation: scene-linear ProPhoto RGB -> film exposure -> developed density ->
// enlarger + paper (or direct scan) -> display sRGB. Architecture and GPU mapping:
// docs/film/architecture.md. Functional spec: docs/film/spec/.
//
// Two layers:
//   bake  - everything that depends only on parameters (spectral integrals, curves, enlarger balance,
//           print + scan path, CAM16 gamut compression) is baked into compact float tables, multithreaded,
//           and cached by a hash of the non-spatial parameters;
//   run   - per-pixel kernels that only do table lookups and a few FMAs (film_kernels_impl.inc).

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dapple/image.h"

namespace dapple::film {

constexpr int kSpectralSamples = 81;  // 380..780 nm, 5 nm step
constexpr int kCurveSamples = 256;    // log-exposure -3..4

// ---- data ---------------------------------------------------------------------------------------------

// One film stock or print paper (profile). NaN marks "not measured" (treated as zero light).
struct FilmStock {
  std::string stock, name, type, support, stage, use, antihalation, referenceIlluminant, viewingIlluminant;
  std::string targetPrint;  // profile info.target_print: the print paper / film this stock is made to be printed on ("" = none, slides)
  std::string copyright, license;  // attribution carried through from the source profile
  bool positive() const { return type == "positive"; }
  bool isPaper() const { return support == "paper"; }
  bool isPrintMedium() const { return stage == "printing"; }

  std::vector<float> logSensitivity;   // 81 x 3
  std::vector<float> windowParams;     // 4 (films) or empty
  std::vector<float> surfaceParams;    // 3 x 15 (films) or empty
  std::vector<float> channelDensity;   // 81 x 3
  std::vector<float> baseDensity;      // 81
  std::vector<float> logExposure;      // 256
  std::vector<float> densityCurves;    // 256 x 3
  std::vector<float> densityCurvesLayers;  // 256 x 3 x 3 (grain model)
  std::vector<float> modelCenters, modelAmplitudes, modelSigmas;  // 3 x 3 each ("cdfs" model)
};

// Exact-size and finiteness validation of a profile (used by the JSON and pack loaders).
bool validateStock(const FilmStock& s, std::string* err = nullptr);

// Spectral upsampling table: per tc node a smooth bounded sigmoid-polynomial emission spectrum.
// coef[(j*n + i)*4 + {0,1,2,3}] = a0, a1, a2, norm (i along tc0, j along tc1).
struct UpsampleTable {
  int n = 0;
  std::vector<float> coef;
};

// Largest in-gamut CAM16-UCS chroma per (J', hue) for sRGB output. data[jIndex * nH + hIndex].
struct CmaxTable {
  int nJ = 64, nH = 720;
  std::vector<float> data;
};

using PrintPaper = FilmStock;  // papers and print films share the profile structure

class FilmLibrary {
 public:
  // Binary film pack (.dfp) written by `dapple film-pack` / writePack().
  static std::shared_ptr<FilmLibrary> loadPack(const std::filesystem::path& path, std::string* err = nullptr);
  static std::shared_ptr<FilmLibrary> loadPackMemory(const void* data, std::size_t size, std::string* err = nullptr);
  // Directory of profile JSON files. Spectral upsampling and gamut tables are generated (~1.5 s).
  static std::shared_ptr<FilmLibrary> loadJsonDir(const std::filesystem::path& dir, std::string* err = nullptr);
  // Single profile JSON (desktop tests).
  static bool loadProfileJson(const std::filesystem::path& file, FilmStock& out, std::string* err = nullptr);

  bool writePack(const std::filesystem::path& path, std::string* err = nullptr) const;

  const FilmStock* find(const std::string& name) const;
  std::vector<std::string> names() const;
  const UpsampleTable& upsample() const { return upsample_; }
  const CmaxTable& cmax() const { return cmax_; }
  const std::string& attribution() const { return attribution_; }

  void add(FilmStock s);
  void setTables(UpsampleTable u, CmaxTable c) { upsample_ = std::move(u); cmax_ = std::move(c); }
  void setAttribution(std::string a) { attribution_ = std::move(a); }

 private:
  std::unordered_map<std::string, FilmStock> stocks_;
  std::vector<std::string> order_;
  UpsampleTable upsample_;
  CmaxTable cmax_;
  std::string attribution_;
};

// ---- parameters (names and defaults follow the reference schema; see spec 01 s4) -----------------------

struct DiffusionFilterParams {
  bool active = false;
  std::string filterFamily = "black_pro_mist";  // glimmerglass, black_pro_mist, pro_mist, cinebloom
  double strength = 0.5, spatialScale = 1.0, haloWarmth = 0.0;
  double coreIntensity = 1.0, coreSize = 1.0, haloIntensity = 1.0, haloSize = 1.0, bloomIntensity = 1.0,
         bloomSize = 1.0;
};

// Spatial / stochastic effects (grain, halation / scatter, DIR diffusion, glare, diffusion filters, blurs). They are
// implemented in the built-in tiled spatial path (core/src/film_spatial*.cpp) and run automatically when any of them
// is active. FilmSpatialHooks below is only the legacy staged path (full-frame callbacks); when a hook is set the
// built-in spatial path is bypassed. The point-wise bake reads only `active` / `percent` of the print glare.
struct GrainParams {
  bool active = true, sublayersActive = true;
  double particleAreaUm2 = 0.2;
  std::array<double, 3> particleScale{1.6, 1.6, 3.2}, particleScaleLayers{2.0, 1.0, 0.5};
  std::array<double, 3> densityMin{0.03, 0.03, 0.03}, uniformity{0.97, 0.99, 0.97};
  double blur = 0.65, blurDyeCloudsUm = 1.0;
  std::array<double, 2> microStructure{0.2, 30.0};
  int nSubLayers = 1;
};
struct HalationParams {
  bool active = true;
  double scatterAmount = 1, scatterSpatialScale = 1, halationAmount = 1, halationSpatialScale = 1;
  std::array<double, 3> scatterCoreUm{2.2, 2.0, 1.6}, scatterTailUm{9.3, 9.7, 9.1},
      scatterTailWeight{0.78, 0.65, 0.67};
  double boostEv = 0, boostRange = 0.3, protectEv = 4.0;
  std::array<double, 3> halationStrength{0.05, 0.015, 0.0}, halationFirstSigmaUm{65, 65, 65};
  int halationNBounces = 3;
  double halationBounceDecay = 0.5;
  bool halationRenormalize = true;
};
struct DirCouplersParams {
  bool active = true;
  double amount = 1.0, inhibitionSamelayer = 1.0, inhibitionInterlayer = 1.0;
  std::array<double, 3> gammaSamelayerRgb{0.336, 0.319, 0.273};
  std::array<double, 2> gammaInterlayerRToGb{0.353, 0.302}, gammaInterlayerGToRb{0.154, 0.353},
      gammaInterlayerBToRg{0.168, 0.226};
  double diffusionSizeUm = 20.0, diffusionTailUm = 200.0, diffusionTailWeight = 0.06;
};
struct GlareParams {
  bool active = true;
  double percent = 0.03, roughness = 0.7, blur = 0.5;
};
struct FilmRenderParams {
  double densityCurveGamma = 1.0;
  std::array<double, 3> densityCurveGammaRgb{1.0, 1.0, 1.0};  // per-layer override (multiplies the scalar)
  GrainParams grain;
  HalationParams halation;
  DirCouplersParams dirCouplers;
  GlareParams glare;  // unused for film
};
struct PrintCurvesMorphParams {
  bool active = false;
  double gammaFactor = 1, gammaFactorFast = 1, gammaFactorSlow = 1;
  double gammaFactorRed = 1, gammaFactorGreen = 1, gammaFactorBlue = 1;
};
struct PrintRenderParams {
  GlareParams glare;
  PrintCurvesMorphParams densityCurvesMorph;
};
struct CameraParams {
  double exposureCompensationEv = 0.0;
  bool autoExposure = true;
  std::string autoExposureMethod = "center_weighted";
  double lensBlurUm = 0.0, filmFormatMm = 35.0;
  // Film white balance (EditParams::filmWb, CLI --adapt / --scene-cct): the scene's illuminant as assumed by the current white balance (CIE xy,
  // 0 = unknown = full adaptation) and the degree of adaptation D (CAT16) of the film's input: 1 = the scene white reaches the film as its own
  // reference white (the model's behaviour, digital white balance), 0 = the scene illuminant reaches the film unadapted (a daylight stock
  // under tungsten goes orange). D = 1 - filmWb.
  double sceneWhiteX = 0.0, sceneWhiteY = 0.0, adaptationDegree = 1.0;
  std::array<double, 3> filterUv{0.0, 410.0, 8.0}, filterIr{0.0, 675.0, 15.0};  // (amplitude, nm, width nm)
  DiffusionFilterParams diffusionFilter;
};
struct EnlargerParams {
  std::string illuminant = "TH-KG3";
  double printExposure = 1.0;
  bool printExposureCompensation = true, normalizePrintExposure = true;
  double yFilterShift = 0, mFilterShift = 0;
  double yFilterNeutral = 55, mFilterNeutral = 65, cFilterNeutral = 0;  // replaced by the solved balance
  DiffusionFilterParams diffusionFilter;
  double preflashExposure = 0, preflashYFilterShift = 0, preflashMFilterShift = 0;
};
struct ScannerParams {
  double lensBlur = 0;
  // Black / white print-luminance correction (spec 02 s3.10); implemented for the print path only.
  bool whiteCorrection = false, blackCorrection = false;
  double whiteLevel = 0.98, blackLevel = 0.01;
  // true: the white / black references are the paper's own Dmin / Dmax (a slide: the clear / densest film) instead of the print
  // of the film's extreme densities at the standard exposure (spec). Used by the develop pipeline's paper-to-screen mapping.
  bool paperReference = false;
  // With paperReference: where the paper's black goes, -1..1. 0 = its true luminance relative to the paper white (white scaling only),
  // +1 = pinned to display black (scanner.blackLevel), -1 = lifted to a matte black (relative luminance 0.02, about L* 16).
  double blackMix = 0;
  std::array<double, 2> unsharpMask{0.7, 0.7};  // (sigma px, amount)
};
struct KneeParams {
  double threshold = 0.0, limit = 1.0, power = 6.0;
};
struct IoParams {
  bool inputGamutCompress = true;  // algorithm "xy"
  KneeParams inputKnee;
  bool outputGamutCompress = true;  // algorithm "cam16ucs" (the only one implemented)
  bool scanFilm = false;  // true: scan the developed film itself (no print)
};
struct SettingsParams {
  bool applyWindow = true, applySurface = false;
  bool applyStocksSpecifics = true;
  bool neutralPrintFilters = true;  // solve the enlarger C/M/Y for a neutral 18.4 % grey
  int lutResolution = 33;           // nodes per axis of the baked film-density -> display LUT (spec default 17)
  bool previewMode = false;
  // Execution hint of the spatial path (tile size in pixels, 0 = default 256x64). Never changes the result (tiles
  // recompute their halo, so output is bit-identical for any tile size); not part of the bake key or of the JSON.
  int tileWidth = 0, tileHeight = 0;
  std::uint32_t seed = 0;  // grain / glare noise seed (stateless counter hash of seed, x, y, layer; same seed -> same pattern)
};
struct DebugParams {
  bool deactivateSpatialEffects = false, deactivateStochasticEffects = false;
};

struct FilmParams {
  std::string film = "kodak_portra_400";
  std::string paper = "kodak_portra_endura";  // "none" -> scan the film itself
  CameraParams camera;
  FilmRenderParams filmRender;
  PrintRenderParams printRender;
  EnlargerParams enlarger;
  ScannerParams scanner;
  IoParams io;
  SettingsParams settings;
  DebugParams debug;

  // Range / finiteness validation of every field (what applyJson and FilmSimulator::setParams enforce). Pixel-radius
  // limits that depend on the image size are applied when the spatial plan is built (see film_spatial.cpp).
  bool validate(std::string* err = nullptr) const;

  // Parses the reference's nested JSON (e.g. {"enlarger":{"y_filter_shift":5}}) on top of *this.
  // Unknown keys are an error, like the reference; so are keys of options this engine does not implement
  // (output gamut knee / lightness compression, developer exhaustion, enlarger lens blur) unless set to their
  // neutral default, and values rejected by validate().
  bool applyJson(const std::string& json, std::string* err = nullptr);
  std::string toJson() const;
  // Applies the --no-spatial switches (spatial + stochastic effects off, glare off, DIR diffusion off).
  void disableSpatialAndStochastic();
};

// ---- spatial hooks ------------------------------------------------------------------------------------
// Plain-data callbacks at the three points where the reference applies spatial effects. All buffers are
// interleaved float3, full frame, row-major, modified in place. When no hook is set the whole pipeline
// runs as one fused streaming pass without any full-frame intermediate.
struct FilmSpatialContext {
  int width = 0, height = 0;
  double pixelSizeUm = 0;  // film_format_mm * 1000 / max(w,h)
  const FilmParams* params = nullptr;
};
struct FilmSpatialHooks {
  void* user = nullptr;
  // Linear film exposure after exposure compensation (camera diffusion, lens blur, halation, scatter).
  void (*exposure)(void* user, float* rgbExposure, const FilmSpatialContext& ctx) = nullptr;
  // DIR inhibitor field q (3 planes, same units as log10 exposure); diffusion of q goes here.
  void (*inhibitor)(void* user, float* q, const FilmSpatialContext& ctx) = nullptr;
  // Developed density (after DIR): grain goes here.
  void (*density)(void* user, float* cmy, const FilmSpatialContext& ctx) = nullptr;
  // Display sRGB-encoded result (scanner blur / unsharp mask go here; decode with FilmSimulator::srgbDecode).
  void (*scan)(void* user, float* rgbEncoded, const FilmSpatialContext& ctx) = nullptr;
  bool any() const { return exposure || inhibitor || density || scan; }
};

// ---- simulator ----------------------------------------------------------------------------------------

struct FilmTimings {
  // Fields are filled by the path that ran: sp* only by the spatial path, exposure/develop/printScan only by
  // renderStaged(); the others stay 0.
  double bakeMs = 0;       // 0 when the cached bake was reused
  double rawLutMs = 0, curvesMs = 0, neutralMs = 0, lutMs = 0;
  double autoExposureMs = 0;
  double exposureMs = 0, developMs = 0, printScanMs = 0;  // staged runs only
  double runMs = 0;        // fused run (or total of the staged run)
  // spatial run (planar passes + fused tile pass), 0 when no spatial effect is active
  double spExposureMs = 0, spCameraMs = 0, spHalationMs = 0, spDirMs = 0, spTileMs = 0;
  double autoExposureEv = 0;
};

// Thread safety: a FilmSimulator has one owner at a time. Concurrent render*() calls on the same instance are
// serialised internally where they share the spatial plan / scratch arena, and setParams()/setHooks() take an
// exclusive lock so a concurrent render never sees half-updated state, but the result of such a race (which
// parameters a render used) is unspecified. The supported multi-threaded entry is acquireSimulator() /
// SimulatorLease, which hands each caller its own instance. params() / effectiveParams() return references that
// are valid only until the next setParams().
// Errors: nothing here throws; setParams() leaves the simulator unchanged when it returns false (strong
// guarantee); render*() return false (never terminate) on allocation failure or invalid state.
class FilmSimulator {
 public:
  explicit FilmSimulator(std::shared_ptr<const FilmLibrary> lib);
  ~FilmSimulator();

  // Validates the parameters and (re)bakes when a non-spatial parameter changed. Returns false and fills
  // `err` for unknown stocks, out-of-range values etc.; the previous state is then untouched.
  bool setParams(const FilmParams& p, std::string* err = nullptr, FilmTimings* t = nullptr);
  const FilmParams& params() const;
  const FilmParams& effectiveParams() const;  // digested: stock overrides, solved neutral filters

  void setHooks(const FilmSpatialHooks& h);

  // Linear ProPhoto (D50) float RGB -> display sRGB-encoded float (not clamped; inside [0,1] up to LUT
  // interpolation error for gamut-compressed output).
  // `channelGain` (optional, 3 floats >= 0): per-channel multiplier of the input (white balance x exposure of the
  // develop pipeline), folded into the input matrix so no pre-multiplied copy of the frame is needed.
  bool render(const ImageF& linearProPhoto, ImageF& out, FilmTimings* t = nullptr,
              const float* channelGain = nullptr) const;
  // Streaming variant of render() for a consumer that does not need the float frame: the chain runs in row bands
  // through a thread-local tile and `sink(user, y, rgb, width)` is called (concurrently, once per row) with the
  // display-encoded, unclamped row. Returns false (nothing called) when spatial effects or hooks are active:
  // use render() then.
  using RowSink = void (*)(void* user, int y, float* rgb, int width);
  // `src` (optional) produces input row y (>= 0 linear ProPhoto, width floats*3) of a w x h frame that is a
  // resampling of `linearProPhoto` (geometry / zoom): then w, h are the output size and the source image only
  // serves the auto-exposure meter. Without `src`, the frame is `linearProPhoto` itself (w = h = 0).
  using RowSource = void (*)(void* user, int y, float* rgb);
  bool renderRows(const ImageF& linearProPhoto, int w, int h, const float* channelGain, RowSource src, void* srcUser,
                  RowSink sink, void* user) const;
  // Same, but runs the three stages (exposure, development, print+scan) as separate passes over
  // cache-resident blocks to time them individually.
  bool renderStaged(const ImageF& linearProPhoto, ImageF& out, FilmTimings* t = nullptr) const;

  // Direct evaluation of uniform patches (linear ProPhoto rgb) at chosen print exposure multipliers without baking tables (`body` gets the evaluator: display
  // sRGB-encoded colour, not clamped). Builds only the model (the raw LUT is cached); the simulator's own state is untouched. Used by the print-exposure
  // calibration of film_match.cpp.
  using UniformEval = std::function<void(const double rgb[3], double printExposure, double out[3])>;
  bool probeUniform(const FilmParams& p, const std::function<void(const UniformEval&)>& body, std::string* err = nullptr);
  // Single pixel, double precision, no tables: the slow reference path (test oracle).
  void renderPixelDirect(const double rgb[3], double out[3]) const;
  // Single pixel through the baked float tables (the same kernel the image path uses).
  void renderPixelBaked(const float rgb[3], float out[3]) const;
  // Intermediates of the direct path, for tests: log10 exposure and developed density.
  void directIntermediates(const double rgb[3], double logE[3], double density[3]) const;

  static float meterEv(const ImageF& img, const std::string& method);
  const char* kernelName() const;  // "avx2", "sse2", "neon", "scalar"

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace dapple::film

// ---- process-wide library and simulator pool (used by the develop pipeline, see renderer.cpp) -------------

namespace dapple::film {

// Releases the per-thread scratch buffers of the film engine (tile workspaces that otherwise keep their high-water
// mark): each worker frees its buffer on its next use, so call this before the next render (e.g. on a memory-pressure
// signal). Buffers also shrink by themselves when a much smaller request follows a large one.
void trimMemory();

// The film pack the develop pipeline draws from. setLibrary() installs one directly; setLibraryPath() only
// records a .dfp path (or a directory of profile JSONs) that is loaded lazily on first use. Thread-safe.
void setLibrary(std::shared_ptr<const FilmLibrary> lib);
bool setLibraryPath(const std::filesystem::path& path, std::string* err = nullptr);  // loads now
void setLibraryPathLazy(const std::filesystem::path& path);
std::shared_ptr<const FilmLibrary> defaultLibrary();  // null when none is available (see lastLibraryError())
// Hash of the content of the loaded pack file (or profile folder); 0 when the library was installed directly. Keys the film table files on disk.
std::uint64_t libraryContentHash();
std::string lastLibraryError();  // why the last lazy / explicit load failed ("" when none did)

// One selectable stock for UIs: id, display name, kind "negative" | "slide" | "paper" (print paper / print film).
struct StockInfo {
  std::string id, name, kind;
  std::string paper;  // the paper a negative gets when the edit names none (defaultPaperFor); "" for slides and papers
};
std::vector<StockInfo> listStocks(const FilmLibrary& lib);
// Paper used when EditParams::filmPaper is empty: a cine print film for cine stocks, "none" (scan) for slides,
// kodak_portra_endura for the rest.
std::string defaultPaperFor(const FilmLibrary& lib, const std::string& filmId);

// Exclusive lease on a simulator for (film, paper): the baked tables of the last use are kept, so a render whose
// non-spatial parameters did not change does not re-bake. Up to a few simulators stay cached.
class SimulatorLease {
 public:
  SimulatorLease() = default;
  SimulatorLease(SimulatorLease&&) noexcept;
  SimulatorLease& operator=(SimulatorLease&&) noexcept;
  ~SimulatorLease();
  explicit operator bool() const { return sim_ != nullptr; }
  void reset();  // releases the lease early
  FilmSimulator* operator->() const { return sim_; }
  FilmSimulator& operator*() const { return *sim_; }

 private:
  friend SimulatorLease acquireSimulator(const std::shared_ptr<const FilmLibrary>&, const std::string&,
                                         const std::string&);
  FilmSimulator* sim_ = nullptr;
  void* entry_ = nullptr;
};
SimulatorLease acquireSimulator(const std::shared_ptr<const FilmLibrary>& lib, const std::string& film,
                                const std::string& paper);

}  // namespace dapple::film
