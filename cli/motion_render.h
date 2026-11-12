#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace dapple::motiontool {
// `dapple motion render ...` (args after "render"); also `dapple motion-render ...`.
int run(const std::vector<std::string>& args);
}  // namespace dapple::motiontool
