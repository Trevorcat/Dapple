#pragma once
// VideoFrameSession: a Motion Photo / Live Photo clip through the same node graph as the still
// (docs/engine/pipeline-architecture.md section 30, requirements 4.9.5a).
//
// A decoded frame enters the engine as a SourceImage in the working space (linear ProPhoto, D50), exactly as a JPEG or RAW does, and goes
// through EngineSession::renderFrame (Planner + CPU / GPU executor). What is held for the whole clip, never recomputed per frame:
//   * the film exposure match (filmKeyEv / filmPrintEv): metered once on the reference frame (the cover frame, or the still itself);
//   * the table nodes (colour lattice, tone analysis, tone table, film tables): cached in the session's ResultCache under keys that do not
//     depend on the pixels, so frame 2..n hit what the reference frame built;
//   * the frame-global solves (dehaze airlight, local detail base): same cache, same keys.
// Per frame only the pixels, the grain seed (ctx.frameIndex) and the spatial stages change.
//
// The frame source is the half-float linear RGB the video core delivers (DV_RGBAF16): BT.709 primaries, 1.0 = SDR reference white, may
// exceed 1 for HDR; S-Log3 is scene linear (18 % grey = 0.18), HLG / PQ are display light relative to 203 nit. No 10 to 8 bit step exists
// between the decoder and the display-encoded float result; the output is quantised once, at the end, to the encoder's depth.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"

namespace dapple::engine::video {

// How the decoded stream was classified (the same numbers as DvColorKind in core/video/dapple_video.h).
enum class ColourKind : int { Standard = 0, Sdr2020 = 1, Hlg = 2, Pq = 3, Slog3 = 4 };

// One decoded frame: half-float RGBA (4 halfs per pixel, alpha ignored), linear light, BT.709 primaries.
struct InputFrame {
  const std::uint16_t* rgbaF16 = nullptr;
  int width = 0, height = 0;
  ColourKind kind = ColourKind::Standard;
  int rotationDeg = 0;  // degrees clockwise to apply for display (container rotation); 0 / 90 / 180 / 270
};

enum class PackFormat : int {
  Rgba8 = 0,  // 8-bit sRGB-encoded RGBA (alpha 255)
  I420 = 1,   // 8-bit 4:2:0 planar, BT.709 limited range (Y then U then V), 8x8 ordered dither
  P010 = 2,   // 10-bit 4:2:0, Y plane then interleaved UV, 16-bit LE words with the value in the top 10 bits, BT.709 limited range (SDR)
  P010Hdr = 3,  // like P010, but in the session's output transfer: HLG / PQ on BT.2020 limited range when outputKind() is HDR, else exactly P010
  Rgb10A2 = 4,  // 10-bit packed RGB for an input Surface: 32-bit LE words, R bits 0-9, G 10-19, B 20-29, alpha 2 bits = 3; sRGB-encoded (SDR)
  Rgb10A2Hdr = 5,  // the same in the session's output transfer on BT.2020 primaries (HLG / PQ) when outputKind() is HDR, else exactly Rgb10A2
};

// What the clip is written as (docs/engine/pipeline-architecture.md section 30.8).
enum class OutputKind : int { Sdr709 = 0, Hlg2020 = 1, Pq2020 = 2 };

struct Config {
  EditParams params;           // the still's edit (geometry included: crop / rotate / straighten / flip)
  CameraProfile camera;        // the still's camera profile (baseline exposure of scene-referred sources)
  std::shared_ptr<const Lut3D> lut;
  float sceneIlluminantX = 0.f, sceneIlluminantY = 0.f;  // the still's as-shot illuminant (film white balance), 0 = unknown
  int maxLongEdge = 0;         // frames larger than this (long edge) are area-averaged down first; 0 = natural size
  float stillAspect = 0.f;     // width / height of the upright still: a frame of another aspect is centre-cropped to it first (0 = keep)
  int stillLongEdge = 0;       // long edge of the still render: the film grain is scale-aware against it (0 = the frame's own scale)
  SpatialQuality spatial = SpatialQuality::Full;
  int bakeSize = 65;           // lattice size of the point chain
  bool allowGpu = false;       // 8-bit output only: the GPU executor writes RGBA8 (see section 30.6)
  // HDR policy: an HLG / PQ source with an edit WITHOUT film stays HDR (output HLG / PQ on BT.2020, highlights above SDR white preserved); film is a print and
  // is always SDR; an edit with geometry (crop / rotate / straighten / flip) is SDR too (the highlight headroom is not warped). false = always SDR.
  bool allowHdr = true;
};

struct FrameInfo {
  int width = 0, height = 0;   // size of the result (even: an odd edge loses its last row / column when YUV is asked for)
  std::string executor;        // "cpu" | "vulkan" | "gles31"
  int outBits = 0;             // precision the graph delivered: 32 (float, quantised by the pack) or 8 (GPU)
  double convertMs = 0, renderMs = 0, packMs = 0;
  bool tablesHit = false;      // the point chain's tables came from the cache (every frame but the reference)
};

class VideoFrameSession {
 public:
  explicit VideoFrameSession(Config cfg);
  ~VideoFrameSession();
  VideoFrameSession(const VideoFrameSession&) = delete;
  VideoFrameSession& operator=(const VideoFrameSession&) = delete;

