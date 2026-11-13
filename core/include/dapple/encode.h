#pragma once

#include <cstdint>
#include <filesystem>
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

}  // namespace dapple
