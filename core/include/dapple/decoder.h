#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "dapple/image.h"
#include "dapple/metadata.h"
#include "dapple/result.h"

namespace dapple {

enum class DecodeQuality {
  Preview,  // fastest path: half-size RAW (no demosaic interpolation)
  Full,     // full resolution, high quality demosaic
};

struct DecodeOptions {
  DecodeQuality quality = DecodeQuality::Preview;
  // Reference rendering of this very frame for the exposure anchor (core/src/exposure_anchor.h): the camera JPEG saved next to the RAW
  // (RAW+JPEG), or any sidecar image of the same frame. Used first when set; when empty the embedded preview is used and, if that is missing
  // or unusable, a sibling file with the same stem and a .jpg / .jpeg extension is tried. Orientation is read from its EXIF.
  std::filesystem::path referenceImage;
};

// Output of every decoder, whatever the source format.
struct SourceImage {
  // Linear ProPhoto RGB, white-balanced with the as-shot white balance.
  // For RAW: 1.0 == sensor clipping point. For JPEG: 1.0 == display white.
  ImageF linear;
  ShotMetadata meta;
  // RAW data is scene-referred and still needs a base tone curve; JPEG/HEIF
  // from the camera are already display-referred.
  bool sceneReferred = true;
};

class ImageDecoder {
 public:
  virtual ~ImageDecoder() = default;
  virtual const char* name() const = 0;
  // Cheap check based on extension and/or magic bytes.
  virtual bool canDecode(const std::filesystem::path& path) const = 0;
  virtual Result<SourceImage> decode(const std::filesystem::path& path,
                                     const DecodeOptions& options) const = 0;
  // Metadata only, without decoding pixels.
  virtual Result<ShotMetadata> probe(const std::filesystem::path& path) const = 0;
};

// The JPEG preview most RAW files embed (what the camera's own JPEG engine
// produced). Oriented upright and scaled to `longEdge` (0 = native size).
// Used for calibration and as an instant placeholder in the app.
Result<Image8> extractEmbeddedPreview(const std::filesystem::path& rawPath, int longEdge);
// The embedded preview for statistics (the exposure anchor): a JPEG of 2048 px or more is read at 1/8 scale (block means from the DC coefficients,
// core/src/decoders/jpeg_dc.h), so a 40 MP preview costs about 0.1 s instead of 4 s on a phone. DAPPLE_ANCHOR_FAST=off decodes in full.
Result<Image8> extractEmbeddedPreviewForAnchor(const std::filesystem::path& rawPath);

// A JPEG as an upright 8-bit sRGB image (EXIF orientation applied), for reference renderings.
Result<Image8> loadReferenceJpeg(const std::filesystem::path& path);

std::unique_ptr<ImageDecoder> makeLibRawDecoder();
std::unique_ptr<ImageDecoder> makeJpegDecoder();

// Picks the first decoder that accepts a file.
class DecoderRegistry {
 public:
  static DecoderRegistry withBuiltins();
  void add(std::unique_ptr<ImageDecoder> decoder);
  const ImageDecoder* find(const std::filesystem::path& path) const;

 private:
  std::vector<std::unique_ptr<ImageDecoder>> decoders_;
};

}  // namespace dapple
