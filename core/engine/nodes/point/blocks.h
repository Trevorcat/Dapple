#pragma once
// The point family's resolved blocks of one frame (docs/engine/pipeline-architecture.md section 50): geometry, reshaping warp, input gain,
// single-image HDR, the film glue and the display stages after a film. Produced by point::resolveFrame (+ point::hdrBind once the tone table
// exists) for both executors: the CPU binding (point/cpu) reads them per pixel, the GPU executor records from them. The other families
// resolve from FrameFacts (point::facts), which carries the Geometry block.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/nodes/frame_facts.h"
#include "engine/nodes/point/frame_options.h"
#include "engine/nodes/point/geometry.h"

namespace dapple {
class Lut3D;
namespace film { struct SkinMask; }  // core/src/film_skin.h
struct HdrField;                   // core/src/hdr_field.h
namespace warp { struct Field; }   // core/src/warp_field.h
}  // namespace dapple

namespace dapple::engine::point {

// Portrait reshaping (section 45): the backward displacement field, applied to the source position after the geometry mapping
// (s -> s + B(s); grid coordinate = s * a + b, displacement in source pixels = d * lpx). Geometry::useGeo is on whenever it is set (an
// identity geometry then takes the exact index mapping where the field is zero). `pad` = source pixels the field can move a read (tile /
// band source windows grow by it).
struct Warp {
  std::shared_ptr<const warp::Field> field;
  float ax = 1.f, bx = 0.f, ay = 1.f, by = 0.f, lpx = 1.f;
  int pad = 0;
  bool active() const { return field != nullptr; }
};

// Single-image HDR (EditParams::hdr): a smooth gain field applied right after the input gain, before the tone stage. Active when `field` is
// set and s > 0; resolved by point::hdrBind (which needs the source and the frame's tone table).
struct Hdr {
  float s = 0.f, ev = 0.f, key = -2.5f, yn = 0.002f, yclip = 1.f, lo = -8.f, hi = -1.f, ls = 0.f, hp = 0.f;  // HdrParams (core/src/hdr_field.h)
  float range = 0.f, shown = 0.f;  // diagnostics: base range p1..p99 of the frame and the range the tone curve shows well (EV)
  std::shared_ptr<const HdrField> field;
  bool active() const { return s > 0.f && ls + hp > 1e-3f && field != nullptr; }
};

// Film glue (section 20): set when a film stock of the library is selected. The film nodes' arguments are built from it (GPU executor;
// the CPU film stage builds the same from the edit).
struct Film {
  bool active = false;
  std::shared_ptr<const dapple::film::SkinMask> skin;  // skin protection: the feathered skin plane of the photo (null = off; film_skin.h)
  float skinAmount = 0.f;         // EditParams::filmSkin
  bool displayRef = false;        // the source is display-referred (JPEG / PNG / HEIC): the inverse camera rendering goes in front of the film
  bool displayPlain = true;       // DisplayStages (master / RGB curves, look LUT) are the identity
  float strength = 1.f;           // film strength 0..1 (the film frame is blended with the normal render when < 1)
  std::string stock, paper;
  float filmEv = 0.f;             // film_exp + the film exposure of the automatic match
  float printEv = 0.f;            // print exposure (EditParams::filmPrintEv)
  float black = 0.f;              // paper black (EditParams::filmBlack)
  float sceneX = 0.f, sceneY = 0.f, adapt = 1.f;  // film white balance: assumed scene illuminant xy and degree of adaptation
  float warmth = 0.f, tint = 0.f, grainPct = 100.f, halationPct = 100.f;
  float formatScale = 1.f;
  int quality = 2;                // 0 off, 1 interactive, 2 full
  bool keepGrain = false;
  bool coarse = false;            // interactive frame while a print-table slider moves: coarse print table if the exact one is not cached
  int fullLongEdge = 0;
  unsigned frameIndex = 0;
};

// DisplayStages after a film: master / red / green / blue curves and the look LUT. Filled only for a film frame with
// `film.displayPlain == false`; the curves are the 1024-sample tables of the point parameters (identity curves are not stored).
struct Display {
  unsigned curveMask = 0;      // bit c: curve c (0 master, 1 red, 2 green, 3 blue) is not the identity
  int curveSamples = 0;
  std::vector<float> curves;   // [4][curveSamples]
  const Lut3D* lut = nullptr;  // look LUT (null = none); owned by the caller, valid for the frame
  float lutMix = 0.f;          // 0..1
  std::uint64_t lutId = 0;     // Lut3D::contentId()
};

struct Blocks {
  bool supported = true;      // false: the GPU cannot run this frame (reason in `why`); the CPU executor ignores it
  std::string why;
  Geometry geo;
  Warp warp;
  float gain[3] = {1.f, 1.f, 1.f};  // white balance * exposure, applied to the source before the tone stage (the film's input gain)
  Hdr hdr;
  float lookGrain = 0.f;      // the look's grain (slider units) the Grain block adds (0 when a film at full strength brings its own)
  float spatialScale = 1.f;   // radii scale of the neighbourhood stages (full-picture long edge / 2048; region, region source applied)
  // GPU export of a downscaled picture: extra output sharpening after the Lanczos resize (EngineSession::exportGpu); makes the graph contain a
  // Detail node. Never set on CPU frames.
  float resizeSharpen = 0.f;
  Film film;
  Display display;
};

// The frame-level facts the other families resolve from: the Geometry block, the radii scale, the quality of `opt`, the look grain.
FrameFacts facts(const Blocks& b, const FastOptions& opt, bool sceneReferred);

}  // namespace dapple::engine::point
