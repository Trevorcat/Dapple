#include "engine/graph/params.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "dapple/film.h"
#include "dapple/look.h"
#include "edit_params_fields.h"

namespace dapple::engine {

std::string Hash128::hex() const {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
  return buf;
}

void ParamInputs::derive() {
  filmActive = filmOnly = false;
  if (ctx.filmLib && !edit.filmStock.empty()) {
    const film::FilmStock* s = ctx.filmLib->find(edit.filmStock);
    filmActive = s && !s->isPrintMedium();
    filmOnly = filmActive && edit.filmStrength / 100.f >= 1.f;
  }
  quality = frame.exact ? 2 : (frame.interactive ? 1 : 2);
  keepGrain = frame.interactive && (frame.activeParam == "grain" || frame.activeParam == "film_grain");
}

namespace {

void hashCurve(const std::vector<CurvePoint>& c, Hasher& h) {
  h.u64(c.size());
  for (const auto& p : c) {
    h.f32(p.first);
    h.f32(p.second);
  }
}

// picks an option different from the current value; `variant` selects which one
bool perturbString(std::string& s, int variant, std::initializer_list<const char*> options) {
  std::vector<std::string> other;
  for (const char* o : options)
    if (s != o) other.push_back(o);
  if (other.empty()) return false;
  s = other[std::size_t(variant) % other.size()];
  return true;
}

bool perturbCurve(std::vector<CurvePoint>& c, int variant) {
  if (c.empty())
    c = variant ? std::vector<CurvePoint>{{0, 0}, {64, 80}, {255, 255}} : std::vector<CurvePoint>{{0, 10}, {128, 140}, {255, 255}};
  else
    c.clear();
  return true;
}

}  // namespace

const ParamRegistry& ParamRegistry::instance() {
  static const ParamRegistry r;
  return r;
}

ParamRegistry::ParamRegistry() {
  // ---- sliders: one key per paramFields() entry --------------------------------------------------------------
  for (const ParamField& f : paramFields()) {
    KeyInfo k;
    k.name = f.key;
    k.kind = KeyInfo::Slider;
    k.field = &f;
    keys_.push_back(std::move(k));
  }
  auto add = [&](const char* name, KeyInfo::Kind kind, void (*hash)(const ParamInputs&, Hasher&),
                 bool (*perturb)(ParamInputs&, int)) {
    KeyInfo k;
    k.name = name;
    k.kind = kind;
    k.hash = hash;
    k.perturb = perturb;
    keys_.push_back(std::move(k));
  };
  const auto G = KeyInfo::Group;
  const auto C = KeyInfo::Context;
  const auto D = KeyInfo::Derived;
  // ---- non-scalar EditParams fields ----------------------------------------------------------------------------
  add("curve_rgb", G, [](const ParamInputs& in, Hasher& h) { hashCurve(in.edit.toneCurve, h); },
      [](ParamInputs& in, int v) { return perturbCurve(in.edit.toneCurve, v); });
  add("curve_r", G, [](const ParamInputs& in, Hasher& h) { hashCurve(in.edit.toneCurveRed, h); },
      [](ParamInputs& in, int v) { return perturbCurve(in.edit.toneCurveRed, v); });
  add("curve_g", G, [](const ParamInputs& in, Hasher& h) { hashCurve(in.edit.toneCurveGreen, h); },
      [](ParamInputs& in, int v) { return perturbCurve(in.edit.toneCurveGreen, v); });
  add("curve_b", G, [](const ParamInputs& in, Hasher& h) { hashCurve(in.edit.toneCurveBlue, h); },
      [](ParamInputs& in, int v) { return perturbCurve(in.edit.toneCurveBlue, v); });
  add("cv_splits", G,
      [](const ParamInputs& in, Hasher& h) {
        h.f32(in.edit.parametricShadowSplit);
        h.f32(in.edit.parametricMidtoneSplit);
        h.f32(in.edit.parametricHighlightSplit);
      },
      [](ParamInputs& in, int v) {
        in.edit.parametricShadowSplit = v ? 18.f : 33.f;
        in.edit.parametricMidtoneSplit = v ? 44.f : 57.f;
        in.edit.parametricHighlightSplit = v ? 81.f : 69.f;
        return true;
      });
  add("toneTarget", G, [](const ParamInputs& in, Hasher& h) { h.str(in.edit.toneTarget); },
      [](ParamInputs& in, int v) { return perturbString(in.edit.toneTarget, v, {"", "low-long", "high-short", "mid-mid"}); });
  add("look", G, [](const ParamInputs& in, Hasher& h) { h.str(in.edit.look); },
      [](ParamInputs& in, int v) { return perturbString(in.edit.look, v, {"std", "warm", "cool", "bw"}); });
  add("palette", G,
      [](const ParamInputs& in, Hasher& h) {
        h.boolean(in.edit.hasPalette);
        if (in.edit.hasPalette) {  // the pad point only matters when it is used
          h.f32(in.edit.paletteX);
          h.f32(in.edit.paletteY);
        }
      },
      [](ParamInputs& in, int v) {
        in.edit.hasPalette = true;
        in.edit.paletteX = v ? 0.2f : 0.7f;
        in.edit.paletteY = v ? 0.8f : 0.3f;
        return true;
      });
  add("crop", G, [](const ParamInputs& in, Hasher& h) { h.str(in.edit.crop); },
      [](ParamInputs& in, int v) { return perturbString(in.edit.crop, v, {"orig", "1:1", "4:5", "16:9"}); });
  add("rotate", G, [](const ParamInputs& in, Hasher& h) { h.i32(in.edit.rotate); },
      [](ParamInputs& in, int v) {
        in.edit.rotate = in.edit.rotate == 90 ? 270 : (v ? 180 : 90);
        return true;
      });
  add("flip", G, [](const ParamInputs& in, Hasher& h) { h.boolean(in.edit.flip); },
      [](ParamInputs& in, int) {
        in.edit.flip = !in.edit.flip;
        return true;
      });
  // film and the modern rendering family share the selector (EditParams::filmStock); each key sees only its own ids
  add("filmStock", G, [](const ParamInputs& in, Hasher& h) { h.str(isModernId(in.edit.filmStock) ? std::string() : in.edit.filmStock); },
      [](ParamInputs& in, int v) {
        if (!in.ctx.filmLib) return false;  // needs a real stock id
        return perturbString(in.edit.filmStock, v, {"", "kodak_portra_400", "fujifilm_pro_400h"});
      });
  add("modern", G,
      [](const ParamInputs& in, Hasher& h) {
        const ModernKind k = modernKind(in.edit.filmStock);
        h.i32(int(k));
        if (k != ModernKind::None) h.f32(in.edit.filmStrength);  // the strength of the base only matters when it is a modern rendering
      },
      [](ParamInputs& in, int v) { return perturbString(in.edit.filmStock, v, {"", "modern_clean", "modern_vivid"}); });
  add("filmPaper", G, [](const ParamInputs& in, Hasher& h) { h.str(in.edit.filmPaper); },
      [](ParamInputs& in, int v) { return perturbString(in.edit.filmPaper, v, {"", "none", "kodak_endura_premier"}); });
  add("lutPath", G, [](const ParamInputs& in, Hasher& h) { h.str(in.edit.lutPath); },
      [](ParamInputs& in, int v) { return perturbString(in.edit.lutPath, v, {"", "a.cube", "b.cube"}); });

  // ---- context ---------------------------------------------------------------------------------------------------
  add("ctx.source", C, [](const ParamInputs& in, Hasher& h) { h.u64(in.ctx.sourceId); h.boolean(in.ctx.sceneReferred); },
      [](ParamInputs& in, int) {
        in.ctx.sceneReferred = !in.ctx.sceneReferred;
        return true;
      });
  add("ctx.proxy", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.ctx.hasProxy); }, nullptr);
  add("ctx.camera", C, [](const ParamInputs& in, Hasher& h) { h.str(in.ctx.cameraId); }, nullptr);  // needs a real profile
  add("ctx.lutIdentity", C, [](const ParamInputs& in, Hasher& h) { h.u64(in.ctx.lutIdentity); }, nullptr);  // needs a real LUT
  add("ctx.filmLib", C, [](const ParamInputs& in, Hasher& h) { h.u64(std::uintptr_t(in.ctx.filmLib)); }, nullptr);
  add("ctx.toneReach", C, [](const ParamInputs& in, Hasher& h) { h.f32(in.ctx.toneReach); },
      [](ParamInputs& in, int v) {
        in.ctx.toneReach = v ? 1.6f : 1.3f;
        return true;
      });
  add("ctx.srcSize", C, [](const ParamInputs& in, Hasher& h) { h.i32(in.ctx.srcW); h.i32(in.ctx.srcH); }, nullptr);
  add("ctx.region", C,
      [](const ParamInputs& in, Hasher& h) {
        for (float v : in.frame.region) h.f32(v);
        h.i32(in.frame.regionLongEdge);
      },
      [](ParamInputs& in, int v) {
        in.frame.region[0] = 0.1f;
        in.frame.region[1] = 0.2f;
        in.frame.region[2] = v ? 0.6f : 0.7f;
        in.frame.region[3] = 0.8f;
        in.frame.regionLongEdge = v ? 640 : 800;
        return true;
      });
  add("ctx.outLongEdge", C, [](const ParamInputs& in, Hasher& h) { h.i32(in.frame.longEdge); },
      [](ParamInputs& in, int v) {
        in.frame.longEdge = v ? 640 : 1080;
        return true;
      });
  add("ctx.quality", D, [](const ParamInputs& in, Hasher& h) { h.i32(in.quality); },
      [](ParamInputs& in, int) {
        in.frame.interactive = !in.frame.interactive;
        return true;
      });
  add("ctx.noise", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.frame.noise); },
      [](ParamInputs& in, int) {
        in.frame.noise = !in.frame.noise;
        return true;
      });
  add("ctx.bakeSize", C, [](const ParamInputs& in, Hasher& h) { h.i32(in.frame.bakeSize); },
      [](ParamInputs& in, int v) {
        in.frame.bakeSize = v ? 41 : 25;
        return true;
      });
  add("ctx.filmActive", D, [](const ParamInputs& in, Hasher& h) { h.boolean(in.filmActive); }, nullptr);
  add("ctx.filmOnly", D, [](const ParamInputs& in, Hasher& h) { h.boolean(in.filmOnly); }, nullptr);
  add("ctx.wantAnalysis", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.frame.wantAnalysis); },
      [](ParamInputs& in, int) {
        in.frame.wantAnalysis = !in.frame.wantAnalysis;
        return true;
      });
  add("ctx.fastAnalysis", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.frame.fastAnalysis); },
      [](ParamInputs& in, int) {
        in.frame.fastAnalysis = !in.frame.fastAnalysis;
        return true;
      });
  add("ctx.exact", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.frame.exact); },
      [](ParamInputs& in, int) {
        in.frame.exact = !in.frame.exact;
        return true;
      });
  add("ctx.wantOverflow", C, [](const ParamInputs& in, Hasher& h) { h.boolean(in.frame.wantOverflow); },
      [](ParamInputs& in, int) {
        in.frame.wantOverflow = !in.frame.wantOverflow;
        return true;
      });
  add("ctx.frameIndex", C, [](const ParamInputs& in, Hasher& h) { h.u64(in.ctx.frameIndex); },
      [](ParamInputs& in, int v) {
        in.ctx.frameIndex = v ? 7u : 3u;
        return true;
      });
  add("ctx.fullLongEdge", C, [](const ParamInputs& in, Hasher& h) { h.i32(in.ctx.fullLongEdge); },
      [](ParamInputs& in, int v) {
        in.ctx.fullLongEdge = v ? 6000 : 4000;
        return true;
      });
  add("ctx.outSpace", C, [](const ParamInputs& in, Hasher& h) { h.i32(in.edit.outputSpace); },
      [](ParamInputs& in, int v) {
        in.edit.outputSpace = v ? 1 : 0;
        return true;
      });
  add("ctx.filmKeyEv", C, [](const ParamInputs& in, Hasher& h) { h.f32(in.edit.filmKeyEv); },
      [](ParamInputs& in, int v) {
        in.edit.filmKeyEv = v ? 0.75f : -0.5f;
        return true;
      });
  add("ctx.filmPrintEv", C, [](const ParamInputs& in, Hasher& h) { h.f32(in.edit.filmPrintEv); },
      [](ParamInputs& in, int v) {
        in.edit.filmPrintEv = v ? 0.5f : -0.5f;
        return true;
      });
  add("ctx.filmScene", C, [](const ParamInputs& in, Hasher& h) { h.f32(in.edit.filmSceneX), h.f32(in.edit.filmSceneY); },
      [](ParamInputs& in, int v) {
        in.edit.filmSceneX = v ? 0.33f : 0.40f;
        in.edit.filmSceneY = v ? 0.35f : 0.39f;
        return true;
      });
  add("ctx.keepGrain", D, [](const ParamInputs& in, Hasher& h) { h.boolean(in.keepGrain); }, nullptr);
  add("ctx.activeParam", C, [](const ParamInputs& in, Hasher& h) { h.str(in.frame.activeParam); },
      [](ParamInputs& in, int v) { return perturbString(in.frame.activeParam, v, {"", "grain", "exposure"}); });
  if (keys_.size() > kMaxKeys) throw std::logic_error("too many ParamKeys");
}

