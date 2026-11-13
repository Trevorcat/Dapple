// Local adjustments: the op registry, clamping, JSON read / write, content hash (docs/engine/pipeline-architecture.md section 40.1).
#include "dapple/local_adjust.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "json_value.h"
#include "local_eval.h"

namespace dapple {

namespace {

using jsonlite::asNumber;
using jsonlite::JParser;
using jsonlite::JValue;

void logOnce(const std::string& what) {
  static std::mutex m;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lk(m);
  if (!seen.insert(what).second) return;
  local::logLine("local adjustments: " + what);
}

float fixf(float v, float lo, float hi, float def) {
  if (!std::isfinite(v)) v = def;
  return std::fmin(std::fmax(v, lo), hi);
}

const char* opName(MaskOp o) { return o == MaskOp::Add ? "add" : o == MaskOp::Subtract ? "subtract" : "intersect"; }

// shortest of %.6g / %.9g that reads back to the same float (a round trip is exact, typical values stay short)
std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", v);
  if (float(std::strtod(b, nullptr)) != float(v)) std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}
std::string esc(const std::string& s) {
  JValue v;
  v.kind = JValue::String;
  v.str = s;
  std::string o;
  jsonlite::writeValue(v, o);
  return o;
}

void rd(const JValue& o, const char* key, float& dst) {
  if (const JValue* v = o.get(key)) asNumber(*v, dst);
}
void rdb(const JValue& o, const char* key, bool& dst) {
  if (const JValue* v = o.get(key)) {
    if (v->kind == JValue::Bool) dst = v->boolean;
    else if (v->kind == JValue::Number) dst = v->number != 0;
  }
}
void rds(const JValue& o, const char* key, std::string& dst) {
  if (const JValue* v = o.get(key); v && v->kind == JValue::String) dst = v->str;
}

MaskKind kindOf(const std::string& s) {
  if (s == "linear") return MaskKind::Linear;
  if (s == "radial") return MaskKind::Radial;
  if (s == "luminance") return MaskKind::Luminance;
  if (s == "color" || s == "colour") return MaskKind::Colour;
  if (s == "brush") return MaskKind::Brush;
  if (s == "raster") return MaskKind::Raster;
  return MaskKind::Unknown;
}

bool readComponent(const JValue& c, MaskComponent& m) {
  if (c.kind != JValue::Object) return false;
  std::string kind;
  rds(c, "kind", kind);
  m = MaskComponent{};
  m.kind = kindOf(kind);
  if (m.kind == MaskKind::Unknown) {
    logOnce("unknown mask component kind '" + kind.substr(0, 40) + "' kept, not evaluated");
    jsonlite::writeValue(c, m.raw);
    return true;
  }
  std::string op;
  rds(c, "op", op);
  m.op = op == "subtract" ? MaskOp::Subtract : op == "intersect" ? MaskOp::Intersect : MaskOp::Add;
  rdb(c, "invert", m.invert);
  switch (m.kind) {
    case MaskKind::Linear:
      rd(c, "x0", m.x0), rd(c, "y0", m.y0), rd(c, "x1", m.x1), rd(c, "y1", m.y1);
      break;
    case MaskKind::Radial:
      rd(c, "cx", m.cx), rd(c, "cy", m.cy), rd(c, "rx", m.rx), rd(c, "ry", m.ry), rd(c, "angle", m.angle), rd(c, "feather", m.feather);
      break;
    case MaskKind::Luminance:
      rd(c, "lo", m.lo), rd(c, "hi", m.hi), rd(c, "feather", m.lumFeather);
      break;
    case MaskKind::Colour:
      rd(c, "hue", m.hue), rd(c, "range", m.hueRange), rd(c, "feather", m.hueFeather), rd(c, "satMin", m.satMin);
      break;
    case MaskKind::Brush:
      if (const JValue* ss = c.get("strokes"); ss && ss->kind == JValue::Array) {
        for (const JValue& s : ss->items) {
          if (s.kind != JValue::Object) continue;
          BrushStroke b;
          rd(s, "size", b.size), rd(s, "hardness", b.hardness), rd(s, "flow", b.flow);
          rdb(s, "erase", b.erase);
          if (const JValue* p = s.get("pts"); p && p->kind == JValue::Array) {
            b.pts.reserve(p->items.size());
            for (std::size_t i = 0; i + 1 < p->items.size(); i += 2) {
              float x = 0, y = 0;
              if (asNumber(p->items[i], x) && asNumber(p->items[i + 1], y)) {
                b.pts.push_back(x);
                b.pts.push_back(y);
              }
            }
          }
          if (!b.pts.empty()) m.strokes.push_back(std::move(b));
        }
      }
      break;
    case MaskKind::Raster:
      rds(c, "ref", m.ref);
      rd(c, "refine", m.refine);
      rd(c, "lo", m.rangeLo), rd(c, "hi", m.rangeHi), rd(c, "feather", m.rangeFeather);
      break;
    default: break;
  }
  return true;
}

bool readItem(const JValue& it, LocalAdjustment& a) {
  if (it.kind != JValue::Object) return false;
  a = LocalAdjustment{};
  rds(it, "id", a.id);
  rds(it, "name", a.name);
  rdb(it, "on", a.enabled);
  rdb(it, "invert", a.invert);
  rd(it, "strength", a.strength);
  if (const JValue* m = it.get("mask"); m && m->kind == JValue::Array) {
    for (const JValue& c : m->items) {
      MaskComponent mc;
      if (readComponent(c, mc)) a.mask.push_back(std::move(mc));
    }
  }
  if (const JValue* ad = it.get("adjust"); ad && ad->kind == JValue::Object) {
    const auto& ops = localOps();
    for (int k = 0; k < kLocalOpCount; ++k) {
      float x = 0.f;
      if (const JValue* v = ad->get(ops[std::size_t(k)].key); v && asNumber(*v, x)) a.ops.v[std::size_t(k)] = x / ops[std::size_t(k)].scale;
    }
  }
  return true;
}

void writeComponent(std::string& o, const MaskComponent& m) {
  if (m.kind == MaskKind::Unknown) {
    o += m.raw.empty() ? "{}" : m.raw;
    return;
  }
  o += "{\"kind\":\"";
  o += maskKindName(m.kind);
  o += "\",\"op\":\"";
  o += opName(m.op);
  o += "\"";
  if (m.invert) o += ",\"invert\":true";
  auto f = [&](const char* k, float v) {
    o += ",\"";
    o += k;
    o += "\":";
    o += num(v);
  };
  switch (m.kind) {
    case MaskKind::Linear: f("x0", m.x0), f("y0", m.y0), f("x1", m.x1), f("y1", m.y1); break;
    case MaskKind::Radial: f("cx", m.cx), f("cy", m.cy), f("rx", m.rx), f("ry", m.ry), f("angle", m.angle), f("feather", m.feather); break;
    case MaskKind::Luminance: f("lo", m.lo), f("hi", m.hi), f("feather", m.lumFeather); break;
    case MaskKind::Colour: f("hue", m.hue), f("range", m.hueRange), f("feather", m.hueFeather), f("satMin", m.satMin); break;
    case MaskKind::Brush:
      o += ",\"strokes\":[";
      for (std::size_t i = 0; i < m.strokes.size(); ++i) {
        const BrushStroke& s = m.strokes[i];
        if (i) o += ",";
        o += "{\"size\":" + num(s.size) + ",\"hardness\":" + num(s.hardness) + ",\"flow\":" + num(s.flow);
        if (s.erase) o += ",\"erase\":true";
        o += ",\"pts\":[";
        for (std::size_t j = 0; j < s.pts.size(); ++j) {
          if (j) o += ",";
          o += num(s.pts[j]);
        }
        o += "]}";
      }
      o += "]";
      break;
    case MaskKind::Raster:
      o += ",\"ref\":" + esc(m.ref);
      f("refine", m.refine);
      if (m.hasRange()) f("lo", m.rangeLo), f("hi", m.rangeHi), f("feather", m.rangeFeather);
      break;
    default: break;
  }
  o += "}";
}

}  // namespace

