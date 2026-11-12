#pragma once
// ParamSet and the parameter registry (docs/engine/pipeline-architecture.md section 4.4). Stage a1.
//
// A ParamKey is an index into a registry built from paramFields() (one key per numeric slider) plus explicit keys
// for the non-scalar EditParams fields and for the context a frame is rendered in (ctx.*). A node declares the
// keys it reads as a ParamMask; its content key hashes only those.
#include <memory>
#include <string>
#include <vector>

#include "dapple/edit_params.h"
#include "engine/graph/types.h"

namespace dapple {
struct ParamField;
namespace film { class FilmLibrary; }
}

namespace dapple::engine {

// What one frame request asks for (engine-level mirror of DappleFrameRequest + values the C layer derives).
struct FrameRequest {
  int longEdge = 0;
  bool interactive = false;
  bool wantAnalysis = false;
  bool exact = false;           // DAPPLE_FRAME_EXACT: reference pipeline, no lattice
  bool noise = true;            // Interactive only: also run noise reduction (derived by the C layer from lastParams)
  bool fastAnalysis = false;    // tonal analysis through the lattice (interactive frames)
  bool wantOverflow = false;    // a GPU executor needs the overflow lattice
  int bakeSize = 0;             // lattice size asked for (0 = 57 interactive / 65 final), as the C layer resolves it
  float region[4] = {0, 0, 0, 0};
  int regionLongEdge = 0;
  std::string activeParam;      // key of the slider being dragged ("" = none); grain stays on while a grain-family slider moves
};

// Per-photo / per-process facts a frame depends on.
struct SessionCtx {
  std::uint64_t sourceId = 0;   // identity of the source pixels the proxy/scaled copies derive from
  bool sceneReferred = true;
  bool hasProxy = false;
  std::string cameraId;
  std::uintptr_t lutIdentity = 0;
  const film::FilmLibrary* filmLib = nullptr;
  float toneReach = 1.f;
  int srcW = 0, srcH = 0;       // size of the (scaled) source this frame reads
  // ---- additive (film nodes) ----
  int fullLongEdge = 0;         // long edge of the full-resolution render this frame is a preview of (0 = none): scale-aware grain
  std::uint32_t frameIndex = 0; // video: enters the grain / glare noise seed (0 = a still; the seed is then unchanged)
  // White balance x exposure gain of the frame (what the Gain node multiplies): the film folds it into its input matrix.
  // Set by the C layer (Renderer); null = unit gain.
  void (*gainFn)(const void* user, const EditParams&, float out[3]) = nullptr;
  const void* gainUser = nullptr;
};

// All inputs a ParamSet is made of. The registry's hash and perturb functions work on this struct.
struct ParamInputs {
  EditParams edit;
  FrameRequest frame;
  SessionCtx ctx;
  // derived (filled by ParamSet::make / derive())
  bool filmActive = false, filmOnly = false;
  bool keepGrain = false;       // interactive frame while a grain-family slider is dragged (14 Q8): grain stays on
  int quality = 2;              // 0 Off, 1 Interactive, 2 Full
  void derive();
};

struct KeyInfo {
  std::string name;
  enum Kind : std::uint8_t { Slider, Group, Context, Derived } kind = Slider;
  const ::dapple::ParamField* field = nullptr;           // Slider
  void (*hash)(const ParamInputs&, Hasher&) = nullptr;   // Group / Context / Derived
  // Changes the input behind this key to a different legal value (variant 0 or 1); false = cannot be varied alone
  // (derived keys, keys that need real external objects such as a LUT).
  bool (*perturb)(ParamInputs&, int variant) = nullptr;
};

class ParamRegistry {
 public:
  static const ParamRegistry& instance();
  std::size_t size() const { return keys_.size(); }
  const KeyInfo& info(ParamKey k) const { return keys_[k]; }
  ParamKey find(std::string_view name) const;   // kNoKey when unknown
  ParamKey mustFind(std::string_view name) const;  // throws std::logic_error when unknown (descriptor tables)
  ParamMask mask(std::initializer_list<std::string_view> names) const;
  Hash128 hashKey(ParamKey k, const ParamInputs& in) const;
  // Changes the input behind key `k` to a different legal value (variant 0 or 1); false when it cannot be varied alone.
  bool perturb(ParamKey k, ParamInputs& in, int variant) const;

 private:
  ParamRegistry();
  std::vector<KeyInfo> keys_;
};

// Immutable snapshot of everything a build/plan/run may read, with each key hashed once.
class ParamSet {
 public:
  static ParamSet make(ParamInputs in);
  const EditParams& edit() const { return in_.edit; }
  const FrameRequest& frame() const { return in_.frame; }
  const SessionCtx& ctx() const { return in_.ctx; }
  const ParamInputs& inputs() const { return in_; }
  Hash128 hash(ParamKey k) const { return h_[k]; }
  Hash128 hash(const ParamMask& m) const;  // O(#bits set): combines the per-key hashes in key order

 private:
  ParamInputs in_;
  std::vector<Hash128> h_;
};

}  // namespace dapple::engine
