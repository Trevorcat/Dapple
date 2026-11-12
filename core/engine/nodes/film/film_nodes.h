#pragma once
// Film simulation as engine nodes (docs/engine/pipeline-architecture.md section 8).
//
// Table nodes (always CPU, cached by the ResultCache):
//   FilmTableRaw        stock                         -> RawTable    (matrix into the film's balance white + raw-exposure LUT)
//   FilmTableCurves     stock                         -> CurvesTable (density / pre-coupler curves, DIR matrix)
//   FilmTablePrintScan  stock, paper, warmth, tint    -> PrintTable  (33^3 print + scan LUT, glare white, sRGB decode)
// Image nodes (CPU impls; each calls the same SIMD kernels the FilmSimulator uses):
//   FilmExposure (P) -> FilmBlurExposure (G/W) -> FilmDirField (G/W) -> FilmDevelop (P) -> FilmGrain (W) -> FilmPrintScan (P)
//   -> FilmGlare (W) -> FilmScan (W)
// Optional effects, identity unless the resolved film asks for them (FilmResolved::fx):
//   highlight boost      inside FilmBlurExposure (a global maximum of the exposure planes, then a per-pixel curve, before the mixtures)
//   enlarger diffusion   FilmGrain -> FilmPrintExposure (P: density -> linear paper exposure, frame-sized planes) -> FilmEnlargerBlur
//                        (G/W: Gaussian mixtures of the paper exposure) -> FilmPrintScan (paper curve + scan LUT instead of the
//                        density LUT, reading the blurred frame with the reflect boundary)
//
// Image layout conventions: the frame source / final output are interleaved RGB; every film intermediate is PLANAR float
// (ImageView::planeStrideFloats != 0): E, E2, q at frame size, the tile-level density and linear-light planes at tile size.
// FilmGrain / FilmGlare / FilmScan may run in place (out aliasing in) or from a separate window; windows must cover the
// node's output rect grown by its halo (clipped to the frame; reflect boundary against CpuCtx::frame, scipy 'reflect').
// FilmBlurExposure / FilmDirField are whole-frame nodes (their run() uses the thread pool itself).
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "engine/graph/node.h"
#include "engine/graph/params.h"
#include "engine/graph/types.h"

namespace dapple::film { class FilmLibrary; struct FilmStock; }

