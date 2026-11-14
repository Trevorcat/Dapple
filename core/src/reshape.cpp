// Portrait reshaping parameters: JSON, clamping, hash (dapple/reshape.h; docs/engine/pipeline-architecture.md section 45).
#include "dapple/reshape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "json_value.h"
#include "local_eval.h"
#include "warp_field.h"

namespace dapple {

const int kReshapeFaceMesh[kReshapeFacePts] = {
    10,  109, 67,  103, 54,  21,  162, 127, 234, 93,  132, 58,  172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361,
    323, 454, 356, 389, 251, 284, 332, 297, 338, 468, 473, 33,  133, 362, 263, 159, 145, 386, 374, 64,  294, 1,   168, 61,  291, 13,  14};

namespace {
float fixv(float v, float lo, float hi, float def) {
  if (!std::isfinite(v)) return def;
  return std::clamp(v, lo, hi);
}
void fixPts(std::vector<float>& p, int stride) {
  for (std::size_t i = 0; i < p.size(); ++i) {
    const bool vis = stride == 3 && i % 3 == 2;
    p[i] = fixv(p[i], vis ? 0.f : -0.5f, vis ? 1.f : 1.5f, vis ? 0.f : 0.5f);
  }
}
void num(std::string& o, const char* key, float v) {
  char b[64];
  std::snprintf(b, sizeof b, ",\"%s\":%.6g", key, double(v));
  o += b;
}
void arr(std::string& o, const char* key, const std::vector<float>& v) {
  o += ",\"";
  o += key;
  o += "\":[";
  char b[32];
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::snprintf(b, sizeof b, i ? ",%.6g" : "%.6g", double(v[i]));
    o += b;
  }
  o += ']';
}
void str(std::string& o, const std::string& s) {
  jsonlite::JValue v;
  v.kind = jsonlite::JValue::String;
  v.str = s;
  jsonlite::writeValue(v, o);
}
bool readPts(const jsonlite::JValue* a, std::vector<float>& out) {
  if (!a || a->kind != jsonlite::JValue::Array) return false;
  out.clear();
  out.reserve(a->items.size());
  for (const auto& it : a->items) {
    float f = 0.f;
    if (!jsonlite::asNumber(it, f)) f = 0.f;
    out.push_back(f);
  }
  return true;
}
void readF(const jsonlite::JValue& o, const char* k, float& v) {
  if (const jsonlite::JValue* x = o.get(k)) jsonlite::asNumber(*x, v);
}
}  // namespace

bool ReshapeParams::active() const {
  for (const auto& f : faces)
    if (f.active()) return true;
  for (const auto& b : bodies)
    if (b.active()) return true;
  for (const auto& s : push)
    if (s.pts.size() >= (s.restore ? 2u : 4u)) return true;
  return false;
}

void clampReshape(ReshapeParams& r) {
  if (int(r.faces.size()) > kReshapeMaxFaces) r.faces.resize(kReshapeMaxFaces), local::logLine("reshape: more than 8 faces, extra dropped");
  if (int(r.bodies.size()) > kReshapeMaxBodies) r.bodies.resize(kReshapeMaxBodies), local::logLine("reshape: more than 4 bodies, extra dropped");
  if (int(r.push.size()) > kReshapeMaxStrokes) r.push.resize(kReshapeMaxStrokes), local::logLine("reshape: more than 64 strokes, extra dropped");
  if (r.matte.size() > 128) r.matte.clear();
  for (auto& f : r.faces) {
    f.index = std::clamp(f.index, 0, 64);
    if (f.pts.size() != 2u * kReshapeFacePts) f.pts.clear();
    fixPts(f.pts, 2);
    f.slim = fixv(f.slim, 0.f, 100.f, 0.f);
    f.small = fixv(f.small, 0.f, 100.f, 0.f);
    f.chin = fixv(f.chin, -100.f, 100.f, 0.f);
    f.eyes = fixv(f.eyes, 0.f, 100.f, 0.f);
    f.nose = fixv(f.nose, 0.f, 100.f, 0.f);
  }
  for (auto& b : r.bodies) {
    if (b.pts.size() != 3u * kReshapeBodyPts) b.pts.clear();
    fixPts(b.pts, 3);
    b.waist = fixv(b.waist, 0.f, 100.f, 0.f);
    b.arms = fixv(b.arms, 0.f, 100.f, 0.f);
    b.legs = fixv(b.legs, 0.f, 100.f, 0.f);
    b.longLegs = fixv(b.longLegs, 0.f, 100.f, 0.f);
    b.head = fixv(b.head, 0.f, 100.f, 0.f);
  }
  for (auto& s : r.push) {
    s.r = fixv(s.r, 0.005f, 0.25f, 0.04f);
    if (s.pts.size() % 2) s.pts.pop_back();
    if (int(s.pts.size()) > 2 * kReshapeMaxStrokePts) s.pts.resize(2 * kReshapeMaxStrokePts);
    fixPts(s.pts, 2);
  }
}

