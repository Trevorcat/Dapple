#pragma once
// Embeds a JPEG preview into a DNG that has none (what ImageCapture's DNG writer produces for RAW-only captures), as a reduced-resolution SubIFD
// (NewSubfileType 1, Compression 7, the layout LibRaw's unpack_thumb and Adobe's readers use). The preview is the camera's own rendering of the same
// frame: the exposure anchor (exposure_anchor.h, tier 1) reads it from the file itself, also after the file was copied elsewhere.
//
// The file is rewritten in place and only appended to: IFD0 is copied to the end of the file with one more entry (SubIFDs, tag 330), the new
// SubIFD and the JPEG follow, and the header points at the new IFD0; the old IFD0 stays as unreferenced bytes, so every offset of the raw data
// keeps its value. A file that already has a SubIFDs tag is left alone (returns false). The JPEG must be in the orientation of the raw data
// (sensor orientation, like the previews of other DNGs) or upright: the anchor tries both (exposure_anchor.cpp).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dapple {

bool embedDngThumbnail(const std::filesystem::path& dng, const std::vector<std::uint8_t>& jpeg, int width, int height, std::string* error = nullptr);

// Same with the JPEG read from a file; its size comes from the SOFn marker.
bool embedDngThumbnailFile(const std::filesystem::path& dng, const std::filesystem::path& jpegFile, std::string* error = nullptr);

}  // namespace dapple