namespace dapple::engine::filmnode {

// ---- table payloads (ResultCache payloads; opaque to the planner) -----------------------------------------------------------
struct RawTable;
struct CurvesTable;
struct PrintTable;

// Film effects the edit schema does not expose (the app never sets them): highlight boost (halation.boost_ev / boost_range /
// protect_ev) and the enlarger diffusion filter (enlarger.diffusion_filter). Defaults are the FilmParams defaults = off.
struct FilmFx {
  float boostEv = 0, boostRange = 0.3f, protectEv = 4.0f;  // boostEv > 0 turns the boost on
  std::uint8_t enlargerFilter = 0;                          // 0 off, else 1 + index into kDiffusionFamilies
  float enlStrength = 0.5f, enlScale = 1.f, enlHaloWarmth = 0.f;
  float enlCoreIntensity = 1.f, enlCoreSize = 1.f, enlHaloIntensity = 1.f, enlHaloSize = 1.f, enlBloomIntensity = 1.f, enlBloomSize = 1.f;
  bool any() const { return boostEv > 0.f || enlargerFilter != 0; }
};
inline constexpr const char* kDiffusionFamilies[4] = {"glimmerglass", "black_pro_mist", "pro_mist", "cinebloom"};

// Everything the film nodes read from the ParamSet and the frame geometry. POD, <= 256 bytes. One struct for all film
// nodes (each reads its own subset; the ParamMasks declare which parameters that subset depends on).
struct FilmResolved {
  const dapple::film::FilmLibrary* lib = nullptr;
  const dapple::film::FilmStock* stock = nullptr;
  const dapple::film::FilmStock* paper = nullptr;   // null: scan the film itself (paper "none")
  float gain[3] = {1, 1, 1};     // white balance x exposure x 2^filmExposure, folded into the input matrix
  float warmth = 0, tint = 0;    // percent, -100..100
  float printEv = 0;             // EditParams::filmPrintEv: print exposure (paper) / scan lamp gain, stops
  float black = 0;               // EditParams::filmBlack, -1..1
  float sceneX = 0, sceneY = 0, adapt = 1;  // film white balance: assumed scene illuminant xy (0 = none) and the degree of adaptation (EditParams::filmWb)
  float grainPct = 100, halationPct = 100;
  float pitch = 1;               // output pixel size in source pixels (1 = plain frame)
  float formatScale = 1;         // pitch * max(w,h) / max(srcW,srcH): scales the film frame in mm
  int w = 0, h = 0;              // frame of this node's grid
  int fullLongEdge = 0;          // scale-aware grain: long edge of the full-resolution render (0 = this frame is it)
  std::uint32_t seed = 0;        // grain / glare noise seed (frameIndex already mixed in)
  std::uint32_t frameIndex = 0;
  std::uint8_t quality = 2;      // 0 off, 1 interactive, 2 full
  std::uint8_t keepGrain = 0;    // interactive frame while a grain-family slider moves
  std::uint8_t coarse = 0;       // coarse print table allowed (interactive frame while a print-table slider moves)
  std::uint8_t clampOut = 0;     // GPU: the frame is followed by a pure clamp (strength 1, no display stages, no vignette): the last film pass may clamp itself
  std::uint8_t scan = 0;         // no paper: scan the developed film
  FilmFx fx;                     // highlight boost / enlarger diffusion filter (not part of the edit schema: tests, CLI, renderer entry)
};
static_assert(sizeof(FilmResolved) <= 256);

// Fills a FilmResolved from the ParamSet (the resolve function of every film node).
void resolveFilm(const ParamSet& ps, const FrameGeom& geom, FilmResolved& out);

const CpuImpl& cpuImpl(NodeType t);                 // run / bake of a film node type
const NodeBehaviour& behaviour(NodeType t);         // resolve / isIdentity / halo of a film node type
bool isFilmNode(NodeType t);

// ---- halos and rects the planner (or a test driver) needs ----------------------------------------------------------------------
struct FilmHalos {
  int grainDye = 0, grainBlur = 0;   // FilmGrain: input halo = dye + blur
  int scan = 0;                      // FilmScan: scanner blur + unsharp radius
  bool boost = false;                // highlight boost (FilmBlurExposure)
  bool enlargerSplit = false;        // enlarger diffusion filter (FilmPrintExposure / FilmEnlargerBlur / split FilmPrintScan)
  bool preMix = false, haloMix = false, dirDiffuse = false, grain = false, glare = false, scanStage = false;
  bool dirHalf = false;
};
FilmHalos halosOf(const FilmResolved& r);

// ---- Whole-frame reference executor ---------------------------------------------------------------------------------------------
// Runs the film nodes in order on a whole frame: exposure rows, mixtures, DIR field, then the fused tile pass (256 x 64 tiles
// with the accumulated halo), calling the node run() functions. This is what the CPU executor does with a Film group; the
// tests and film-render-bench use it as the reference for gate B (byte-identical with FilmSimulator::render).
//   in  : interleaved RGB frame (linear ProPhoto, ungained)    out : interleaved RGB (display sRGB-encoded, unclamped)
struct FilmTimingsNodes {
  double tablesMs = 0, exposureMs = 0, blurMs = 0, dirMs = 0, tileMs = 0, totalMs = 0;
};
struct FilmTablesHandle {
  std::shared_ptr<const RawTable> raw;
  std::shared_ptr<const CurvesTable> curves;
  std::shared_ptr<const PrintTable> print;
};
// Bakes (or fetches from the process-wide table cache) the three tables of `r`; returns false on error.
bool bakeFilmTables(const FilmResolved& r, FilmTablesHandle& out, std::string* err, FilmTimingsNodes* t = nullptr);
bool runFilmFrame(const FilmResolved& r, const FilmTablesHandle& tables, const float* in, std::size_t inStrideFloats,
                  float* out, std::size_t outStrideFloats, int tileW = 0, int tileH = 0, FilmTimingsNodes* t = nullptr);

// ---- Renderer-facing entry (FrameKernels::runFilm) --------------------------------------------------------------------------------
// The film of one frame from the values the C layer / renderer already have; tables come from a process-wide cache with the
// granularity of the table nodes (raw + curves per stock, print per stock / paper / warmth / tint), so a warmth drag re-bakes
// only the print LUT. Returns false when the node chain cannot render this frame (unknown stock / paper, allocation failure): the
// caller takes the FilmSimulator path. Every effect of the film model is supported (boost and enlarger diffusion included).
struct FilmFrameArgs {
  const dapple::film::FilmLibrary* lib = nullptr;
  const char* stock = nullptr;     // stock id
  const char* paper = nullptr;     // resolved print paper id, "none" = scan the film
  float gain[3] = {1, 1, 1};       // white balance x exposure (the film exposure is folded in here)
  float filmExposure = 0;          // EV on top
  float printEv = 0;               // print exposure / scan lamp gain in stops (EditParams::filmPrintEv)
  float black = 0;                 // paper black, -1..1 (EditParams::filmBlack)
  float sceneX = 0, sceneY = 0, adapt = 1;  // film white balance (EditParams::filmWb): assumed scene illuminant xy and degree of adaptation
  float warmth = 0, tint = 0, grainPct = 100, halationPct = 100;
  float formatScale = 1;
  int w = 0, h = 0;
  FilmFx fx;                       // optional effects (default off)
  int quality = 2;                 // 0 off, 1 interactive, 2 full
  bool keepGrain = false;          // interactive tier while a grain-family slider moves
  bool coarse = false;             // interactive tier while a print-table slider moves (FastOptions::coarseFilm)
  int fullLongEdge = 0;            // scale-aware grain: long edge of the full-resolution render (0 = none)
  std::uint32_t frameIndex = 0;
};
bool filmFrameResolved(const FilmFrameArgs& a, FilmResolved& out);
bool renderFilmFrame(const FilmFrameArgs& a, const float* in, std::size_t inStrideFloats, float* out, std::size_t outStrideFloats);
// DAPPLE_FILM_NODES: unset / anything but "0" = on.
bool enabled();

}  // namespace dapple::engine::filmnode
