#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "dapple/image.h"
#include "dapple/result.h"

namespace dapple {

struct JpegOptions {
  int quality = 92;
};

// The same stream writeJpeg stores, in memory.
Result<std::vector<std::uint8_t>> encodeJpeg(const Image8& image, const JpegOptions& options = {});

Result<void> writeJpeg(const Image8& image, const std::filesystem::path& path,
                       const JpegOptions& options = {});

}  // namespace dapple
