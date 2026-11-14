// The runtime layer of the AI masks: the only file that talks to core/ai. With DAPPLE_AI off every call reports "unavailable".
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "ai_masks.h"

#ifdef DAPPLE_AI
#include "ai/ai.h"
#include "ai/tasks.h"
#endif

namespace dapple::aimask::rt {

#ifndef DAPPLE_AI

bool compiled() { return false; }
Avail availability(const std::string&) {
  Avail a;
  a.reason = "AI masks are not part of this build (DAPPLE_AI off)";
  return a;
}
static Res off() { return {false, false, "AI masks are not part of this build"}; }
Res person(const Img&, PersonOut&, const std::atomic<bool>*) { return off(); }
Res faces(const Img&, const Plane*, int, std::vector<FaceMesh>&, const std::atomic<bool>*) { return off(); }
Res poses(const Img&, int, std::vector<PoseOut>&, const std::atomic<bool>*) { return off(); }
Res depth(const Img&, DepthOut&, const std::atomic<bool>*) { return off(); }
Res prepareTap(const Img&, const std::string&, std::unique_ptr<TapHandle>&, const std::atomic<bool>*) { return off(); }
void configure(const std::string&, int) {}
int releaseIdle() { return 0; }
int releaseAll() { return 0; }
std::size_t resident() { return 0; }

#else

namespace {
namespace fs = std::filesystem;

constexpr bool kLiteRt =
#ifdef DAPPLE_AI_LITERT
    true;
#else
    false;
#endif
constexpr bool kOrt =
#ifdef DAPPLE_AI_ORT
    true;
#else
    false;
#endif

bool modelExists(const std::string& dir, const char* id, bool* viaLiteRt = nullptr) {
  std::error_code ec;
  const fs::path base = fs::path(std::u8string(dir.begin(), dir.end()));
  if (kLiteRt && fs::exists(base / (std::string(id) + ".tflite"), ec)) {
    if (viaLiteRt) *viaLiteRt = true;
    return true;
  }
  if (kOrt && (fs::exists(base / (std::string(id) + ".ort"), ec) || fs::exists(base / (std::string(id) + ".onnx"), ec))) {
    if (viaLiteRt) *viaLiteRt = false;
    return true;
  }
  return false;
}

ai::Image toImage(const Img& i) {
  ai::Image im;
  im.rgb = i.rgb, im.w = i.w, im.h = i.h, im.stride = i.stride;
  return im;
}
Res fromStatus(const ai::Status& s) {
  Res r;
  r.ok = s.ok();
  r.cancelled = s.code == ai::Code::Cancelled;
  if (!r.ok) r.err = s.message.empty() ? "model run failed" : s.message;
  return r;
}
ai::TaskOptions opts(const std::atomic<bool>* c) {
  ai::TaskOptions o;
  o.cancel = c;
  return o;
}

class Handle final : public TapHandle {
 public:
  std::unique_ptr<ai::TapSession> s;
  Res tap(float x, float y, TapOut& out, const std::atomic<bool>* cancel) override {
    ai::TapSession::Result r;
    const ai::Status st = s->tap(x, y, r, true, opts(cancel));
    if (!st.ok()) return fromStatus(st);
    out.w = r.w, out.h = r.h, out.validW = r.validW, out.validH = r.validH, out.mask = std::move(r.mask);
    return {true, false, ""};
  }
};
}  // namespace

bool compiled() { return true; }

Avail availability(const std::string& dir) {
  Avail a;
  a.backends = std::string(kLiteRt ? "litert" : "") + (kLiteRt && kOrt ? "+" : "") + (kOrt ? "ort" : "");
  auto need = [&](std::initializer_list<const char*> ids) {
    for (const char* id : ids)
      if (!modelExists(dir, id)) {
        if (a.reason.empty()) a.reason = std::string("model file missing or runtime not built: ") + id;
        return false;
      }
    return true;
  };
  a.person = need({"person_parts"});
  a.faces = need({"face_detector", "face_landmarks"});
  a.depth = need({"depth"});
  if (kLiteRt && modelExists(dir, "tap_encoder") && modelExists(dir, "tap_decoder")) {
    a.tap = true, a.tapEngine = "magictouch";
  } else if (kOrt && modelExists(dir, "sam_encoder") && modelExists(dir, "sam_decoder")) {
    a.tap = true, a.tapEngine = "mobilesam";
  } else if (a.reason.empty()) {
    a.reason = "tap models missing";
  }
  if (a.person && a.faces && a.depth && a.tap) a.reason.clear();
  a.pose = modelExists(dir, "pose_detector") && modelExists(dir, "pose_landmarks");  // optional: never the reason of the others
  return a;
}

Res person(const Img& im, PersonOut& out, const std::atomic<bool>* cancel) {
  ai::PersonParts p;
  const ai::Status st = ai::segmentPerson(toImage(im), p, opts(cancel));
  if (!st.ok()) return fromStatus(st);
  out.trusted = p.trusted, out.gate = p.gateReason ? p.gateReason : "", out.faces = p.faces, out.prob = std::move(p.prob);
  return {true, false, ""};
}

Res faces(const Img& im, const Plane* personMask, int maxFaces, std::vector<FaceMesh>& out, const std::atomic<bool>* cancel) {
  out.clear();
  std::vector<ai::FoundFace> f;  // mesh-validated only (profile views and false detections are dropped)
  const bool hint = personMask && !personMask->empty();
  const ai::Status st = ai::findFaces(toImage(im), hint ? personMask->px.data() : nullptr, hint ? personMask->w : 0, hint ? personMask->h : 0, f, opts(cancel));
  if (!st.ok()) return fromStatus(st);
  std::stable_sort(f.begin(), f.end(), [](const ai::FoundFace& a, const ai::FoundFace& b) { return a.face.w * a.face.h > b.face.w * b.face.h; });  // largest first
  for (const ai::FoundFace& ff : f) {
    if (int(out.size()) >= maxFaces) break;
    FaceMesh m;
    for (int i = 0; i < 478; ++i) m.xy[i][0] = ff.lm.xy[i][0], m.xy[i][1] = ff.lm.xy[i][1], m.z[i] = ff.lm.z[i];
    out.push_back(m);
  }
  return {true, false, ""};
}

Res poses(const Img& im, int maxPoses, std::vector<PoseOut>& out, const std::atomic<bool>* cancel) {
  out.clear();
  std::vector<ai::Pose> p;
  const ai::Status st = ai::findPoses(toImage(im), p, maxPoses, opts(cancel));
  if (!st.ok()) return fromStatus(st);
  for (const ai::Pose& q : p) {
    PoseOut o;
    for (int i = 0; i < 33; ++i) o.xy[i][0] = q.xy[i][0], o.xy[i][1] = q.xy[i][1], o.vis[i] = q.vis[i];
    o.score = q.score;
    out.push_back(o);
  }
  return {true, false, ""};
}

Res depth(const Img& im, DepthOut& out, const std::atomic<bool>* cancel) {
  ai::Depth d;
  const ai::Status st = ai::depth(toImage(im), d, opts(cancel));
  if (!st.ok()) return fromStatus(st);
  out.w = d.w, out.h = d.h, out.d = std::move(d.d);
  return {true, false, ""};
}

Res prepareTap(const Img& im, const std::string& dir, std::unique_ptr<TapHandle>& out, const std::atomic<bool>* cancel) {
  const Avail a = availability(dir);
  if (!a.tap) return {false, false, a.reason};
  auto h = std::make_unique<Handle>();
  const ai::Status st = ai::prepareTap(toImage(im), h->s, a.tapEngine == "magictouch" ? ai::TapEngine::MagicTouch : ai::TapEngine::MobileSam, opts(cancel));
  if (!st.ok()) return fromStatus(st);
  out = std::move(h);
  return {true, false, ""};
}

void configure(const std::string& dir, int idleMs) {
  ai::Config c = ai::config();
  c.modelDir = dir;
  c.idleReleaseMs = idleMs;
  ai::setConfig(c);
}
int releaseIdle() { return ai::releaseIdle(); }
int releaseAll() { return ai::releaseAll(); }
std::size_t resident() { return ai::residentBytes(); }

#endif

}  // namespace dapple::aimask::rt
