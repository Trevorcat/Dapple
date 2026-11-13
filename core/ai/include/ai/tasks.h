#pragma once
// Task wrappers: each hides one model's pre/post-processing exactly as the desktop reference (scripts/ai-eval/ai_ref.py) does and
// returns LOW-RESOLUTION planes in the model's own frame plus the metadata needed to map them back. Refinement to full resolution is
// the engine's job. Coordinates in results are pixels of the input image unless stated otherwise.
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "ai/ai.h"

namespace dapple::ai {

// 8-bit sRGB RGB image view (stride in bytes, >= 3*w).
struct Image {
  const uint8_t* rgb = nullptr;
  int w = 0, h = 0;
  int stride = 0;
};

struct TaskOptions {
  int threads = 0;
  const std::atomic<bool>* cancel = nullptr;
};

// ---- faces ------------------------------------------------------------------------------------------------------------------
struct Face {
  float score = 0;                     // BlazeFace score after sigmoid
  float cx = 0, cy = 0, w = 0, h = 0;  // box
  float kp[6][2] = {};                 // right eye, left eye, nose, mouth, right ear, left ear
};
// BlazeFace short range: letterbox to 128, anchors decode, weighted NMS (IoU 0.3), score >= minScore. Sorted by score.
Status detectFaces(const Image& im, std::vector<Face>& out, float minScore = 0.5f, const TaskOptions& o = {});

struct Landmarks {
  bool valid = false;     // face flag of the mesh model (a profile view or a false detection is rejected: valid=false)
  float faceFlag = 0;     // raw logit; > 0 means a face
  float xy[478][2] = {};  // image pixels
  float z[478] = {};      // crop-relative depth, in image pixels (same scale as xy)
  float crop[6] = {};     // the 256x256 crop -> image affine (a,b,c,d,e,f): x = a*u+b*v+c, y = d*u+e*v+f
};
// Rotated crop around `face` (eye line, 1.5 x box) and Face Mesh v2.
Status faceLandmarks(const Image& im, const Face& face, Landmarks& out, const TaskOptions& o = {});

// ---- person parts -----------------------------------------------------------------------------------------------------------
enum PersonClass : uint8_t { kBackground = 0, kHair = 1, kBodySkin = 2, kFaceSkin = 3, kClothes = 4, kOthers = 5 };
struct PersonParts {
  static constexpr int kSize = 256;
  std::vector<uint8_t> classMap;  // 256x256 argmax class (PersonClass); all background when !trusted
  std::vector<uint8_t> prob;      // 6 x 256 x 256, probability*255 (the raw network answer, also when !trusted)
  bool trusted = false;           // person gate result
  int faces = 0;                  // validated faces (detector + mesh flag)
  float personArea = 0;           // fraction of the frame the network calls person
  float blobArea = 0;             // fraction covered by the largest connected person blob
  const char* gateReason = "";    // "face", "region", "none"
};
// Selfie Multiclass 256 (the whole image squashed to 256x256, as the reference does) plus the person gate: the 6-class map is only
// trusted when a validated face is found, or the largest person blob is person-sized and has hair and face skin and clothes/body
// (profile and back-of-head portraits fail the face detector).
Status segmentPerson(const Image& im, PersonParts& out, const TaskOptions& o = {});

// ---- tap select ---------------------------------------------------------------------------------------------------------------
enum class TapEngine { MagicTouch, MobileSam };
// A prepared image (encoder run once, ~0.7 s on a mid phone) from which any number of taps are answered quickly.
class TapSession {
 public:
  virtual ~TapSession() = default;
  struct Result {
    int w = 0, h = 0;         // plane size (MagicTouch 768x768 = the whole image squashed; MobileSam 256x256 low-res logits->prob, see validW/H)
    int validW = 0, validH = 0;  // part of the plane that is image (MobileSam pads the long edge frame to a square)
    std::vector<float> mask;  // probability 0..1, w*h
    float iou = 0;            // MobileSam: predicted quality, MagicTouch: 0
  };
  // x, y in input image pixels. positive=false is a "negative" tap (MobileSam only).
  virtual Status tap(float x, float y, Result& out, bool positive = true, const TaskOptions& o = {}) = 0;
  virtual TapEngine engine() const = 0;
};
Status prepareTap(const Image& im, std::unique_ptr<TapSession>& out, TapEngine engine = TapEngine::MagicTouch, const TaskOptions& o = {});

// ---- depth --------------------------------------------------------------------------------------------------------------------
struct Depth {
  int w = 0, h = 0;      // 518x518, the whole image squashed to the square (as fed)
  std::vector<float> d;  // relative inverse depth (larger = nearer), raw network output
  float lo = 0, hi = 0;  // min, max of d
};
Status depth(const Image& im, Depth& out, const TaskOptions& o = {});

}  // namespace dapple::ai