std::string reshapeToJson(const ReshapeParams& r) {
  std::string o = "{\"v\":1";
  if (!r.matte.empty()) {
    o += ",\"matte\":";
    str(o, r.matte);
  }
  if (!r.faces.empty()) {
    o += ",\"faces\":[";
    for (std::size_t i = 0; i < r.faces.size(); ++i) {
      const ReshapeFace& f = r.faces[i];
      if (i) o += ',';
      char b[32];
      std::snprintf(b, sizeof b, "{\"i\":%d", f.index);
      o += b;
      if (f.slim != 0) num(o, "slim", f.slim);
      if (f.small != 0) num(o, "small", f.small);
      if (f.chin != 0) num(o, "chin", f.chin);
      if (f.eyes != 0) num(o, "eyes", f.eyes);
      if (f.nose != 0) num(o, "nose", f.nose);
      arr(o, "pts", f.pts);
      o += '}';
    }
    o += ']';
  }
  if (!r.bodies.empty()) {
    o += ",\"bodies\":[";
    for (std::size_t i = 0; i < r.bodies.size(); ++i) {
      const ReshapeBody& b = r.bodies[i];
      if (i) o += ',';
      o += "{\"v\":1";
      if (b.waist != 0) num(o, "waist", b.waist);
      if (b.arms != 0) num(o, "arms", b.arms);
      if (b.legs != 0) num(o, "legs", b.legs);
      if (b.longLegs != 0) num(o, "longlegs", b.longLegs);
      if (b.head != 0) num(o, "head", b.head);
      arr(o, "pts", b.pts);
      o += '}';
    }
    o += ']';
  }
  if (!r.push.empty()) {
    o += ",\"push\":[";
    for (std::size_t i = 0; i < r.push.size(); ++i) {
      const ReshapePush& s = r.push[i];
      if (i) o += ',';
      char b[48];
      std::snprintf(b, sizeof b, "{\"r\":%.6g", double(s.r));
      o += b;
      if (s.restore) o += ",\"restore\":true";
      arr(o, "pts", s.pts);
      o += '}';
    }
    o += ']';
  }
  for (const auto& e : r.extra) {
    o += ',';
    str(o, e.first);
    o += ':';
    o += e.second;
  }
  o += '}';
  return o;
}

bool reshapeFromValue(const jsonlite::JValue& v, ReshapeParams& r) {
  r = ReshapeParams{};
  if (v.kind != jsonlite::JValue::Object) return false;
  for (const auto& m : v.members) {
    const std::string& k = m.first;
    const jsonlite::JValue& x = m.second;
    if (k == "v") continue;
    if (k == "matte") {
      if (x.kind == jsonlite::JValue::String) r.matte = x.str;
    } else if (k == "faces" && x.kind == jsonlite::JValue::Array) {
      for (const auto& it : x.items) {
        if (it.kind != jsonlite::JValue::Object) continue;
        ReshapeFace f;
        float idx = 0.f;
        readF(it, "i", idx);
        f.index = int(std::lround(idx));
        readF(it, "slim", f.slim);
        readF(it, "small", f.small);
        readF(it, "chin", f.chin);
        readF(it, "eyes", f.eyes);
        readF(it, "nose", f.nose);
        readPts(it.get("pts"), f.pts);
        r.faces.push_back(std::move(f));
      }
    } else if (k == "bodies" && x.kind == jsonlite::JValue::Array) {
      for (const auto& it : x.items) {
        if (it.kind != jsonlite::JValue::Object) continue;
        ReshapeBody b;
        readF(it, "waist", b.waist);
        readF(it, "arms", b.arms);
        readF(it, "legs", b.legs);
        readF(it, "longlegs", b.longLegs);
        readF(it, "head", b.head);
        readPts(it.get("pts"), b.pts);
        r.bodies.push_back(std::move(b));
      }
    } else if (k == "push" && x.kind == jsonlite::JValue::Array) {
      for (const auto& it : x.items) {
        if (it.kind != jsonlite::JValue::Object) continue;
        ReshapePush s;
        readF(it, "r", s.r);
        if (const jsonlite::JValue* rs = it.get("restore"); rs && rs->kind == jsonlite::JValue::Bool) s.restore = rs->boolean;
        readPts(it.get("pts"), s.pts);
        r.push.push_back(std::move(s));
      }
    } else {
      std::string raw;
      jsonlite::writeValue(x, raw);
      r.extra.emplace_back(k, raw);
    }
  }
  clampReshape(r);
  return true;
}

std::uint64_t reshapeHash(const ReshapeParams& r) {
  if (!r.active()) return 0;
  const std::string s = reshapeToJson(r);
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  if (!r.matte.empty()) h = (h ^ local::rasterContentHash(r.matte)) * 1099511628211ull;
  return h ? h : 1;
}

}  // namespace dapple
