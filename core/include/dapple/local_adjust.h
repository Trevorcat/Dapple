#pragma once
// Local adjustments through masks (docs/engine/pipeline-architecture.md section 40): the data model carried by EditParams::local and its
// JSON form (key "local" of the params JSON).
//
// An edit carries an ordered list of local adjustments. Each = a mask (an ordered fold of components: add / subtract / intersect) +
// a set of adjustment ops + strength + invert. Coordinates are the photo's own normalised space (0..1 of the decoded picture, before
// crop / rotate / straighten / flip); distances are fractions of the photo's long edge.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dapple {

constexpr int kMaxLocalAdjustments = 16;   // extra entries are dropped (one log line)
constexpr int kMaxMaskComponents = 8;      // per mask
constexpr int kMaxRasterComponents = 12;   // distinct raster planes per edit (extra ones evaluate to 0, one log line); grids shrink above 4 (section 40.4)
constexpr int kMaxBrushComponents = 8;     // per edit
constexpr int kMaxBrushPoints = 100000;    // per edit (points beyond are dropped)

enum class MaskKind : std::uint8_t { Linear, Radial, Luminance, Colour, Brush, Raster, Unknown };
enum class MaskOp : std::uint8_t { Add, Subtract, Intersect };

struct BrushStroke {
  float size = 0.05f;      // radius, fraction of the long edge (0.0005..0.5)
  float hardness = 50.f;   // 0..100 (100 = hard edge)
  float flow = 100.f;      // 0..100
  bool erase = false;
  std::vector<float> pts;  // x0, y0, x1, y1, ... photo-normalised
};

struct MaskComponent {
  MaskKind kind = MaskKind::Radial;
  MaskOp op = MaskOp::Add;
  bool invert = false;
  // linear: (x0, y0) full effect, (x1, y1) none
  float x0 = 0.5f, y0 = 0.f, x1 = 0.5f, y1 = 0.5f;
  // radial: centre, radii (fractions of the long edge), rotation (degrees, clockwise in the photo), feather 0..100
  float cx = 0.5f, cy = 0.5f, rx = 0.25f, ry = 0.25f, angle = 0.f, feather = 50.f;
  // luminance: L* range 0..100 with a soft edge 0..50 (uses `feather` above as its edge width when kind == Luminance: field lumFeather)
  float lo = 0.f, hi = 100.f, lumFeather = 15.f;  // (default feather 15 L*: a threshold never looks binary; `refine` below smooths the L* it reads)
  // color: OKLab hue centre (degrees), half width 0..180, feather 0..90, minimum saturation 0..100
  float hue = 0.f, hueRange = 30.f, hueFeather = 20.f, satMin = 10.f;
  // brush
  std::vector<BrushStroke> strokes;
  // raster: plane id (dapple_mask_raster_set) and edge-aware refinement 0..100 (0 = bilinear upsampling only). luminance: the same key = spatial
  // coherence 0..100 (0 = per-pixel L*, the old behaviour; > 0 = L* of an edge-aware smoothing of the photo, so a range on foliage selects regions,
  // not single pixels: section 40.10)
  std::string ref;
  float refine = 50.f;
  // raster with range (depth, AI planes): the plane's value 0..100 is first ranged like a luminance (lo..hi with a soft edge), then refined. The
  // default 0..100 is the identity (and is not written to JSON).
  float rangeLo = 0.f, rangeHi = 100.f, rangeFeather = 10.f;
  bool hasRange() const { return rangeLo > 0.f || rangeHi < 100.f; }
  // Unknown: the component's JSON as read (written back verbatim, evaluates to nothing)
  std::string raw;
};

