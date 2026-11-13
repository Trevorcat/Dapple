#include "dapple/edit_params.h"

#include <cmath>
#include <cstdio>
#include <sstream>

#include "dapple/look.h"
#include "edit_params_fields.h"

namespace dapple {

namespace {

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return buf;
}

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

void writeCurve(std::ostringstream& o, const char* key, const std::vector<CurvePoint>& c) {
  o << ",\n  \"" << key << "\": [";
  for (std::size_t i = 0; i < c.size(); ++i) o << (i ? ", " : "") << "[" << num(c[i].first) << ", " << num(c[i].second) << "]";
  o << "]";
}

bool validCrop(const std::string& c) {
  return c == "orig" || c == "1:1" || c == "4:5" || c == "3:4" || c == "16:9" || c == "9:16";
}

}  // namespace

const std::vector<ParamField>& paramFields() {
  static const std::vector<ParamField> table = [] {
    std::vector<ParamField> t;
    static const EditParams d;  // defaults, and the base for offsets
    auto add = [&](std::string key, const float& member, float lo, float hi, const char* group, float scale = 1.f,
                   bool additive = true) {
      t.push_back({std::move(key),
                   std::size_t(reinterpret_cast<const char*>(&member) - reinterpret_cast<const char*>(&d)), member, lo,
                   hi, scale, group, additive});
    };
    add("exposure", d.exposure, -5.f, 5.f, "light", 20.f);
    add("contrast", d.contrast, -100.f, 100.f, "light");
    add("hdr", d.hdr, 0.f, 100.f, "light");
    add("highlights", d.highlights, -100.f, 100.f, "light");
    add("shadows", d.shadows, -100.f, 100.f, "light");
    add("whites", d.whites, -100.f, 100.f, "light");
    add("blacks", d.blacks, -100.f, 100.f, "light");
    add("temp", d.temperature, -100.f, 100.f, "color");
    add("tint", d.tint, -100.f, 100.f, "color");
    add("vibrance", d.vibrance, -100.f, 100.f, "color");
    add("saturation", d.saturation, -100.f, 100.f, "color");
    for (int i = 0; i < kHslBandCount; ++i) {
      const std::string b = std::string("hsl_") + hslBandName(i) + "_";
      add(b + "h", d.hsl[i].hue, -100.f, 100.f, "color");
      add(b + "s", d.hsl[i].saturation, -100.f, 100.f, "color");
      add(b + "l", d.hsl[i].luminance, -100.f, 100.f, "color");
    }
    add("cv_rgb_H", d.parametricHighlights, -100.f, 100.f, "curves");
    add("cv_rgb_L", d.parametricLights, -100.f, 100.f, "curves");
    add("cv_rgb_D", d.parametricDarks, -100.f, 100.f, "curves");
    add("cv_rgb_S", d.parametricShadows, -100.f, 100.f, "curves");
    static const char* kCh[3] = {"r", "g", "b"};
    // channelParametric[ch] = {shadows, darks, lights, highlights}
    static const char* kZone[4] = {"S", "D", "L", "H"};
    for (int c = 0; c < 3; ++c)
      for (int z = 3; z >= 0; --z)
        add(std::string("cv_") + kCh[c] + "_" + kZone[z], d.channelParametric[c][z], -100.f, 100.f, "curves");
    add("clarity", d.clarity, -100.f, 100.f, "effects");
    add("texture", d.texture, -100.f, 100.f, "effects");
    add("dehaze", d.dehaze, -100.f, 100.f, "effects");
    add("vignette", d.vignette, -100.f, 100.f, "effects");
    add("grain", d.grain, 0.f, 100.f, "effects");
    add("sharpen", d.sharpen, 0.f, 150.f, "detail");
    add("sharpRadius", d.sharpRadius, 5.f, 30.f, "detail", 1.f, false);
    add("nrLuma", d.nrLuma, 0.f, 100.f, "detail");
    add("nrColor", d.nrColor, 0.f, 100.f, "detail");
    auto grade = [&](const char* zone, const ToneZoneGrade& g) {
      const std::string b = std::string("gr_") + zone + "_";
      add(b + "h", g.hue, 0.f, 360.f, "grading", 1.f, false);
      add(b + "s", g.saturation, 0.f, 100.f, "grading");
      add(b + "l", g.luminance, -100.f, 100.f, "grading");
    };
    grade("shadows", d.gradeShadows);
    grade("mid", d.gradeMidtones);
    grade("highlights", d.gradeHighlights);
    grade("global", d.gradeGlobal);
    add("gr_blend", d.gradeBlend, 0.f, 100.f, "grading", 1.f, false);
    add("gr_balance", d.gradeBalance, -100.f, 100.f, "grading");
    add("straighten", d.straighten, -45.f, 45.f, "crop");
    add("lookStrength", d.lookStrength, 0.f, 100.f, "look", 1.f, false);
    add("toneStrength", d.toneStrength, 0.f, 100.f, "tone");
    add("lutAmount", d.lutAmount, 0.f, 100.f, "lut", 1.f, false);
    add("film_strength", d.filmStrength, 0.f, 100.f, "film", 1.f, false);
    add("film_exp", d.filmExposure, -3.f, 3.f, "film", 20.f);
    add("film_warmth", d.filmWarmth, -100.f, 100.f, "film");
    add("film_tint", d.filmTint, -100.f, 100.f, "film");
    add("film_grain", d.filmGrain, 0.f, 200.f, "film", 1.f, false);
    add("film_halation", d.filmHalation, 0.f, 200.f, "film", 1.f, false);
    add("film_shoot", d.filmShoot, -1.f, 1.f, "film", 1.f);
    add("film_print", d.filmPrint, -1.f, 1.f, "film", 1.f);
    add("film_black", d.filmBlack, -1.f, 1.f, "film", 1.f);
    add("film_latitude", d.filmLatitude, -1.f, 1.f, "film", 1.f);
    add("film_wb", d.filmWb, 0.f, 1.f, "film", 1.f);
    return t;
  }();
  return table;
}

