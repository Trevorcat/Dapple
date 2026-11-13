#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "dapple/image.h"
#include "dapple/result.h"

namespace dapple {

struct JpegOptions {
  int quality = 92;
  // Complete marker segments (FF E1 ... Exif, FF E2 ... ICC_PROFILE, see exif_writer.h) written into the stream: the Exif segment right after SOI,
  // the ICC segment after the JFIF header. Empty = none (the bare stream of before). The same options always give the same bytes, so the Ultra HDR base
  // stays identical to the plain export when both are encoded with the same options.
  std::vector<std::uint8_t> exifSegment;
  std::vector<std::uint8_t> iccSegment;
};

// The same stream writeJpeg stores, in memory.
Result<std::vector<std::uint8_t>> encodeJpeg(const Image8& image, const JpegOptions& options = {});

Result<void> writeJpeg(const Image8& image, const std::filesystem::path& path,
                       const JpegOptions& options = {});

// A finished stream to `path` (temp file + rename: a failed write leaves nothing at `path`).
Result<void> writeJpegBytes(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& path);

// Streaming encode (docs/engine/pipeline-architecture.md section 42): the strip encoder behind encodeJpeg fed band by band, so the 8-bit picture
// never exists whole. Rows arrive in order (any band height); every 512 rows one strip is encoded on a worker while the caller renders the next
// band (at most two strips of rows are alive). finish() returns what encodeJpeg(image, options) returns for the same pixels, byte for byte
// (pictures below 1024 rows or 2 MP are not stripped there: they are buffered whole here and encoded at finish()).
class JpegStripEncoder {
 public:
  JpegStripEncoder(int width, int height, const JpegOptions& options = {});
  ~JpegStripEncoder();
  JpegStripEncoder(const JpegStripEncoder&) = delete;
  JpegStripEncoder& operator=(const JpegStripEncoder&) = delete;
  bool push(const std::uint8_t* rgb, int rows, std::size_t strideBytes = 0);  // false: encode failed / more rows than the picture has
  Result<std::vector<std::uint8_t>> finish();
  bool stripped() const;
  std::size_t bufferBytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dapple
