#pragma once
// CpuFrame: the point family's CPU binding of one frame (docs/engine/pipeline-architecture.md section 50): the source, the frame's point
// blocks (Geometry, Warp, Gain, Hdr: point::resolveFrame + hdrBind), the lattice sampler or the exact colour operator, the local
// adjustments' binding. The family's CPU nodes are block fragments over it (nodes/point/cpu_nodes.cpp); the film stage's glue reads the
// same binding (nodes/film/film_glue.cpp).
//
// Block convention: `px` = n pixels of interleaved float RGB (3 floats per pixel), n <= 256 in the executor.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/nodes/point/point.h"
#include "engine/nodes/frame_blocks.h"
#include "engine/nodes/frame_facts.h"
#include "src/local_eval.h"  // core/src (the C layer includes this header too)

namespace dapple::engine::point {
class CpuFrame final : public local::PixelSource {
 public:
  // lut != null: the baked variant (lattice + tone table, region + quality from `opt`);
  // lut == null: the exact variant (per-pixel PointOp::apply; `opt.spatial` = Off skips the neighbourhood stages).
  CpuFrame(const Params& r, const SourceImage& src, const BakedLut* lut, const FastOptions& opt);
  ~CpuFrame() override;
  CpuFrame(const CpuFrame&) = delete;
  CpuFrame& operator=(const CpuFrame&) = delete;

  bool valid() const;
  int width() const override;
  int height() const;
  bool exactVariant() const;
  bool useGeo() const;           // output pixel (x, y) is a resampling of the source (geometry or region)
  bool filmSelected() const;     // a film stock of the library is selected (film path will be tried)
  float filmStrength() const;    // 0..1
  bool filmFull() const { return filmSelected() && filmStrength() >= 1.f; }
  // Radii scale of the neighbourhood stages of this frame (region, region source and, on the exact path, the film outcome applied).
  float spatialScale() const;
  // The point family's blocks of this frame (geometry, warp, gain, HDR, look grain, radii scale; point::resolveFrame + hdrBind).
  const engine::point::Blocks& blocks() const;
  // What the node families resolve their blocks from (engine/nodes/frame_blocks.h), with the parameters of the frame.
  engine::FrameFacts frameFacts() const;
  // What the frame bound from its source that the families' resolves read (local adjustments, reshaping field); owned by this object.
  engine::FrameBound frameBound() const;
  // resolveFrameBlocks(params(), frameFacts(), frameBound())
  engine::FrameBlocks frameBlocks() const;
  const EditParams& params() const;

  // ---- point block fragments --------------------------------------------------------------------------------------------
  void copyRow(float* px, int n, int x0, int y) const;    // Resize/Source head, no resampling: the source row as it is
  void fetchRow(float* px, int n, int x0, int y) const;   // Geometry head: the source resampled through geometry / region
  void gain(float* px, int n) const;                      // Gain: max(0, v * wb * 2^EV)
  void toneColour(float* px, int n) const;                // ToneColour (baked, lattice + overflow) / ToneColourExact
  // CPU fusion recipe: [Resize | Geometry] + Gain + ToneColour in one pass (baked variant only; same per-pixel operations,
  // in the same order, as the three fragments above, without the two extra passes over the block).
  bool canFuseHead() const;
  void headGainTone(float* px, int n, int x0, int y) const;  // applies the HDR gain after the gain when hdrActive()
  bool hdrActive() const;                                 // single-image HDR is part of this frame (EditParams::hdr, point::Blocks::hdr)
  void hdrBlock(float* px, int n, int x0, int y) const;   // Hdr node (unfused chain): the HDR gain on gained pixels
  void displayStages(float* px, int n) const;             // curves + LUT on an encoded display value (film path)
  void blend(float* main, const float* film, int n) const;  // main += strength * (film - main)

  // ---- local::PixelSource (the local adjustments' view of the geometry, docs/engine/pipeline-architecture.md section 40) ------------
  // Source position of output pixel (x, y), normalised by the source size (the photo space of the masks).
  void srcUV(int x, int y, float& u, float& v) const override;
  // The pre-gain source value of output pixels x0 .. x0 + n - 1 of row y (copyRow or fetchRow).
  void sourceRow(float* px, int n, int x0, int y) const override;
  void linearRow(float* px, int n, int x0, int y) const override;  // gain + hdrBlock

