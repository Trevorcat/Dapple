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
constexpr int kMaxRasterComponents = 4;    // per edit (extra ones evaluate to 0, one log line)
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
  float lo = 0.f, hi = 100.f, lumFeather = 10.f;
  // color: OKLab hue centre (degrees), half width 0..180, feather 0..90, minimum saturation 0..100
  float hue = 0.f, hueRange = 30.f, hueFeather = 20.f, satMin = 10.f;
  // brush
  std::vector<BrushStroke> strokes;
  // raster: plane id (dapple_mask_raster_set) and edge-aware refinement 0..100 (0 = bilinear upsampling only)
  std::string ref;
  float refine = 50.f;
  // Unknown: the component's JSON as read (written back verbatim, evaluates to nothing)
  std::string raw;
};

// The local op registry (one row per op; docs section 40.2). Order is the slot order of LocalOps::v.
enum class LocalStage : std::uint8_t { Linear, Window };
struct LocalOpInfo {
  const char* key;   // JSON key inside "adjust"
  float lo, hi;      // internal range
  float scale;       // JSON value = internal * scale (exposure: EV * 20)
  LocalStage stage;
};
enum LocalOpId : int {
  kLoExposure, kLoContrast, kLoHighlights, kLoShadows, kLoWhites, kLoBlacks, kLoTemp, kLoTint, kLoSaturation, kLoVibrance,
  kLoColorHue, kLoColorSat,                                    // stage L
  kLoClarity, kLoTexture, kLoSharpen, kLoNoise, kLoDehaze,     // stage W (aux slots 0..4 in this order)
  kLocalOpCount
};
const std::array<LocalOpInfo, kLocalOpCount>& localOps();
constexpr int kLocalWindowFirst = kLoClarity;
constexpr int kLocalWindowSlots = kLocalOpCount - kLocalWindowFirst;  // 5

struct LocalOps {
  std::array<float, kLocalOpCount> v{};  // all zero = no change (colour sat 0 = no tint)
  bool anyLinear() const;
  bool anyWindow() const;
};

struct LocalAdjustment {
  std::string id, name;
  bool enabled = true;
  bool invert = false;
  float strength = 100.f;  // 0..100
  std::vector<MaskComponent> mask;
  LocalOps ops;
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