  // Freezes the clip-wide state from the reference frame (film match, tone proxy, table nodes). The first render() does it with its own frame
  // when this was not called. Calling it again re-freezes (a new clip, or the user changed the edit: use setEdit instead).
  bool setReference(const InputFrame& f, std::string* err = nullptr);
  // The reference is the still itself (already in the working space): same effect as setReference on the cover frame.
  bool setReferenceStill(const SourceImage& still, std::string* err = nullptr);
  bool hasReference() const;

  // Replaces the edit (the user moved a slider): keeps the held reference, re-meters the film on it, the tables follow by key.
  bool setEdit(const EditParams& params, std::string* err = nullptr);

  // The display-encoded float frame (0..1 sRGB-encoded RGB, clamped), the exact input of every pack format.
  bool renderFloat(const InputFrame& f, std::uint32_t frameIndex, ImageF& out, FrameInfo* info = nullptr, std::string* err = nullptr);
  // renderFloat that also hands out the working-space source of the frame (the HDR output needs it)
  bool renderFloatSrc(const InputFrame& f, std::uint32_t frameIndex, ImageF& out, SourceImage& srcOut, FrameInfo* info = nullptr, std::string* err = nullptr);
  // The frame packed for the encoder. frameIndex: 0 = like a still, n >= 1 = clip frame n (grain seed).
  bool render(const InputFrame& f, std::uint32_t frameIndex, PackFormat fmt, std::vector<std::uint8_t>& out, FrameInfo* info = nullptr,
              std::string* err = nullptr);

  // The edit as the clip uses it (film match applied) and the frame size the session was frozen for.
  const EditParams& heldParams() const;
  // The transfer / primaries the clip is written in (decided when the reference is set) and why. Tags for the encoder: primaries / transfer / matrix
  // as FFmpeg AVCOL_* numbers (1 / 1 / 1 for SDR, 9 / 18 or 16 / 9 for HLG / PQ).
  OutputKind outputKind() const;
  const char* outputNote() const;
  void outputTags(int out[4]) const;  // primaries, transfer, matrix, range (1 = limited)
  int frameWidth() const;
  int frameHeight() const;
  // Size of every result frame (after the edit's geometry; even, so every pack format fits), known once the reference is set.
  int outputWidth() const;
  int outputHeight() const;
  EngineSession& engine();

  // The camera profile every clip is rendered with, whatever the still's camera: a video signal is already camera-processed (its own gamma, gamut, exposure),
  // so there is no RAW baseline exposure, no baseline contrast / HSL and no fitted colour match. Scene-referred sources (S-Log3 / HLG / PQ) are anchored at
  // 18 % grey = 0.18 with an explicit, stable baseline of kVideoBaselineEv (0 EV): the shooter's exposure stays as recorded. The per-file anchor of the RAW
  // path (ShotMetadata::exposureAnchor*) never applies to video. `Config::camera` is therefore only used for its id.
  static constexpr float kVideoBaselineEv = 0.f;
  static CameraProfile videoCamera();

  // ---- pieces, public for the tests ----------------------------------------------------------------------------------------
  // The conversion into the working space: rotation, centre crop to the still's aspect, optional area-average downscale, then
  // BT.709 linear -> linear ProPhoto. Scene-referred for S-Log3 / HLG / PQ (18 % grey = 0.18, like a RAW), display-referred for SDR (1.0 = white, like a JPEG).
  static bool toSource(const InputFrame& f, const Config& cfg, SourceImage& out, std::string* err = nullptr);
  // Packs a display-encoded float frame. The result size is `even(w) x even(h)` for the YUV formats.
  static void pack(const ImageF& img, PackFormat fmt, std::vector<std::uint8_t>& out, int* w, int* h);
  // The HDR output: `sdrEnc` = the graph's display-encoded float frame (the edit's look), `src` = the frame in the working space, `gainEv` = the edit's
  // exposure. The result is the BT.2020 HLG / PQ encoded RGB (0..1): linear(sdrEnc) + the part of the source above SDR white (scaled by the exposure)
  // converted to BT.2020, then the transfer's OETF. Same size as `sdrEnc`; false when the sizes differ (geometry edit).
  static bool toHdrEncoded(const ImageF& sdrEnc, const SourceImage& src, float gainEv, OutputKind kind, ImageF& out);
  static std::size_t packedBytes(PackFormat fmt, int w, int h);

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// IEEE half to float (the video core's F16 output).
float halfToFloat(std::uint16_t h);

}  // namespace dapple::engine::video
