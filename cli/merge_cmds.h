#pragma once
#include <string>
#include <vector>

namespace dapple::mergetool {
// hdr / pano / motion / detect. Returns the process exit code.
int run(const std::string& cmd, const std::vector<std::string>& args);
bool handles(const std::string& cmd);
}  // namespace dapple::mergetool
