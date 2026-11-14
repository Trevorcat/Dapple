// EditParams JSON reader: the inverse of toJson(). A small hand-written,
// tolerant parser, enough for our schema (no external dependency).

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "dapple/edit_params.h"
#include "edit_params_fields.h"
#include "json_value.h"
#include "local_eval.h"
#include "warp_field.h"

namespace dapple {

namespace {
using jsonlite::asNumber;
using jsonlite::JParser;
using jsonlite::JValue;

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
  if (const JValue* v = root.get("film"); v && v->kind == JValue::String) p.filmStock = v->str;
  if (const JValue* v = root.get("film_paper"); v && v->kind == JValue::String) p.filmPaper = v->str;
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
  if (const JValue* lv = root.get("local")) localFromValue(*lv, p.local);
  if (const JValue* rv = root.get("reshape")) reshapeFromValue(*rv, p.reshape);
  p.clamp();
  return p;
}

}  // namespace dapple
