#pragma once
// Export metadata: the EXIF (APP1) block and the ICC profile (APP2) of an exported JPEG, and a small reader that finds the capture facts and the GPS fix
// in a source file (JPEG, TIFF-based RAW / DNG, Fujifilm RAF, PNG eXIf, HEIC / AVIF Exif item). Written from the public specifications (CIPA DC-008 Exif
// 2.32, ISO 15076-1 / ICC v4.4); no third-party code. The writer emits what a viewer shows (orientation, dates, camera, exposure, size, optional GPS), not
// maker notes, serial numbers or a thumbnail.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dapple {

struct ExportMeta {
  std::string make, model, lens;
  std::string software;       // "Dapple 0.1.0"
  float iso = 0.f;            // 0 = unknown (each of these is written only when known)
  float shutterSeconds = 0.f;
  float aperture = 0.f;
  float focalLengthMm = 0.f;
  std::string dateTimeOriginal;  // "YYYY:MM:DD HH:MM:SS" (local time of the camera) or empty
  std::string subSec;            // digits after the second ("123"), or empty
  std::string offsetTime;        // "+08:00", or empty
  int width = 0, height = 0;     // pixel size of the exported image (0 = not written)
  int colorSpace = 1;            // Exif ColorSpace: 1 = sRGB, 0xFFFF = uncalibrated (Display P3 and others: the ICC profile tells)

  bool hasGps = false;
  double latitude = 0, longitude = 0;  // signed degrees, north / east positive
  bool hasAltitude = false;
  double altitude = 0;  // metres above (positive) or below sea level
  std::string gpsDate;  // "YYYY:MM:DD" (UTC) or empty
  double gpsTime[3] = {0, 0, 0};
  bool hasGpsTime = false;
};

// The whole APP1 segment (FF E1, length, "Exif\0\0", little-endian TIFF with IFD0, Exif IFD and, when m.hasGps, the GPS IFD). Orientation is always 1: the
// pixels are already upright. Never empty.
std::vector<std::uint8_t> buildExifSegment(const ExportMeta& m);

enum class IccSpace { Srgb, DisplayP3 };
// The ICC profile (v4.3 matrix / TRC, sRGB parametric curve, D50 PCS) of the space, about 500 bytes.
std::vector<std::uint8_t> buildIccProfile(IccSpace space);
// The profile as one APP2 segment ("ICC_PROFILE\0", chunk 1 of 1).
std::vector<std::uint8_t> buildIccSegment(IccSpace space);

// What a source file says about its capture. False when nothing usable was found (the struct is left as it was). Fields already set in `m` are
// overwritten only when the file has a value.
bool readSourceExif(const std::filesystem::path& file, ExportMeta& m);

// Parses a TIFF block (Exif payload without the "Exif\0\0" prefix) the same way; exposed for tests.
bool parseExifTiff(const std::uint8_t* tiff, std::size_t size, ExportMeta& m);

}  // namespace dapple