void EditParams::clamp() {
  for (const ParamField& f : paramFields()) {
    float& v = f.ref(*this);
    if (!std::isfinite(v)) v = f.def;
    v = std::fmin(std::fmax(v, f.lo), f.hi);
  }
  auto fix = [](float& v, float lo, float hi, float def) {
    if (!std::isfinite(v)) v = def;
    v = std::fmin(std::fmax(v, lo), hi);
  };
  fix(parametricShadowSplit, 0.f, 100.f, 25.f);
  fix(parametricMidtoneSplit, 0.f, 100.f, 50.f);
  fix(parametricHighlightSplit, 0.f, 100.f, 75.f);
  fix(paletteX, 0.04f, 0.96f, 0.5f);
  fix(paletteY, 0.04f, 0.96f, 0.5f);
  if (!validCrop(crop)) crop = "orig";
  if (lookIndex(look) < 0) look = "std";
  if (filmStock.size() > 64 || filmStock == "none") filmStock.clear();
  if (filmPaper.size() > 64) filmPaper.clear();
  rotate = ((rotate % 360) + 360) % 360;
  rotate = (rotate + 45) / 90 * 90 % 360;
  clampLocal(local);
}

EditParams& EditParams::operator+=(const EditParams& d) {
  static const EditParams def;
  for (const ParamField& f : paramFields()) {
    float& v = f.ref(*this);
    const float dv = f.ref(d);
    if (f.additive) {
      v += dv - f.def;
    } else if (dv != f.def) {
      v = dv;
    }
  }
  if (!d.toneTarget.empty()) toneTarget = d.toneTarget;
  if (!d.toneCurve.empty()) toneCurve = d.toneCurve;
  if (!d.toneCurveRed.empty()) toneCurveRed = d.toneCurveRed;
  if (!d.toneCurveGreen.empty()) toneCurveGreen = d.toneCurveGreen;
  if (!d.toneCurveBlue.empty()) toneCurveBlue = d.toneCurveBlue;
  if (!d.lutPath.empty()) {
    lutPath = d.lutPath;
    lutAmount = d.lutAmount;
  }
  if (d.crop != def.crop) crop = d.crop;
  rotate = (rotate + d.rotate) % 360;
  flip = flip != d.flip;
  if (d.look != def.look) look = d.look;
  if (!d.filmStock.empty()) filmStock = d.filmStock;
  if (!d.filmPaper.empty()) filmPaper = d.filmPaper;
  if (d.hasPalette) {
    hasPalette = true;
    paletteX = d.paletteX;
    paletteY = d.paletteY;
  }
  if (!d.local.empty()) {  // both sets apply: the delta's adjustments follow this edit's own
    local.insert(local.end(), d.local.begin(), d.local.end());
    clampLocal(local);
  }
  return *this;
}

const char* hslBandName(int band) {
  static const char* kNames[] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
  return band >= 0 && band < kHslBandCount ? kNames[band] : "?";
}