const std::array<LocalOpInfo, kLocalOpCount>& localOps() {
  static const std::array<LocalOpInfo, kLocalOpCount> t = {{
      {"exposure", -4.f, 4.f, 20.f, LocalStage::Linear},
      {"contrast", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"highlights", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"shadows", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"whites", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"blacks", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"temp", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"tint", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"saturation", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"vibrance", -100.f, 100.f, 1.f, LocalStage::Linear},
      {"color_hue", 0.f, 360.f, 1.f, LocalStage::Linear},
      {"color_sat", 0.f, 100.f, 1.f, LocalStage::Linear},
      {"clarity", -100.f, 100.f, 1.f, LocalStage::Window},
      {"texture", -100.f, 100.f, 1.f, LocalStage::Window},
      {"sharpen", -100.f, 100.f, 1.f, LocalStage::Window},
      {"noise", 0.f, 100.f, 1.f, LocalStage::Window},
      {"dehaze", -100.f, 100.f, 1.f, LocalStage::Window},
  }};
  return t;
}

bool LocalOps::anyLinear() const {
  for (int k = 0; k < kLocalWindowFirst; ++k)
    if (k != kLoColorHue && v[std::size_t(k)] != 0.f) return true;
  return false;
}
bool LocalOps::anyWindow() const {
  for (int k = kLocalWindowFirst; k < kLocalOpCount; ++k)
    if (v[std::size_t(k)] != 0.f) return true;
  return false;
}