  // ---- film (super-node until the film engineer's nodes land) -----------------------------------------------------------
  // Streaming path (full-strength film, 8-bit target, no neighbourhood stage): renders the whole frame into rgba.
  bool streamFilm(std::uint8_t* rgba);
  // FilmSim: the film render of the whole frame into `F` (display-encoded, unclamped). False: film unavailable, the frame
  // takes the normal path (then needNormal() is true).
  // Local adjustments (docs section 40.7): stage L is applied to the film's scene input; `aux` (whole frame) receives the stage-W amounts.
  bool runFilm(ImageF& F, const local::AuxTarget* aux = nullptr);
  bool filmOk() const;
  bool needNormal() const;       // the normal (tone + colour) branch contributes to the output

  // The exact variant's neighbourhood parameters depend on the film outcome; call after runFilm() (no-op for baked).
  void finalizeSpatial();

  // ---- streaming export (docs/engine/pipeline-architecture.md section 42) ------------------------------------------------------------
  // The source of the frame carries its size only (no pixels): before the head runs over a rect of output pixels the executor asks sourceRect()
  // which source pixels it reads ([sx0, sx1) x [sy0, sy1), clamped to the source; geometry is affine, so the corners plus the resampling support
  // bound it) and hands them over with setSourceWindow() (window pixel (0, 0) = source pixel (ox, oy)). Every source read of the head then goes to
  // the window, with the same clamping to the source edges as the whole picture, so the pixels are identical. Not for the film branch.
  void sourceRect(int x0, int y0, int x1, int y1, int& sx0, int& sy0, int& sx1, int& sy1) const;
  void setSourceWindow(const ImageF* win, int ox, int oy);
  // The film branch in bands (section 42.6). setSourceProvider: how the kernels obtain the source window of a rect themselves (it ends in
  // setSourceWindow). filmStreamBegin replaces runFilm: the film's picture-wide pre-passes (false + why: not streamable, render whole);
  // filmStreamRows: rows [y0, y1) of the film plane F into `out` (w x (y1 - y0), display-encoded, unclamped), bit-identical to runFilm's;
  // filmStreamAux: the local adjustments' stage-W amounts of a rect of a film frame (what runFilm writes into the whole-frame aux).
  void setSourceProvider(std::function<bool(int x0, int y0, int x1, int y1)> fn);
  bool filmStreamBegin(std::string* why = nullptr);
  bool filmStreamRows(int y0, int y1, ImageF& out);
  bool filmStreamAux(int x0, int y0, int x1, int y1, const local::AuxTarget& aux);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- the family's CPU nodes (block fragments over the frame's binding, nodes/point/cpu.cpp) ------------------------------------------
// Resolved slot of Resize / Geometry / Gain / Hdr / ToneColour(Exact) / the fused head / Blend: the frame's binding.
struct FrameSlot {
  const CpuFrame* frame;
};
// Resolved slot of DisplayStages: the film plane it reads and whether a Blend follows.
struct DisplayCpu {
  const CpuFrame* frame;
  const ImageF* film;
  bool replace;     // the display-staged film replaces the pixel (no Blend follows)
  const int* row0;  // streaming export: frame row of the film band's first row (null: the whole frame)
};
void copyBlock(const void* r, float* px, int n, const PixelPos& pos);  // the head of a chain without resampling: the source row as it is
const CpuImpl& geometryCpu();    // fuses [Geometry] + Gain + [Hdr] + ToneColour into one fragment (baked variant)
const CpuImpl& gainCpu();
const CpuImpl& hdrCpu();
const CpuImpl& toneColourCpu();  // ToneColour (lattice + overflow) and ToneColourExact
// The CPU fusion recipe of a head chain led by a resampling node (CpuImpl::fuse of Geometry and Resize): [Resize | Geometry] + Gain + [Hdr]
// + ToneColour as one block fragment, baked variant only.
const CpuImpl* fuseHead(const FrameEnv& env, std::span<const NodeType> chain, std::size_t* consumed);
const CpuImpl& displayCpu();
const CpuImpl& blendCpu();

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

}  // namespace dapple::engine::point
