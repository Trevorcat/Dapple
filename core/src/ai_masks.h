#pragma once
// AI masks as an engine service (docs/engine/pipeline-architecture.md section 41): raster planes (8 bit, long edge <= 1024, the photo's own
// normalised space like every mask) produced by the on-device models of core/ai (person parts, face features, tap select, depth) and a
// classical prior (sky), registered in the raster store of the local adjustments (local_eval.h) under stable string ids
//
//   ai:<photo>:<key>      <photo> = 16 hex digits of photoIdOf(); key = hair faceskin bodyskin clothes person background | depth | sky |
//                         f<N>:face|eyes|iris|sclera|lips|teeth|mouth|brows|undereye (N = 1.. faces, largest first) | tap:<taps>  (taps = "+412,531;-200,700")
//
// The id alone says what to compute, so a plane that is missing (cache wipe, another device) can be re-created from its id.
//
// Layers: (1) pure builders (this header, ai_masks.cpp): polygon rasteriser, part splitting, tap accumulation, depth plane, sky prior, cache
// file format, id grammar. No runtime. (2) rt:: (ai_masks_service.cpp): the only part that touches core/ai (compiled to "unavailable" stubs when
// DAPPLE_AI is off). (3) Service (ai_masks.cpp): jobs on the background pool with progress + cancel, disk cache, tap sessions, ensure().
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dapple::aimask {

// ---- planes and builders (no runtime) ----------------------------------------------------------------------------------------------
struct Plane {
  int w = 0, h = 0;
  std::vector<std::uint8_t> px;  // w * h, 0..255
  bool empty() const { return w <= 0 || h <= 0 || px.empty(); }
};
struct Pt {
  float x = 0, y = 0;
};

// Even-odd fill of closed rings (pixel coordinates, pixel centres at +0.5) with exact horizontal and 4x vertical coverage, then a gaussian
// edge softening of `softSigma` px (0 = hard antialiased edge).
Plane polygonPlane(int w, int h, const std::vector<std::vector<Pt>>& rings, float softSigma);
Plane resizePlane(const std::uint8_t* src, int sw, int sh, int dw, int dh);  // bilinear
Plane resizePlaneF(const float* src, int sw, int sh, int dw, int dh, float lo, float hi);  // bilinear, (v - lo) / (hi - lo) -> 0..255
void planeMul(Plane& a, const Plane& b);   // a *= b
void planeSub(Plane& a, const Plane& b);   // a *= (1 - b)
void planeMax(Plane& a, const Plane& b);   // a = max(a, b)
void planeInvert(Plane& a);
double planeSum(const Plane& a);           // sum / 255

// Face mesh topology (MediaPipe Face Mesh, 478 points; Apache-2.0, face_mesh_connections.py of google-ai-edge/mediapipe, FACEMESH_* sets chained
// into rings).
extern const int kFaceOval[36];
extern const int kLipsOuter[20], kLipsInner[20];
extern const int kEyeRight[16], kEyeLeft[16];            // the person's right eye = image left (mesh convention)
extern const int kIrisRight[4], kIrisLeft[4];            // 469..472, 474..477 (centre 468 / 473)
extern const int kBrowRightUpper[5], kBrowRightLower[5], kBrowLeftUpper[5], kBrowLeftLower[5];

struct FaceMesh {
  float xy[478][2] = {};  // pixels of the input image
  float z[478] = {};      // same scale
};
struct FaceFeatures {
  bool frontal = false;   // |yaw| < kFrontalYawDeg (yaw from the 3D distance between points 234 and 454)
  float yawDeg = 0;
  float mouthOpen = 0;    // inner lip gap (13-14) / inner mouth width (78-308)
  bool open = false;      // mouthOpen >= kMouthOpen and the inner polygon holds visible teeth
  Plane face;             // always (oval)
  Plane eyes, iris, sclera, lips, teeth, brows;  // frontal faces only (teeth: open mouth with bright low-chroma pixels inside)
  Plane mouth;            // frontal faces, open mouth (mouthOpen >= kMouthOpen): the filled inner-lip loop, whether or not teeth show (43.4/43.6)
  float box[4] = {0, 0, 0, 0};  // mesh extent, pixels of the input image: x0 y0 x1 y1
  float cx = 0, cy = 0;   // midpoint of the iris centres, pixels
  Plane undereye;         // frontal faces only: the crescent below each eye (portrait retouch, docs/engine/pipeline-architecture.md 43.4)
  float iod = 0;          // inter-ocular distance (iris centres 468 / 473), pixels of the input image
};
// The under-eye crescents of a face (docs section 43.4), eyes = the lid-opening plane it is clipped by (may be empty).
Plane underEyePlane(const FaceMesh& m, int w, int h, const Plane& eyes);
constexpr float kFrontalYawDeg = 28.f;
constexpr float kMouthOpen = 0.10f;
// rgb may be null (the teeth plane is then the inner mouth polygon of an open mouth, unrefined).
bool faceFeatures(const FaceMesh& m, int w, int h, const std::uint8_t* rgb, int stride, FaceFeatures& out);

// The feature keys ("face", "eyes", ...) of a face that exist as planes: the entries of the faces group, in this order.
std::vector<std::string> faceFeatureKeys(const FaceFeatures& f);
// Group meta of the faces group: {"count":n,"list":[{"index":1,"box":[x0,y0,x1,y1],"yaw":..,"frontal":..,"mouth_open":..,"teeth":..,"iod":..,"center":[x,y],"planes":[..]}]}
// (photo normalised; iod = inter-ocular distance / long edge; index = 1-based, largest first). `faces` in the order of the planes ids f1, f2 ...
std::string facesMetaJson(const std::vector<FaceFeatures>& faces, int w, int h);
// The "list":[...] array text of a faces meta ("[]" when absent).
std::string facesListOfMeta(const std::string& meta);

struct PartPlanes {
  Plane hair, faceSkin, bodySkin, clothes, person, background;
};
enum { kPartBackground = 0, kPartHair = 1, kPartBodySkin = 2, kPartFaceSkin = 3, kPartClothes = 4, kPartOthers = 5 };
// prob: 6 x S x S bytes (class probability * 255, ai::PersonParts::prob), upsampled bilinear to w x h. person = 255 - background (so "others" belong
// to the person), background = the network's background.
PartPlanes splitParts(const std::uint8_t* prob, int S, int w, int h);
std::vector<std::uint8_t> probFromClassMap(const std::vector<std::uint8_t>& classMap, int S);  // hard 255 / 0 planes (tests)

// Inverse depth (larger = nearer, any scale, dw x dh) -> far plane at w x h: 0 = nearest, 255 = farthest, robust min / max (2nd and 98th percentile).
Plane depthFarPlane(const float* d, int dw, int dh, int w, int h);

// Tap accumulation: each tap's object mask (soft, plane resolution) added (max) or subtracted (a * (1 - m)) in order.
struct TapMask {
  bool add = true;
  Plane mask;
};
Plane accumulateTaps(const std::vector<TapMask>& taps, int w, int h);

// Classical sky prior: blue / bright, low texture, connected to the top edge, optionally with the far part of a depth plane (same size or 0).
// Returns an empty plane when no sky-like region of at least `minArea` of the frame exists.
Plane skyPrior(const std::uint8_t* rgb, int w, int h, int stride, const Plane* farDepth, float* confidence);

// ---- ids ----------------------------------------------------------------------------------------------------------------------------
struct TapPoint {
  float x = 0, y = 0;  // photo normalised
  bool add = true;
};
constexpr int kMaxTaps = 10;
std::string tapSpec(const std::vector<TapPoint>& taps);                   // "+412,531;-200,700"
bool parseTapSpec(const std::string& s, std::vector<TapPoint>& out);
struct ParsedId {
  bool ok = false;
  std::string photo;
  std::string group;   // person | faces | depth | sky | tap
  std::string key;     // the part after "ai:<photo>:"
  int face = 0;        // faces: 1-based
  std::string feature; // faces: face eyes ...
  std::vector<TapPoint> taps;
};
ParsedId parseId(const std::string& id);
std::string makeId(const std::string& photo, const std::string& key);

// ---- cache files ----------------------------------------------------------------------------------------------------------------------
constexpr int kEpoch = 6;  // algorithm version: bump when a builder changes (old cache files are then misses)
struct SetEntry {
  std::string key;  // "hair", "f1:lips" ...
  Plane plane;
};
struct PlaneSet {
  std::vector<SetEntry> entries;
  std::string meta;  // JSON object text of the group (gate result, face list ...)
};
std::vector<std::uint8_t> packSet(const PlaneSet& s);
bool unpackSet(const std::uint8_t* data, std::size_t n, PlaneSet& out);  // false on any inconsistency (checksum, sizes, epoch)
// 16 hex digits identifying the photo content: size + a 256 x 256 sample of the decoded scene-linear picture (16 bit quantised).
std::string photoIdOf(const float* linearRgb, int w, int h);

// ---- runtime layer (ai_masks_service.cpp) ------------------------------------------------------------------------------------------
struct Img {
  const std::uint8_t* rgb = nullptr;
  int w = 0, h = 0, stride = 0;
};
namespace rt {
bool compiled();  // DAPPLE_AI
struct Avail {
  bool person = false, faces = false, depth = false, tap = false;
  std::string tapEngine;  // "magictouch" | "mobilesam" | ""
  std::string backends;   // "litert+ort" ...
  std::string reason;     // why something is missing (first line)
};
Avail availability(const std::string& modelDir);
struct Res {
  bool ok = false, cancelled = false;
  std::string err;
};
struct PersonOut {
  bool trusted = false;
  std::string gate;
  int faces = 0;
  std::vector<std::uint8_t> prob;  // 6 x 256 x 256
};
Res person(const Img&, PersonOut&, const std::atomic<bool>* cancel);
// `personMask` (optional, 0..255 plane of the person group): person regions without a validated face get extra detector windows (small and several faces)
Res faces(const Img&, const Plane* personMask, int maxFaces, std::vector<FaceMesh>&, const std::atomic<bool>* cancel);
struct DepthOut {
  int w = 0, h = 0;
  std::vector<float> d;
};
Res depth(const Img&, DepthOut&, const std::atomic<bool>* cancel);
struct TapOut {
  int w = 0, h = 0, validW = 0, validH = 0;
  std::vector<float> mask;
};
class TapHandle {
 public:
  virtual ~TapHandle() = default;
  virtual Res tap(float x, float y, TapOut& out, const std::atomic<bool>* cancel) = 0;  // x, y in pixels of the prepared image
};
Res prepareTap(const Img&, const std::string& modelDir, std::unique_ptr<TapHandle>& out, const std::atomic<bool>* cancel);
void configure(const std::string& modelDir, int idleReleaseMs);
int releaseIdle();
int releaseAll();
std::size_t resident();
}  // namespace rt

// ---- service ----------------------------------------------------------------------------------------------------------------------------
enum Task : int { kTaskPerson = 1, kTaskFaces = 2, kTaskDepth = 4, kTaskSky = 8, kTaskTapPrepare = 16 };
constexpr int kMaxFaces = 4;
constexpr int kPlaneLongEdge = 1024;

// What a job runs on: the identity of the photo and a way to get its neutral rendition (called at most once, on the job thread).
struct Rendition {
  int w = 0, h = 0;
  std::vector<std::uint8_t> rgb;  // w * h * 3, sRGB 8 bit, long edge <= kPlaneLongEdge
};
struct Source {
  std::string photoId;
  std::function<bool(Rendition&, std::string& err)> render;
  void* owner = nullptr;  // job tag (the session): cancelOwner / waitOwner
};

struct PlaneInfo {
  std::string id, key, label, group;
  int face = 0;       // faces: 1-based, else 0
  int w = 0, h = 0;
  bool fromCache = false;
};
struct GroupStatus {
  std::string group;   // person faces depth sky
  std::string state;   // ok | cached | none | unavailable | error | cancelled | skipped
  std::string message;
  double ms = 0;
};
struct Result {
  std::string state = "done";  // done | cancelled | error
  std::string message;
  std::vector<PlaneInfo> planes;
  std::vector<GroupStatus> groups;
  bool personFound = false;
  int faceCount = 0;
  std::string facesList;  // JSON array of the faces (facesMetaJson "list"), "face_list" of the job JSON
  bool skyFound = false;
  double renderMs = 0, totalMs = 0;
  std::string json() const;
};

struct Control {
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(float, const char*)> progress;  // 0..1, stage
  bool yield = true;                                  // wait while dapple_set_interaction(1) between stages
};

void configure(const std::string& modelDir, const std::string& cacheDir);
std::string cacheDir();
std::string modelDir();
std::string availabilityJson();
// Writes a group (person faces depth sky) of a photo into the disk cache (tests; restoring planes from another source).
bool storeGroup(const std::string& photo, const std::string& group, const PlaneSet& set);
// Computes the groups in `tasks` (cache first), registers every plane in the raster store. Synchronous (the caller's thread).
Result compute(const Source& src, int tasks, int maxFaces, bool force, const Control& ctl);

// Jobs on a background thread (utility priority, one at a time, yielding to interaction). Returns the job id.
int startJob(const Source& src, int tasks, int maxFaces, bool force);
std::string jobJson(int job);
void cancelJob(int job);
void cancelOwner(void* owner);
void waitOwner(void* owner);  // returns when no job of `owner` runs
bool waitJob(int job, int timeoutMs);

// Tap select. State per photo; taps accumulate into one plane. op: 0 add, 1 subtract, 2 undo last, 3 reset.
struct TapResult {
  bool ok = false;
  std::string message, id;
  int w = 0, h = 0;
  std::vector<TapPoint> taps;
  double ms = 0, prepareMs = 0;
};
TapResult tap(const Source& src, float u, float v, int op, const Control& ctl);

// Re-creates the planes `ids` (all with this photo's id) that are not in the raster store: disk cache, else recomputation. Returns the ids that
// still are missing, with the reason in `why` (first reason).
std::vector<std::string> ensure(const Source& src, const std::vector<std::string>& ids, const Control& ctl, std::string* why);
bool isAiId(const std::string& id);

// Housekeeping: frees model memory after the idle time (also called by the janitor thread), tap sessions included.
void releaseIdle();
void releaseAll();
void pruneCache(std::uint64_t maxBytes);

}  // namespace dapple::aimask
