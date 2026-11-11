#pragma once

#include <filesystem>

#include "dapple/image.h"
#include "dapple/result.h"

namespace dapple {

struct JpegOptions {
  int quality = 92;
};

Result<void> writeJpeg(const Image8& image, const std::filesystem::path& path,
                       const JpegOptions& options = {});

}  // namespace dapple
