#pragma once

#include <optional>
#include <string>
#include <vector>

#include <array>

#include "dapple/edit_params.h"
#include "dapple/metadata.h"

namespace dapple {

// Tri-state because transfer features are often firmware dependent and we
// only want to claim what has been verified.
enum class Support { No, Yes, Unverified };

// How photos can get from the camera to the phone. The ingest layer reads
// this to decide which IngestSource implementations to offer for a body.
struct TransferCapabilities {
  Support ftpPush = Support::Unverified;        // camera uploads to an FTP server (our app)
  Support usbPtp = Support::Unverified;         // phone pulls over USB-C using PTP
  Support vendorAppAutoSend = Support::Unverified;  // vendor app pushes into the phone gallery
  std::string vendorApp;                        // e.g. "Creators' App", "FUJIFILM XApp"
};

// Camera matching profile: a smooth colour correction that makes an untouched RAW render
// look like the maker's own JPEG engine. Fitted by `dapple fit-profile` on pairs of
// (our tone-mapped render, camera JPEG); see docs/architecture.md.
//
// Applied in the colour stage of the scene-referred path, right after the tone stage:
//   1. 3x3 matrix in linear sRGB (rows sum to 1, so neutrals stay neutral),
//   2. OKLab offset LUT: trilinear lookup of (L, a, b) in a lutL x lutAB x lutAB grid
//      (L over 0..1, a and b over -lutABMax..+lutABMax); the node values are additive
//      offsets (dL, da, db). Beyond the grid the nearest node is used.
struct ColorMatchProfile {
  bool hasMatrix = false;
  float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  int lutL = 0;
  int lutAB = 0;
  float lutABMax = 0.32f;
  std::vector<float> lut;  // lutL * lutAB * lutAB * 3 values, index ((iL * lutAB + ib) * lutAB + ia) * 3

  bool hasLut() const { return lutL >= 2 && lutAB >= 2 && lut.size() == std::size_t(lutL) * lutAB * lutAB * 3; }
  bool enabled() const { return hasMatrix || hasLut(); }
};

// Everything camera-specific lives here. Adding a camera body (for a friend,
// say) means adding one profile; nothing in the pipeline special-cases models.
struct CameraProfile {
  std::string id;           // stable key, e.g. "sony.ilce-7rm5"
  std::string make;         // normalized maker as reported by the decoder
  std::string model;        // normalized model as reported by the decoder
  std::string displayName;  // e.g. "Sony α7R V"
  CfaKind cfa = CfaKind::Unknown;
  std::vector<std::string> rawExtensions;  // lower-case, with dot: ".arw"

  // Rendering defaults that make an untouched RAW look like a pleasing,
  // camera-JPEG-like starting point. Applied before user/preset edits.
  float baselineExposureEv = 0.f;
  float baselineContrast = 0.f;  // same scale as EditParams::contrast
  // Per-band colour rendering that reproduces the maker's JPEG colour (the
  // raw colour matrix alone is flat and slightly off in hue). EditParams::hsl scale.
  std::array<HslAdjust, kHslBandCount> baselineHsl{};
  // Fitted colour match to the maker's JPEG look (empty = not calibrated; the HSL
  // baseline above is then the only colour rendering).
  ColorMatchProfile colorMatch;

  TransferCapabilities transfer;
  std::string notes;
};

class CameraRegistry {
 public:
  // Registry pre-populated with the built-in profiles.
  static CameraRegistry withBuiltins();

  void add(CameraProfile profile);

  // Exact make+model match, else a maker-level generic profile, else the
  // global generic profile. Never fails.
  const CameraProfile& resolve(const std::string& make, const std::string& model) const;

  const std::vector<CameraProfile>& all() const { return profiles_; }

  // True when any known profile (or the generic set) claims this extension.
  bool isRawExtension(const std::string& lowerExt) const;

 private:
  std::vector<CameraProfile> profiles_;
  CameraProfile generic_;
};

// The camera profile for one file: the registry's profile for its make / model, with baselineExposureEv replaced by the file's own exposure
// anchor when the decoder computed one (ShotMetadata::exposureAnchorTier > 0). DAPPLE_PROFILE=generic forces the generic profile (diagnostic).
CameraProfile effectiveCamera(const CameraRegistry& registry, const ShotMetadata& meta);

}  // namespace dapple