ParamKey ParamRegistry::find(std::string_view name) const {
  for (std::size_t i = 0; i < keys_.size(); ++i)
    if (keys_[i].name == name) return ParamKey(i);
  return kNoKey;
}

ParamKey ParamRegistry::mustFind(std::string_view name) const {
  const ParamKey k = find(name);
  if (k == kNoKey) throw std::logic_error("unknown ParamKey: " + std::string(name));
  return k;
}

ParamMask ParamRegistry::mask(std::initializer_list<std::string_view> names) const {
  ParamMask m;
  for (auto n : names) m.add(mustFind(n));
  return m;
}

Hash128 ParamRegistry::hashKey(ParamKey k, const ParamInputs& in) const {
  const KeyInfo& ki = keys_[k];
  Hasher h;
  h.u64(k);
  if (ki.kind == KeyInfo::Slider) h.f32(ki.field->ref(in.edit));
  else ki.hash(in, h);
  return h.finish();
}

bool ParamRegistry::perturb(ParamKey k, ParamInputs& in, int variant) const {
  const KeyInfo& ki = keys_[k];
  if (ki.kind == KeyInfo::Slider) {
    const ParamField& f = *ki.field;
    float& v = f.ref(in.edit);
    if (!(f.hi > f.lo)) return false;
    const float t = (v - f.lo) / (f.hi - f.lo);
    const float step = variant ? 0.21f : 0.37f;
    v = t < 0.5f ? v + step * (f.hi - v) : v - step * (v - f.lo);
    return true;
  }
  return ki.perturb ? ki.perturb(in, variant) : false;
}

ParamSet ParamSet::make(ParamInputs in) {
  in.edit.clamp();
  in.derive();
  ParamSet s;
  s.in_ = std::move(in);
  const ParamRegistry& r = ParamRegistry::instance();
  s.h_.resize(r.size());
  for (std::size_t k = 0; k < r.size(); ++k) s.h_[k] = r.hashKey(ParamKey(k), s.in_);
  return s;
}

Hash128 ParamSet::hash(const ParamMask& m) const {
  Hasher h;
  for (std::size_t k = 0; k < h_.size(); ++k)
    if (m.bits.test(k)) h.hash(h_[k]);
  return h.finish();
}

}  // namespace dapple::engine
