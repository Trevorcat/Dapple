#pragma once
// DNG 1.7 JPEG XL raw data (Compression 52546): LibRaw 0.22 / the 202611 snapshot decode it only with the Adobe DNG SDK, which we do not build.
// This module reads IFD0 (strips or tiles, each one a JPEG XL codestream) with a small TIFF reader and libjxl's decoder (cmake/Jxl.cmake) and
// returns the 16-bit samples of the main image. Used for LinearRaw (3 samples per pixel, Samsung Galaxy S22 and newer) and for single-sample
// images; CFA mosaics stored as 4 half-size planes are interleaved back (RGGB order of the file's CFAPattern is NOT resolved here: the caller
// reports them as unsupported when `planes == 4`). Pure functions, no LibRaw dependency.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dapple::dng {

struct JxlRaw {
  int width = 0, height = 0, channels = 0;  // samples per pixel
  std::vector<std::uint16_t> data;          // interleaved, width * height * channels
  std::string error;                        // non-empty on failure
};

// True when IFD0 of the TIFF / DNG at `path` has Compression 52546.
bool isJxlDng(const std::filesystem::path& path);

// Decodes IFD0. `threads` <= 0: hardware concurrency. Fails with a precise message when the library was built without libjxl.
JxlRaw decodeJxlDng(const std::filesystem::path& path, int threads = 0);

// Decodes one JPEG XL codestream (tests): 16-bit interleaved samples, `channels` = colour channels of the stream.
JxlRaw decodeJxlStream(const std::uint8_t* data, std::size_t size);

}  // namespace dapple::dng