namespace {

std::string writeJson(const EditParams& p, bool diffOnly) {
  static const EditParams def;
  std::ostringstream o;
  o << "{";
  bool first = true;
  auto sep = [&] {
    o << (first ? "\n  " : ",\n  ");
    first = false;
  };
  for (const ParamField& f : paramFields()) {
    const float v = f.ref(p);
    if (diffOnly && v == f.def) continue;
    sep();
    o << "\"" << f.key << "\": " << num(double(v) * double(f.scale));
  }
  if (!diffOnly || p.crop != def.crop) { sep(); o << "\"crop\": \"" << escape(p.crop) << "\""; }
  if (!diffOnly || p.rotate != def.rotate) { sep(); o << "\"rot\": " << p.rotate; }
  if (!diffOnly || p.flip != def.flip) { sep(); o << "\"flip\": " << (p.flip ? "true" : "false"); }
  if (!diffOnly || p.look != def.look) { sep(); o << "\"look\": \"" << escape(p.look) << "\""; }
  if (!diffOnly || !p.filmStock.empty()) { sep(); o << "\"film\": \"" << escape(p.filmStock) << "\""; }
  if (!diffOnly || !p.filmPaper.empty()) { sep(); o << "\"film_paper\": \"" << escape(p.filmPaper) << "\""; }
  if (p.hasPalette) { sep(); o << "\"palette\": {\"x\": " << num(p.paletteX) << ", \"y\": " << num(p.paletteY) << "}"; }
  if (!diffOnly || !p.toneTarget.empty()) { sep(); o << "\"toneTarget\": \"" << escape(p.toneTarget) << "\""; }
  if (!diffOnly || !p.lutPath.empty()) { sep(); o << "\"lut\": \"" << escape(p.lutPath) << "\""; }
  if (!diffOnly || p.parametricShadowSplit != 25.f || p.parametricMidtoneSplit != 50.f ||
      p.parametricHighlightSplit != 75.f) {
    sep();
    o << "\"cv_splits\": [" << num(p.parametricShadowSplit) << ", " << num(p.parametricMidtoneSplit) << ", "
      << num(p.parametricHighlightSplit) << "]";
  }
  std::ostringstream curves;
  if (!diffOnly || !p.toneCurve.empty()) writeCurve(curves, "toneCurve", p.toneCurve);
  if (!diffOnly || !p.toneCurveRed.empty()) writeCurve(curves, "toneCurveRed", p.toneCurveRed);
  if (!diffOnly || !p.toneCurveGreen.empty()) writeCurve(curves, "toneCurveGreen", p.toneCurveGreen);
  if (!diffOnly || !p.toneCurveBlue.empty()) writeCurve(curves, "toneCurveBlue", p.toneCurveBlue);
  std::string cs = curves.str();
  if (first && !cs.empty()) cs.erase(0, 1);  // leading comma
  o << cs;
  if (!p.local.empty()) {
    o << ((first && cs.empty()) ? "\n  " : ",\n  ");
    o << "\"local\": " << localToJson(p.local);
  }
  o << "\n}\n";
  return o.str();
}

}  // namespace

std::string toJson(const EditParams& p) {
  EditParams c = p;
  c.clamp();
  return writeJson(c, false);
}

std::string toJsonDiff(const EditParams& p) {
  EditParams c = p;
  c.clamp();
  return writeJson(c, true);
}

std::string paramsSchemaJson() {
  std::ostringstream o;
  o << "{\"sliders\":[";
  bool first = true;
  for (const ParamField& f : paramFields()) {
    o << (first ? "" : ",") << "{\"key\":\"" << f.key << "\",\"min\":" << num(double(f.lo) * f.scale)
      << ",\"max\":" << num(double(f.hi) * f.scale) << ",\"default\":" << num(double(f.def) * f.scale)
      << ",\"group\":\"" << f.group << "\"}";
    first = false;
  }
  o << "],\"crop\":{\"values\":[\"orig\",\"1:1\",\"4:5\",\"3:4\",\"16:9\",\"9:16\"],\"default\":\"orig\"},"
       "\"rot\":{\"values\":[0,90,180,270],\"default\":0},\"flip\":{\"default\":false},"
       "\"palette\":{\"min\":0.04,\"max\":0.96},\"look\":{\"default\":\"std\"},"
       "\"film\":{\"default\":\"\"},\"film_paper\":{\"default\":\"\"}}";
  return o.str();
}

}  // namespace dapple
