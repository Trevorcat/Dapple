// EditParams JSON reader: the inverse of toJson(). A small hand-written,
// tolerant parser, enough for our schema (no external dependency).

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "dapple/edit_params.h"
#include "edit_params_fields.h"

namespace dapple {

namespace {

struct JValue {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  double number = 0;
  bool boolean = false;
  std::string str;
  std::vector<JValue> items;
  std::vector<std::pair<std::string, JValue>> members;

  const JValue* get(const char* key) const {
    if (kind != Object) return nullptr;
    for (const auto& m : members)
      if (m.first == key) return &m.second;
    return nullptr;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& s) : s_(s) {}

  bool parse(JValue& out) {
    skipWs();
    if (!value(out, 0)) return false;
    skipWs();
    return pos_ == s_.size();
  }
  std::string error() const { return "JSON syntax error at offset " + std::to_string(pos_); }

 private:
  const std::string& s_;
  std::size_t pos_ = 0;

  void skipWs() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool lit(const char* word) {
    const std::size_t n = std::char_traits<char>::length(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }
  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned& v) {
    if (pos_ + 4 > s_.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {
            pos_ += 2;
            unsigned lo = 0;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: out += e; break;  // \" \\ \/ and anything unknown
      }
    }
    return false;
  }
  bool value(JValue& v, int depth) {
    if (depth > 32 || pos_ >= s_.size()) return false;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      v.kind = JValue::Object;
      skipWs();
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      for (;;) {
        skipWs();
        std::string key;
        if (!string(key)) return false;
        skipWs();
        if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
        skipWs();
        JValue child;
        if (!value(child, depth + 1)) return false;
        v.members.emplace_back(std::move(key), std::move(child));
        skipWs();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == '}') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      v.kind = JValue::Array;
      skipWs();
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      for (;;) {
        skipWs();
        JValue child;
        if (!value(child, depth + 1)) return false;
        v.items.push_back(std::move(child));
        skipWs();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') { ++pos_; continue; }
        if (s_[pos_] == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '"') {
      v.kind = JValue::String;
      return string(v.str);
    }
    if (lit("true")) { v.kind = JValue::Bool; v.boolean = true; return true; }
    if (lit("false")) { v.kind = JValue::Bool; return true; }
    if (lit("null")) { v.kind = JValue::Null; return true; }
    char* end = nullptr;
    const double d = std::strtod(s_.c_str() + pos_, &end);
    if (end == s_.c_str() + pos_) return false;
    pos_ = std::size_t(end - s_.c_str());
    v.kind = JValue::Number;
    v.number = d;
    return true;
  }
};

bool asNumber(const JValue& v, float& out) {
  if (v.kind == JValue::Number) {
    out = float(v.number);
    return std::isfinite(out);
  }
  if (v.kind == JValue::String) {
    char* end = nullptr;
    const float f = std::strtof(v.str.c_str(), &end);
    if (end != v.str.c_str() && std::isfinite(f)) {
      out = f;
      return true;
    }
  }
  return false;
}

void readNum(const JValue& obj, const char* key, float& dst) {
  if (const JValue* v = obj.get(key)) asNumber(*v, dst);
}

void readCurveJson(const JValue& obj, const char* key, std::vector<CurvePoint>& dst) {
  const JValue* v = obj.get(key);
  if (!v || v->kind != JValue::Array) return;
  std::vector<CurvePoint> pts;
  for (const JValue& pt : v->items) {
    float x = 0, y = 0;
    if (pt.kind == JValue::Array && pt.items.size() >= 2 && asNumber(pt.items[0], x) && asNumber(pt.items[1], y)) {
      pts.emplace_back(x, y);
    } else if (pt.kind == JValue::Object && pt.get("x") && pt.get("y") && asNumber(*pt.get("x"), x) &&
               asNumber(*pt.get("y"), y)) {
      pts.emplace_back(x, y);
    }
  }
  dst = std::move(pts);
}

void readGrade(const JValue& obj, const char* key, ToneZoneGrade& g) {
  const JValue* v = obj.get(key);
  if (!v || v->kind != JValue::Object) return;
  readNum(*v, "hue", g.hue);
  readNum(*v, "saturation", g.saturation);
}

}  // namespace

Result<EditParams> fromJson(const std::string& json) {
  JValue root;
  JParser parser(json);
  if (!parser.parse(root)) return fail(parser.error());
  if (root.kind != JValue::Object) return fail("EditParams JSON must be an object");
  if (const JValue* inner = root.get("params"); inner && inner->kind == JValue::Object) {
    JValue unwrapped = *inner;
    root = std::move(unwrapped);
  }

  EditParams p;
  // ---- legacy keys first (flat contract keys below win) ----
  readNum(root, "temperature", p.temperature);
  readNum(root, "gradeBalance", p.gradeBalance);
  if (const JValue* ev = root.get("exposureEv")) asNumber(*ev, p.exposure);
  if (const JValue* para = root.get("parametric"); para && para->kind == JValue::Object) {
    readNum(*para, "shadows", p.parametricShadows);
    readNum(*para, "darks", p.parametricDarks);
    readNum(*para, "lights", p.parametricLights);
    readNum(*para, "highlights", p.parametricHighlights);
    if (const JValue* sp = para->get("splits"); sp && sp->kind == JValue::Array && sp->items.size() >= 3) {
      asNumber(sp->items[0], p.parametricShadowSplit);
      asNumber(sp->items[1], p.parametricMidtoneSplit);
      asNumber(sp->items[2], p.parametricHighlightSplit);
    }
  }
  if (const JValue* hsl = root.get("hsl"); hsl && hsl->kind == JValue::Object) {
    for (int i = 0; i < kHslBandCount; ++i) {
      const JValue* band = hsl->get(hslBandName(i));
      if (band && band->kind == JValue::Array && band->items.size() >= 3) {
        asNumber(band->items[0], p.hsl[i].hue);
        asNumber(band->items[1], p.hsl[i].saturation);
        asNumber(band->items[2], p.hsl[i].luminance);
      }
    }
  }
  readGrade(root, "gradeShadows", p.gradeShadows);
  readGrade(root, "gradeMidtones", p.gradeMidtones);
  readGrade(root, "gradeHighlights", p.gradeHighlights);

  // ---- the contract's flat numeric keys ----
  for (const ParamField& f : paramFields()) {
    const JValue* v = root.get(f.key.c_str());
    float x = 0.f;
    if (v && asNumber(*v, x)) f.ref(p) = x / f.scale;
  }
  if (const JValue* v = root.get("crop"); v && v->kind == JValue::String) p.crop = v->str;
  if (const JValue* v = root.get("rot")) {
    float x = 0.f;
    if (asNumber(*v, x)) p.rotate = int(std::lround(x));
  }
  if (const JValue* v = root.get("flip")) {
    if (v->kind == JValue::Bool) p.flip = v->boolean;
    else if (v->kind == JValue::Number) p.flip = v->number != 0;
  }
  if (const JValue* v = root.get("look"); v && v->kind == JValue::String) p.look = v->str;
  if (const JValue* pal = root.get("palette"); pal && pal->kind == JValue::Object) {
    float x = p.paletteX, y = p.paletteY;
    const JValue* jx = pal->get("x");
    const JValue* jy = pal->get("y");
    if (jx && jy && asNumber(*jx, x) && asNumber(*jy, y)) {
      p.hasPalette = true;
      p.paletteX = x;
      p.paletteY = y;
    }
  }
  if (const JValue* sp = root.get("cv_splits"); sp && sp->kind == JValue::Array && sp->items.size() >= 3) {
    asNumber(sp->items[0], p.parametricShadowSplit);
    asNumber(sp->items[1], p.parametricMidtoneSplit);
    asNumber(sp->items[2], p.parametricHighlightSplit);
  }
  if (const JValue* tt = root.get("toneTarget"); tt && tt->kind == JValue::String) p.toneTarget = tt->str;
  if (const JValue* lut = root.get("lut"); lut && lut->kind == JValue::String) p.lutPath = lut->str;
  readCurveJson(root, "toneCurve", p.toneCurve);
  readCurveJson(root, "toneCurveRed", p.toneCurveRed);
  readCurveJson(root, "toneCurveGreen", p.toneCurveGreen);
  readCurveJson(root, "toneCurveBlue", p.toneCurveBlue);
  p.clamp();
  return p;
}

}  // namespace dapple