const char* maskKindName(MaskKind k) {
  switch (k) {
    case MaskKind::Linear: return "linear";
    case MaskKind::Radial: return "radial";
    case MaskKind::Luminance: return "luminance";
    case MaskKind::Colour: return "color";
    case MaskKind::Brush: return "brush";
    case MaskKind::Raster: return "raster";
    default: return "unknown";
  }
}

void clampLocal(std::vector<LocalAdjustment>& list) {
  if (int(list.size()) > kMaxLocalAdjustments) {
    logOnce("more than 16 local adjustments: the extra ones are ignored");
    list.resize(kMaxLocalAdjustments);
  }
  const auto& ops = localOps();
  int points = 0;
  for (LocalAdjustment& a : list) {
    if (a.id.size() > 64) a.id.resize(64);
    if (a.name.size() > 128) a.name.resize(128);
    a.strength = fixf(a.strength, 0.f, 100.f, 100.f);
    for (int k = 0; k < kLocalOpCount; ++k) {
      float& v = a.ops.v[std::size_t(k)];
      v = fixf(v, ops[std::size_t(k)].lo, ops[std::size_t(k)].hi, 0.f);
    }
    if (int(a.mask.size()) > kMaxMaskComponents) {
      logOnce("more than 8 mask components: the extra ones are ignored");
      a.mask.resize(kMaxMaskComponents);
    }
    for (MaskComponent& m : a.mask) {
      m.x0 = fixf(m.x0, -4.f, 5.f, 0.5f), m.y0 = fixf(m.y0, -4.f, 5.f, 0.f);
      m.x1 = fixf(m.x1, -4.f, 5.f, 0.5f), m.y1 = fixf(m.y1, -4.f, 5.f, 0.5f);
      m.cx = fixf(m.cx, -4.f, 5.f, 0.5f), m.cy = fixf(m.cy, -4.f, 5.f, 0.5f);
      m.rx = fixf(m.rx, 0.001f, 4.f, 0.25f), m.ry = fixf(m.ry, 0.001f, 4.f, 0.25f);
      m.angle = fixf(m.angle, -3600.f, 3600.f, 0.f);
      m.angle = std::fmod(m.angle, 360.f);
      m.feather = fixf(m.feather, 0.f, 100.f, 50.f);
      m.lo = fixf(m.lo, 0.f, 100.f, 0.f), m.hi = fixf(m.hi, 0.f, 100.f, 100.f);
      if (m.hi < m.lo) std::swap(m.lo, m.hi);
      m.lumFeather = fixf(m.lumFeather, 0.f, 50.f, 10.f);
      m.hue = fixf(m.hue, -3600.f, 3600.f, 0.f);
      m.hue = std::fmod(std::fmod(m.hue, 360.f) + 360.f, 360.f);
      m.hueRange = fixf(m.hueRange, 0.f, 180.f, 30.f);
      m.hueFeather = fixf(m.hueFeather, 0.f, 90.f, 20.f);
      m.satMin = fixf(m.satMin, 0.f, 100.f, 10.f);
      m.refine = fixf(m.refine, 0.f, 100.f, 50.f);
      m.rangeLo = fixf(m.rangeLo, 0.f, 100.f, 0.f), m.rangeHi = fixf(m.rangeHi, 0.f, 100.f, 100.f);
      if (m.rangeHi < m.rangeLo) std::swap(m.rangeLo, m.rangeHi);
      m.rangeFeather = fixf(m.rangeFeather, 0.f, 50.f, 10.f);
      if (m.ref.size() > 128) m.ref.resize(128);
      for (BrushStroke& s : m.strokes) {
        s.size = fixf(s.size, 0.0005f, 0.5f, 0.05f);
        s.hardness = fixf(s.hardness, 0.f, 100.f, 50.f);
        s.flow = fixf(s.flow, 0.f, 100.f, 100.f);
        for (float& p : s.pts) p = fixf(p, -4.f, 5.f, 0.f);
        if (points + int(s.pts.size() / 2) > kMaxBrushPoints) {
          logOnce("more than 100000 brush points: the extra ones are ignored");
          s.pts.resize(std::size_t(std::max(0, kMaxBrushPoints - points)) * 2);
        }
        points += int(s.pts.size() / 2);
      }
      m.strokes.erase(std::remove_if(m.strokes.begin(), m.strokes.end(), [](const BrushStroke& s) { return s.pts.empty(); }), m.strokes.end());
    }
  }
}

