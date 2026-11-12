#pragma once
// FrameKernels: the per-pixel pieces of Renderer::renderBakedImpl / renderFloat, resolved once per frame and exposed as
// block operations, for the engine's CPU nodes (core/engine/nodes/main). Internal header (core/src).
//
// Nothing here is a new algorithm: every method is the body of the per-pixel loop of the renderer it was taken from, so a
// graph built from these pieces reproduces the renderer bit for bit (gate B, tests/engine/golden).
//
// Block convention: `px` = n pixels of interleaved float RGB (3 floats per pixel), n <= 256 in the executor.
#include <cstdint>
#include <memory>

#include "dapple/frame_plan.h"
#include "dapple/renderer.h"
#include "spatial.h"

namespace dapple {

class FrameKernels {
 public:
  // lut != null: the baked path (renderBakedImpl: lattice + tone table, region + quality from `opt`);
  // lut == null: the exact path (renderFloat: per-pixel PointOp::apply; `opt.spatial` = Off skips the neighbourhood stages).
  FrameKernels(const Renderer& r, const SourceImage& src, const BakedLut* lut, const FastOptions& opt);
  ~FrameKernels();
  FrameKernels(const FrameKernels&) = delete;
  FrameKernels& operator=(const FrameKernels&) = delete;

  bool valid() const;
  int width() const;
  int height() const;
  bool exactVariant() const;
  bool useGeo() const;           // output pixel (x, y) is a resampling of the source (geometry or region)
  bool filmSelected() const;     // a film stock of the library is selected (film path will be tried)
  float filmStrength() const;    // 0..1
  bool filmFull() const { return filmSelected() && filmStrength() >= 1.f; }
  bool vignetteActive() const;
  // The neighbourhood stages of this frame (quality, region and, on the exact path, the film outcome applied).
  const SpatialParams& spatial() const;
  float spatialScale() const;    // SpatialParams::scale resolved (0 -> own long edge / 2048)
  // What the GraphBuilder (Full detail) needs: geometry, vignette, spatial amounts.
  FramePlan framePlan() const;

  // ---- point block fragments --------------------------------------------------------------------------------------------
  void copyRow(float* px, int n, int x0, int y) const;    // Resize/Source head, no resampling: the source row as it is
  void fetchRow(float* px, int n, int x0, int y) const;   // Geometry head: the source resampled through geometry / region
  void gain(float* px, int n) const;                      // Gain: max(0, v * wb * 2^EV)
  void toneColour(float* px, int n) const;                // ToneColour (baked, lattice + overflow) / ToneColourExact
  // CPU fusion recipe: [Resize | Geometry] + Gain + ToneColour in one pass (baked variant only; same per-pixel operations,
  // in the same order, as the three fragments above, without the two extra passes over the block).
  bool canFuseHead() const;
  void headGainTone(float* px, int n, int x0, int y) const;
  void displayStages(float* px, int n) const;             // curves + LUT on an encoded display value (film path)
  void blend(float* main, const float* film, int n) const;  // main += strength * (film - main)
  void vignette(float* px, int n, int x0, int y) const;

  // ---- film (super-node until the film engineer's nodes land) -----------------------------------------------------------
  // Streaming path (full-strength film, 8-bit target, no neighbourhood stage): renders the whole frame into rgba.
  bool streamFilm(std::uint8_t* rgba);
  // FilmSim: the film render of the whole frame into `F` (display-encoded, unclamped). False: film unavailable, the frame
  // takes the normal path (then needNormal() is true).
  bool runFilm(ImageF& F);
  bool filmOk() const;
  bool needNormal() const;       // the normal (tone + colour) branch contributes to the output

  // The exact variant's neighbourhood parameters depend on the film outcome; call after runFilm() (no-op for baked).
  void finalizeSpatial();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A recycled float frame (the renderer's ScratchImage): a fresh 20 MB buffer costs ~5 ms of page faults.
class ScratchFrame {
 public:
  ScratchFrame();
  ~ScratchFrame();
  ScratchFrame(const ScratchFrame&) = delete;
  ScratchFrame& operator=(const ScratchFrame&) = delete;
  ImageF& get(int w, int h);  // pixel values undefined

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dapple
