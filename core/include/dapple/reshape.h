#pragma once
// Portrait reshaping (docs/engine/pipeline-architecture.md section 45): the parameters of the geometric warp stage, JSON key "reshape" of
// the params. Everything is resolution independent: points are in the photo's own normalised space (x, y in 0..1 of the decoded picture,
// before crop / rotate / straighten / flip, like the local masks), radii in units of the photo's long edge.
//
// The engine turns these into one smooth backward displacement field (core/src/warp_field.h) that is folded into the geometry fetch of
// the frame: every pixel, mask, AI plane and HDR field sample is read at the content's original position.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dapple {

// Face anchor points: a fixed subset of the 478 Face Mesh v2 landmarks, in this order (x, y pairs):
//   0..35  the face oval (FACEMESH_FACE_OVAL ring from 10 at the top, 234 = index 8, 152 (chin) = 18, 454 = 28; oval[i] mirrors oval[36 - i])
//   36 468 (iris centre, the person's right eye), 37 473 (iris centre, left eye), 38 33, 39 133, 40 362, 41 263 (eye corners),
//   42 159, 43 145, 44 386, 45 374 (lids), 46 64, 47 294 (nose wings), 48 1 (nose tip), 49 168 (nose bridge), 50 61, 51 291 (mouth corners),
//   52 13, 53 14 (inner lips)
constexpr int kReshapeFacePts = 54;
extern const int kReshapeFaceMesh[kReshapeFacePts];  // the mesh index of each anchor (reshape.cpp)
// Body anchor points: the 33 BlazePose landmarks (x, y, visibility 0..1) in their own order (0 nose, 11 / 12 shoulders, 13 / 14 elbows,
// 15 / 16 wrists, 23 / 24 hips, 25 / 26 knees, 27 / 28 ankles, 29 / 30 heels, 31 / 32 foot index, 7 / 8 ears).
constexpr int kReshapeBodyPts = 33;

struct ReshapeFace {
  int index = 0;            // the AI face index N (ids f<N>:...), informational
  std::vector<float> pts;   // 2 * kReshapeFacePts, photo-normalised
  // strengths: 0..100 (chin -100..100: + longer). Default 0 = no change.
  float slim = 0, small = 0, chin = 0, eyes = 0, nose = 0;
  bool active() const { return pts.size() == 2u * kReshapeFacePts && (slim != 0 || small != 0 || chin != 0 || eyes != 0 || nose != 0); }
};

struct ReshapeBody {
  std::vector<float> pts;   // 3 * kReshapeBodyPts (x, y, visibility)
  // 瘦身 waist, 瘦手臂 arms, 瘦腿 legs, 长腿 long legs, 头小 head: 0..100
  float waist = 0, arms = 0, legs = 0, longLegs = 0, head = 0;
  bool active() const {
    return pts.size() == 3u * kReshapeBodyPts && (waist != 0 || arms != 0 || legs != 0 || longLegs != 0 || head != 0);
  }
};

// Manual push (推): a stroke drags the picture under a soft round brush along its points; `restore` brings the strokes and operators
// under it back toward the original (还原).
struct ReshapePush {
  float r = 0.04f;          // brush radius, long-edge units (0.005 .. 0.25)
  bool restore = false;
  std::vector<float> pts;   // x, y pairs, photo-normalised, in drawing order (at least 2 points for a push)
};

struct ReshapeParams {
  std::string matte;        // raster id of the person plane (background protection); empty = no protection
  std::vector<ReshapeFace> faces;
  std::vector<ReshapeBody> bodies;
  std::vector<ReshapePush> push;
  std::vector<std::pair<std::string, std::string>> extra;  // unknown keys, kept verbatim (raw JSON) and written back

  bool empty() const { return faces.empty() && bodies.empty() && push.empty() && extra.empty() && matte.empty(); }
  bool active() const;      // anything that moves a pixel
};

// Limits (extra entries are dropped on read, one log line)
constexpr int kReshapeMaxFaces = 8, kReshapeMaxBodies = 4, kReshapeMaxStrokes = 64, kReshapeMaxStrokePts = 1024;

void clampReshape(ReshapeParams& r);
// {"v":1, "matte":..., "faces":[...], "bodies":[...], "push":[...]} in a fixed key order; only non-default values.
std::string reshapeToJson(const ReshapeParams& r);
// Hash of everything that changes pixels (0 when inactive; includes the matte plane's content hash).
std::uint64_t reshapeHash(const ReshapeParams& r);

}  // namespace dapple
