#pragma once
// Per-process temp names for the test binaries: ctest -j runs several processes of one binary at once, and a fixed name in the shared temp directory
// made them rewrite / delete each other's files (a torn LUT gave an unexplained golden mismatch under load, wave 8).
#include <chrono>
#include <cstdint>
#include <string>

inline std::string testUniqueTag() {
  static const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tag));
  return tag;
}