// The local op registry (one row per op; docs sections 40.2 and 43). Order is the index order of LocalOps::v.
// Stage Heal: the op is consumed when the frame is resolved (spot healing: automatic spots detected inside the mask, docs section 43).
enum class LocalStage : std::uint8_t { Linear, Window, Heal };
struct LocalOpInfo {
  const char* key;   // JSON key inside "adjust"
  float lo, hi;      // internal range
  float scale;       // JSON value = internal * scale (exposure: EV * 20)
  LocalStage stage;
};
enum LocalOpId : int {
  kLoExposure, kLoContrast, kLoHighlights, kLoShadows, kLoWhites, kLoBlacks, kLoTemp, kLoTint, kLoSaturation, kLoVibrance,
  kLoColorHue, kLoColorSat,                                    // stage L
  kLoClarity, kLoTexture, kLoSharpen, kLoNoise, kLoDehaze,     // stage W (legacy: aux channels 0..4 in this order when no retouch op is used)
  kLoSmooth, kLoEven, kLoRedness, kLoUnderEye, kLoShine, kLoFaceLight,  // stage W, portrait retouch (docs section 43)
  kLoWhiten,                                                   // stage L, portrait retouch (teeth / eye whites)
  kLoHeal,                                                     // stage Heal: automatic spot healing amount
  kLocalOpCount
};
const std::array<LocalOpInfo, kLocalOpCount>& localOps();
constexpr int kLocalWindowFirst = kLoClarity;
constexpr int kLocalLegacyWindowOps = 5;                       // clarity .. dehaze
constexpr int kLocalRetouchFirst = kLoSmooth;
constexpr int kLocalWindowOps = kLoFaceLight + 1 - kLocalWindowFirst;  // 11 stage-W ops (contiguous)
constexpr int kLocalWindowSlots = 8;                           // per-pixel amount channels a frame can carry (2 x RGBA16F on the GPU)

struct LocalOps {
  std::array<float, kLocalOpCount> v{};  // all zero = no change (colour sat 0 = no tint)
  bool anyLinear() const;
  bool anyWindow() const;
};

// A manual heal spot (portrait retouch, docs section 43): photo-normalised centre, radius in units of the long edge, optional source offset
// (units of the long edge; `src` false = the engine picks a nearby clean patch).
struct HealSpot {
  float x = 0.5f, y = 0.5f, r = 0.01f;
  float dx = 0.f, dy = 0.f;
  bool src = false;
};
constexpr int kMaxHealSpots = 64;    // manual spots per adjustment

struct LocalAdjustment {
  std::string id, name;
  bool enabled = true;
  bool invert = false;
  float strength = 100.f;  // 0..100
  std::vector<MaskComponent> mask;
  LocalOps ops;
  // portrait retouch (docs section 43; JSON object "retouch" of the item, written only when not default)
  float face = 0.f;          // inter-ocular distance as a fraction of the photo's long edge (from the AI face metadata); 0 = estimate from the mask
  float healSens = 25.f;     // automatic heal sensitivity 0..100 (25 = conservative: freckles stay)
  std::vector<HealSpot> spots;  // manual heal spots (healed whatever the mask; the adjustment's strength applies)
  std::string extraOps;      // "adjust" keys this build does not know, kept verbatim ("k":v,... without braces) and written back
  std::string extraKeys;     // item keys this build does not know, kept verbatim and written back
  bool anyRetouch() const;   // a retouch op, or heal spots
};

// Brings every value into its range, drops entries beyond the caps (one log line each kind of drop).
void clampLocal(std::vector<LocalAdjustment>& list);
// JSON of the list ({"v":1,"items":[...]}); "" when the list is empty.
std::string localToJson(const std::vector<LocalAdjustment>& list);
// Reads the value of the "local" key (an object as above, or a bare array of items). Tolerant: bad entries are skipped.
// Returns false only when `json` is not valid JSON.
bool localFromJson(const std::string& json, std::vector<LocalAdjustment>& out);
// Hash of everything that changes pixels (disabled adjustments and names do not), incl. the content of the raster planes referenced.
std::uint64_t localHash(const std::vector<LocalAdjustment>& list);
const char* maskKindName(MaskKind k);

}  // namespace dapple
