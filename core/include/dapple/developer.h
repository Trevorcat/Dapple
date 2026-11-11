#pragma once

#include <filesystem>
#include <optional>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"
#include "dapple/lut.h"

namespace dapple {

struct DevelopRequest {
  std::filesystem::path input;
  EditParams look;                // preset / user edits
  const Lut3D* lut = nullptr;     // optional creative LUT (look.lutAmount applies)
  std::optional<AutoOptions> autoAdjust;  // per-photo correction on top of the look
  DecodeQuality quality = DecodeQuality::Preview;
  int longEdge = 0;               // output size limit, 0 = native
};

struct DevelopTimings {
  double decodeMs = 0, analyzeMs = 0, renderMs = 0;
};

struct DevelopResult {
  Image8 image;
  ShotMetadata meta;
  const CameraProfile* camera = nullptr;
  std::optional<AutoResult> autoResult;
  EditParams applied;  // look + automatic correction actually rendered
  DevelopTimings timings;
};

// One-call "file in, finished photo out" used by the CLI and the app bindings.
// Thread-compatible: share one Developer per thread, or guard it.
class Developer {
 public:
  Developer();
  explicit Developer(CameraRegistry cameras);

  Result<DevelopResult> develop(const DevelopRequest& request) const;

  const CameraRegistry& cameras() const { return cameras_; }
  const DecoderRegistry& decoders() const { return decoders_; }

 private:
  CameraRegistry cameras_;
  DecoderRegistry decoders_;
};

}  // namespace dapple
