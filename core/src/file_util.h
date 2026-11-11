#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dapple/result.h"

namespace dapple {

// UTF-8 text of a path. path::string() on Windows yields ANSI bytes and can throw.
inline std::string pathToUtf8(const std::filesystem::path& path) {
  const auto u = path.u8string();
  return std::string(u.begin(), u.end());
}

inline std::string lowerExtension(const std::filesystem::path& path) {
  std::string ext;
  try {
    ext = pathToUtf8(path.extension());
  } catch (...) {
    return {};
  }
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext;
}

// std::filesystem::path handles non-ASCII (e.g. Chinese) paths on Windows,
// unlike fopen(const char*), so all file IO goes through here.
inline Result<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return fail("Cannot open " + pathToUtf8(path));
  const auto size = static_cast<std::size_t>(in.tellg());
  std::vector<std::uint8_t> bytes(size);
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)))
    return fail("Cannot read " + pathToUtf8(path));
  return bytes;
}

inline Result<std::string> readTextFile(const std::filesystem::path& path) {
  auto bytes = readFile(path);
  if (!bytes) return std::unexpected(bytes.error());
  return std::string(bytes->begin(), bytes->end());
}

}  // namespace dapple
