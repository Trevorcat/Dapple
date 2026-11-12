#pragma once

#include <string>

namespace dapple {

enum class CfaKind {
  Unknown,
  Bayer,   // 2x2 pattern: Sony, Canon, Nikon, most others
  XTrans,  // 6x6 pattern: Fujifilm X-Trans sensors
  None,    // already demosaiced (JPEG/HEIF/linear DNG)
};

const char* toString(CfaKind kind);

struct ShotMetadata {
  std::string make;   // normalized maker, e.g. "Sony", "Fujifilm"
  std::string model;  // normalized model, e.g. "ILCE-7RM5", "X-T50"
  std::string lens;
  float iso = 0.f;
  float shutterSeconds = 0.f;
  float aperture = 0.f;
  float focalLengthMm = 0.f;
  long long timestamp = 0;  // unix seconds, 0 when unknown

  int sensorWidth = 0;   // full-resolution output size before any downscale
  int sensorHeight = 0;
  CfaKind cfa = CfaKind::Unknown;
  int bitsPerSample = 0;
  std::string rawFormat;  // e.g. "lossless compressed (M)", informational
  float asShotWb[3] = {1.f, 1.f, 1.f};  // camera multipliers normalized to G = 1
  // CIE xy of the as-shot illuminant (camera neutral through the inverse of the file's XYZ -> camera matrix); {0, 0} = unknown
  float asShotIlluminantXy[2] = {0.f, 0.f};
  float asShotCct = 0.f;  // correlated colour temperature of that illuminant (the camera's own Kelvin table where it records one); 0 = unknown

  // Vendor look the photographer picked in camera, when the file records it
  // (e.g. Fujifilm film simulation, Sony Creative Look). Empty if unknown.
  std::string inCameraLook;
};

}  // namespace dapple
