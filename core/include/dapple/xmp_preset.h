#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "dapple/edit_params.h"
#include "dapple/result.h"

namespace dapple {

struct PresetImport {
  std::string name;
  EditParams params;
  // Camera Raw settings present in the file that Dapple does not reproduce
  // (or only approximates). Shown to the user so expectations are honest.
  std::vector<std::string> unsupported;
};

// Lightroom / Camera Raw preset or sidecar (.xmp). Supports both the
// attribute form (crs:Exposure2012="+0.50") and the element form.
Result<PresetImport> loadXmpPreset(const std::filesystem::path& path);
Result<PresetImport> parseXmpPreset(const std::string& xml);

}  // namespace dapple