std::string localToJson(const std::vector<LocalAdjustment>& list) {
  if (list.empty()) return "";
  const auto& ops = localOps();
  std::string o = "{\"v\":1,\"items\":[";
  for (std::size_t i = 0; i < list.size(); ++i) {
    const LocalAdjustment& a = list[i];
    if (i) o += ",";
    o += "{\"id\":" + esc(a.id);
    if (!a.name.empty()) o += ",\"name\":" + esc(a.name);
    if (!a.enabled) o += ",\"on\":false";
    if (a.invert) o += ",\"invert\":true";
    o += ",\"strength\":" + num(a.strength);
    o += ",\"mask\":[";
    for (std::size_t c = 0; c < a.mask.size(); ++c) {
      if (c) o += ",";
      writeComponent(o, a.mask[c]);
    }
    o += "],\"adjust\":{";
    bool first = true;
    for (int k = 0; k < kLocalOpCount; ++k) {
      const float v = a.ops.v[std::size_t(k)];
      if (v == 0.f) continue;
      if (!first) o += ",";
      first = false;
      o += "\"";
      o += ops[std::size_t(k)].key;
      o += "\":" + num(double(v) * ops[std::size_t(k)].scale);
    }
    o += "}}";
  }
  o += "]}";
  return o;
}

bool localFromValue(const jsonlite::JValue& v, std::vector<LocalAdjustment>& out) {
  out.clear();
  const JValue* items = nullptr;
  if (v.kind == JValue::Array) {
    items = &v;
  } else if (v.kind == JValue::Object) {
    float ver = 1.f;
    rd(v, "v", ver);
    if (ver > 1.f) logOnce("local adjustments of a newer schema version: reading what this build knows");
    items = v.get("items");
  }
  if (!items || items->kind != JValue::Array) return true;
  for (const JValue& it : items->items) {
    LocalAdjustment a;
    if (readItem(it, a)) out.push_back(std::move(a));
  }
  clampLocal(out);
  return true;
}

bool localFromJson(const std::string& json, std::vector<LocalAdjustment>& out) {
  JValue root;
  JParser p(json);
  if (!p.parse(root)) return false;
  return localFromValue(root, out);
}

std::uint64_t localHash(const std::vector<LocalAdjustment>& list) {
  std::uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&](std::uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0x100000001B3ull;
  };
  auto f = [&](float v) {
    if (v == 0.f) v = 0.f;
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    mix(b);
  };
  auto s = [&](const std::string& str) {
    mix(str.size());
    for (char c : str) mix(std::uint8_t(c));
  };
  for (const LocalAdjustment& a : list) {
    if (!a.enabled) continue;
    mix(0xA0 | (a.invert ? 1 : 0));
    f(a.strength);
    for (float v : a.ops.v) f(v);
    for (const MaskComponent& m : a.mask) {
      mix(std::uint64_t(m.kind) << 8 | std::uint64_t(m.op) << 4 | (m.invert ? 1 : 0));
      switch (m.kind) {
        case MaskKind::Linear: f(m.x0), f(m.y0), f(m.x1), f(m.y1); break;
        case MaskKind::Radial: f(m.cx), f(m.cy), f(m.rx), f(m.ry), f(m.angle), f(m.feather); break;
        case MaskKind::Luminance: f(m.lo), f(m.hi), f(m.lumFeather); break;
        case MaskKind::Colour: f(m.hue), f(m.hueRange), f(m.hueFeather), f(m.satMin); break;
        case MaskKind::Brush:
          mix(m.strokes.size());
          for (const BrushStroke& st : m.strokes) {
            f(st.size), f(st.hardness), f(st.flow);
            mix(st.erase ? 1 : 0);
            mix(st.pts.size());
            for (float p : st.pts) f(p);
          }
          break;
        case MaskKind::Raster:
          s(m.ref);
          f(m.refine);
          mix(local::rasterContentHash(m.ref));
          if (m.hasRange()) f(m.rangeLo), f(m.rangeHi), f(m.rangeFeather);  // identity range: the hash of older edits is unchanged
          break;
        default: break;
      }
    }
  }
  return h;
}

}  // namespace dapple
