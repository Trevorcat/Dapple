// FilmParams <-> JSON (reference schema key names), and the no-spatial switch.
#include <cmath>
#include <cstdio>
#include <sstream>

#include "dapple/film.h"
#include "film_json.h"

namespace dapple::film {

using detail::JValue;

namespace {

struct Ctx {
  std::string err;
  bool fail(const std::string& path, const char* what) {
    if (err.empty()) err = path + ": " + what;
    return false;
  }
};

bool getD(const JValue& v, double& out) {
  if (v.type == JValue::Num) { out = v.num; return true; }
  if (v.type == JValue::Bool) { out = v.boolean ? 1 : 0; return true; }
  return false;
}
bool getB(const JValue& v, bool& out) {
  if (v.type == JValue::Bool) { out = v.boolean; return true; }
  if (v.type == JValue::Num) { out = v.num != 0; return true; }
  return false;
}
bool getS(const JValue& v, std::string& out) {
  if (v.type == JValue::Str) { out = v.str; return true; }
  return false;
}
template <std::size_t N>
bool getArr(const JValue& v, std::array<double, N>& out) {
  if (v.type == JValue::Num) {  // scalar broadcast
    out.fill(v.num);
    return true;
  }
  if (v.type != JValue::Arr || v.arr.size() != N) return false;
  for (std::size_t i = 0; i < N; ++i)
    if (!getD(v.arr[i], out[i])) return false;
  return true;
}
bool getI(const JValue& v, int& out) {
  double d;
  if (!getD(v, d) || !(d > -2147483648.0 && d < 2147483648.0)) return false;
  out = int(d);
  return true;
}

#define KEY_D(name, field) if (k == name) { if (!getD(v, field)) return c.fail(path + k, "expected a number"); continue; }
#define KEY_B(name, field) if (k == name) { if (!getB(v, field)) return c.fail(path + k, "expected a boolean"); continue; }
#define KEY_S(name, field) if (k == name) { if (!getS(v, field)) return c.fail(path + k, "expected a string"); continue; }
#define KEY_I(name, field) if (k == name) { if (!getI(v, field)) return c.fail(path + k, "expected an integer"); continue; }
#define KEY_A(name, field) if (k == name) { if (!getArr(v, field)) return c.fail(path + k, "expected an array of the right length"); continue; }
// Options of the reference this engine does not implement: accepted only at their neutral value.
#define KEY_UNSUPPORTED(name, neutral) \
  if (k == name) { \
    double x_; \
    if (!getD(v, x_)) return c.fail(path + k, "expected a number"); \
    if (x_ != (neutral)) return c.fail(path + k, "not implemented (only the default " #neutral " is supported)"); \
    continue; \
  }
#define KEY_IGNORE(name) if (k == name) continue;
#define UNKNOWN() return c.fail(path + k, "unknown key");

bool diffusion(const JValue& o, DiffusionFilterParams& d, Ctx& c, const std::string& path) {
  if (o.type != JValue::Obj) return c.fail(path, "expected an object");
  for (const auto& [k, v] : o.obj) {
    KEY_B("active", d.active) KEY_S("filter_family", d.filterFamily) KEY_D("strength", d.strength)
    KEY_D("spatial_scale", d.spatialScale) KEY_D("halo_warmth", d.haloWarmth) KEY_D("core_intensity", d.coreIntensity)
    KEY_D("core_size", d.coreSize) KEY_D("halo_intensity", d.haloIntensity) KEY_D("halo_size", d.haloSize)
    KEY_D("bloom_intensity", d.bloomIntensity) KEY_D("bloom_size", d.bloomSize)
    UNKNOWN()
  }
  return true;
}

bool glare(const JValue& o, GlareParams& g, Ctx& c, const std::string& path) {
  if (o.type != JValue::Obj) return c.fail(path, "expected an object");
  for (const auto& [k, v] : o.obj) {
    KEY_B("active", g.active) KEY_D("percent", g.percent) KEY_D("roughness", g.roughness) KEY_D("blur", g.blur)
    UNKNOWN()
  }
  return true;
}

bool knee(const JValue& v, KneeParams& kp) {
  std::array<double, 3> a;
  if (!getArr(v, a)) return false;
  kp = {a[0], a[1], a[2]};
  return true;
}

bool gamutBlock(const JValue& o, bool& active, KneeParams& kp, std::array<double, 3>* light, bool inputSide, Ctx& c,
                const std::string& path) {
  if (o.type != JValue::Obj) return c.fail(path, "expected an object");
  for (const auto& [k, v] : o.obj) {
    if (k == "active") {
      if (!getB(v, active)) return c.fail(path + k, "expected a boolean");
      continue;
    }
    if (k == "algorithm") {
      std::string a;
      if (!getS(v, a)) return c.fail(path + k, "expected a string");
      if (inputSide) {
        if (a != "xy") return c.fail(path + k, "only the 'xy' input gamut compression is implemented");
      } else if (a == "off") {
        active = false;
      } else if (a == "cam16ucs") {
        active = true;
      } else {
        return c.fail(path + k, "only 'cam16ucs' and 'off' output gamut compression are implemented");
      }
      continue;
    }
    if (k == "knee") {
      if (!knee(v, kp)) return c.fail(path + k, "expected [threshold, limit, power]");
      if (!inputSide && (kp.threshold != 0.0 || kp.limit != 1.0 || kp.power != 6.0))
        return c.fail(path + k, "not implemented (the output gamut compression constants are fixed)");
      continue;
    }
    if (k == "lightness_compression" && light) {
      if (!getArr(v, *light)) return c.fail(path + k, "expected [threshold, limit, power]");
      if ((*light)[0] != 0.7 || (*light)[1] != 1.0 || (*light)[2] != 2.2)
        return c.fail(path + k, "not implemented (the CAM16 lightness compression constants are fixed)");
      continue;
    }
    UNKNOWN()
  }
  return true;
}

bool applyAll(const JValue& root, FilmParams& p, Ctx& c) {
  if (root.type != JValue::Obj) return c.fail("", "top level must be an object");
  for (const auto& [top, tv] : root.obj) {
    const std::string path = top + ".";
    if (top == "film") {
      if (!getS(tv, p.film)) return c.fail(top, "expected a string");
      continue;
    }
    if (top == "print") {
      if (!getS(tv, p.paper)) return c.fail(top, "expected a string");
      continue;
    }
    if (top == "taps") {
      for (const auto& [k, v] : tv.obj)
        if (v.type != JValue::Null) return c.fail(path + k, "taps are not supported");
      continue;
    }
    if (tv.type != JValue::Obj) return c.fail(top, "expected an object");
    if (top == "camera") {
      auto& m = p.camera;
      for (const auto& [k, v] : tv.obj) {
        KEY_D("exposure_compensation_ev", m.exposureCompensationEv) KEY_B("auto_exposure", m.autoExposure)
        KEY_S("auto_exposure_method", m.autoExposureMethod) KEY_D("lens_blur_um", m.lensBlurUm)
        KEY_D("film_format_mm", m.filmFormatMm) KEY_A("filter_uv", m.filterUv) KEY_A("filter_ir", m.filterIr)
        if (k == "diffusion_filter") {
          if (!diffusion(v, m.diffusionFilter, c, path + k + ".")) return false;
          continue;
        }
        UNKNOWN()
      }
    } else if (top == "film_render") {
      auto& m = p.filmRender;
      for (const auto& [k, v] : tv.obj) {
        if (k == "density_curve_gamma") {
          if (v.type == JValue::Arr) {
            if (!getArr(v, m.densityCurveGammaRgb)) return c.fail(path + k, "expected number or [r,g,b]");
            m.densityCurveGamma = 1.0;
          } else if (!getD(v, m.densityCurveGamma)) {
            return c.fail(path + k, "expected number or [r,g,b]");
          }
          continue;
        }
        if (k == "grain") {
          auto& g = m.grain;
          const std::string gp = path + k + ".";
          for (const auto& [kk, vv] : v.obj) {
            const std::string& k2 = kk;
            const std::string& path2 = gp;
            Ctx& c2 = c;
            (void)path2;
            if (k2 == "active") { if (!getB(vv, g.active)) return c2.fail(gp + k2, "expected a boolean"); continue; }
            if (k2 == "sublayers_active") { if (!getB(vv, g.sublayersActive)) return c2.fail(gp + k2, "expected a boolean"); continue; }
            if (k2 == "particle_area_um2") { if (!getD(vv, g.particleAreaUm2)) return c2.fail(gp + k2, "expected a number"); continue; }
            if (k2 == "particle_scale") { if (!getArr(vv, g.particleScale)) return c2.fail(gp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "particle_scale_layers") { if (!getArr(vv, g.particleScaleLayers)) return c2.fail(gp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "density_min") { if (!getArr(vv, g.densityMin)) return c2.fail(gp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "uniformity") { if (!getArr(vv, g.uniformity)) return c2.fail(gp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "blur") { if (!getD(vv, g.blur)) return c2.fail(gp + k2, "expected a number"); continue; }
            if (k2 == "blur_dye_clouds_um") { if (!getD(vv, g.blurDyeCloudsUm)) return c2.fail(gp + k2, "expected a number"); continue; }
            if (k2 == "micro_structure") { if (!getArr(vv, g.microStructure)) return c2.fail(gp + k2, "expected [a,b]"); continue; }
            if (k2 == "n_sub_layers") { if (!getI(vv, g.nSubLayers)) return c2.fail(gp + k2, "expected an integer"); continue; }
            return c2.fail(gp + k2, "unknown key");
          }
          continue;
        }
        if (k == "halation") {
          auto& h = m.halation;
          const std::string hp = path + k + ".";
          for (const auto& [k2, vv] : v.obj) {
            if (k2 == "active") { if (!getB(vv, h.active)) return c.fail(hp + k2, "expected a boolean"); continue; }
            if (k2 == "scatter_amount") { if (!getD(vv, h.scatterAmount)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "scatter_spatial_scale") { if (!getD(vv, h.scatterSpatialScale)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "halation_amount") { if (!getD(vv, h.halationAmount)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "halation_spatial_scale") { if (!getD(vv, h.halationSpatialScale)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "scatter_core_um") { if (!getArr(vv, h.scatterCoreUm)) return c.fail(hp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "scatter_tail_um") { if (!getArr(vv, h.scatterTailUm)) return c.fail(hp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "scatter_tail_weight") { if (!getArr(vv, h.scatterTailWeight)) return c.fail(hp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "boost_ev") { if (!getD(vv, h.boostEv)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "boost_range") { if (!getD(vv, h.boostRange)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "protect_ev") { if (!getD(vv, h.protectEv)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "halation_strength") { if (!getArr(vv, h.halationStrength)) return c.fail(hp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "halation_first_sigma_um") { if (!getArr(vv, h.halationFirstSigmaUm)) return c.fail(hp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "halation_n_bounces") { if (!getI(vv, h.halationNBounces)) return c.fail(hp + k2, "expected an integer"); continue; }
            if (k2 == "halation_bounce_decay") { if (!getD(vv, h.halationBounceDecay)) return c.fail(hp + k2, "expected a number"); continue; }
            if (k2 == "halation_renormalize") { if (!getB(vv, h.halationRenormalize)) return c.fail(hp + k2, "expected a boolean"); continue; }
            return c.fail(hp + k2, "unknown key");
          }
          continue;
        }
        if (k == "dir_couplers") {
          auto& d = m.dirCouplers;
          const std::string dp = path + k + ".";
          for (const auto& [k2, vv] : v.obj) {
            if (k2 == "active") { if (!getB(vv, d.active)) return c.fail(dp + k2, "expected a boolean"); continue; }
            if (k2 == "amount") { if (!getD(vv, d.amount)) return c.fail(dp + k2, "expected a number"); continue; }
            if (k2 == "inhibition_samelayer") { if (!getD(vv, d.inhibitionSamelayer)) return c.fail(dp + k2, "expected a number"); continue; }
            if (k2 == "inhibition_interlayer") { if (!getD(vv, d.inhibitionInterlayer)) return c.fail(dp + k2, "expected a number"); continue; }
            if (k2 == "gamma_samelayer_rgb") { if (!getArr(vv, d.gammaSamelayerRgb)) return c.fail(dp + k2, "expected [r,g,b]"); continue; }
            if (k2 == "gamma_interlayer_r_to_gb") { if (!getArr(vv, d.gammaInterlayerRToGb)) return c.fail(dp + k2, "expected [g,b]"); continue; }
            if (k2 == "gamma_interlayer_g_to_rb") { if (!getArr(vv, d.gammaInterlayerGToRb)) return c.fail(dp + k2, "expected [r,b]"); continue; }
            if (k2 == "gamma_interlayer_b_to_rg") { if (!getArr(vv, d.gammaInterlayerBToRg)) return c.fail(dp + k2, "expected [r,g]"); continue; }
            if (k2 == "diffusion_size_um") { if (!getD(vv, d.diffusionSizeUm)) return c.fail(dp + k2, "expected a number"); continue; }
            if (k2 == "diffusion_tail_um") { if (!getD(vv, d.diffusionTailUm)) return c.fail(dp + k2, "expected a number"); continue; }
            if (k2 == "diffusion_tail_weight") { if (!getD(vv, d.diffusionTailWeight)) return c.fail(dp + k2, "expected a number"); continue; }
            return c.fail(dp + k2, "unknown key");
          }
          continue;
        }
        if (k == "glare") {
          if (!glare(v, m.glare, c, path + k + ".")) return false;
          continue;
        }
        UNKNOWN()
      }
    } else if (top == "print_render") {
      for (const auto& [k, v] : tv.obj) {
        if (k == "glare") {
          if (!glare(v, p.printRender.glare, c, path + k + ".")) return false;
          continue;
        }
        if (k == "density_curves_morph") {
          auto& m = p.printRender.densityCurvesMorph;
          const std::string mp = path + k + ".";
          for (const auto& [k2, vv] : v.obj) {
            if (k2 == "active") { if (!getB(vv, m.active)) return c.fail(mp + k2, "expected a boolean"); continue; }
            if (k2 == "gamma_factor") { if (!getD(vv, m.gammaFactor)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "gamma_factor_fast") { if (!getD(vv, m.gammaFactorFast)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "gamma_factor_slow") { if (!getD(vv, m.gammaFactorSlow)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "gamma_factor_red") { if (!getD(vv, m.gammaFactorRed)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "gamma_factor_green") { if (!getD(vv, m.gammaFactorGreen)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "gamma_factor_blue") { if (!getD(vv, m.gammaFactorBlue)) return c.fail(mp + k2, "expected a number"); continue; }
            if (k2 == "developer_exhaustion") {
              double x;
              if (!getD(vv, x)) return c.fail(mp + k2, "expected a number");
              if (x != 0.0) return c.fail(mp + k2, "not implemented (only 0 is supported)");
              continue;
            }
            return c.fail(mp + k2, "unknown key");
          }
          continue;
        }
        UNKNOWN()
      }
    } else if (top == "enlarger") {
      auto& m = p.enlarger;
      for (const auto& [k, v] : tv.obj) {
        KEY_S("illuminant", m.illuminant) KEY_D("print_exposure", m.printExposure)
        KEY_B("print_exposure_compensation", m.printExposureCompensation)
        KEY_B("normalize_print_exposure", m.normalizePrintExposure) KEY_D("y_filter_shift", m.yFilterShift)
        KEY_D("m_filter_shift", m.mFilterShift) KEY_D("y_filter_neutral", m.yFilterNeutral)
        KEY_D("m_filter_neutral", m.mFilterNeutral) KEY_D("c_filter_neutral", m.cFilterNeutral)
        KEY_UNSUPPORTED("lens_blur", 0.0) KEY_D("preflash_exposure", m.preflashExposure)
        KEY_D("preflash_y_filter_shift", m.preflashYFilterShift) KEY_D("preflash_m_filter_shift", m.preflashMFilterShift)
        if (k == "diffusion_filter") {
          if (!diffusion(v, m.diffusionFilter, c, path + k + ".")) return false;
          continue;
        }
        UNKNOWN()
      }
    } else if (top == "scanner") {
      auto& m = p.scanner;
      for (const auto& [k, v] : tv.obj) {
        KEY_D("lens_blur", m.lensBlur) KEY_B("white_correction", m.whiteCorrection)
        KEY_B("black_correction", m.blackCorrection) KEY_D("white_level", m.whiteLevel)
        KEY_D("black_level", m.blackLevel) KEY_A("unsharp_mask", m.unsharpMask)
        UNKNOWN()
      }
    } else if (top == "io") {
      auto& m = p.io;
      for (const auto& [k, v] : tv.obj) {
        if (k == "input_color_space") {
          std::string s;
          if (!getS(v, s) || (s != "ProPhoto RGB" && s != "ProPhotoRGB"))
            return c.fail(path + k, "only 'ProPhoto RGB' input is supported");
          continue;
        }
        if (k == "output_color_space") {
          std::string s;
          if (!getS(v, s) || s != "sRGB") return c.fail(path + k, "only 'sRGB' output is supported");
          continue;
        }
        if (k == "input_cctf_decoding" || k == "output_cctf_encoding") {
          bool b;
          if (!getB(v, b) || b != (k == "output_cctf_encoding")) return c.fail(path + k, "unsupported value");
          continue;
        }
        if (k == "input_gamut_compress") {
          std::array<double, 3>* none = nullptr;
          if (!gamutBlock(v, m.inputGamutCompress, m.inputKnee, none, true, c, path + k + ".")) return false;
          continue;
        }
        if (k == "output_gamut_compress") {
          bool dummy = m.outputGamutCompress;
          KneeParams kneeDummy{0.0, 1.0, 6.0};
          std::array<double, 3> lightDummy{0.7, 1.0, 2.2};
          if (!gamutBlock(v, dummy, kneeDummy, &lightDummy, false, c, path + k + ".")) return false;
          m.outputGamutCompress = dummy;
          continue;
        }
        KEY_B("scan_film", m.scanFilm)
        KEY_IGNORE("crop") KEY_IGNORE("crop_center") KEY_IGNORE("crop_size") KEY_IGNORE("upscale_factor")
        UNKNOWN()
      }
    } else if (top == "settings") {
      auto& m = p.settings;
      for (const auto& [k, v] : tv.obj) {
        if (k == "rgb_to_raw_method") {
          std::string s;
          if (!getS(v, s) || s != "hanatos2025") return c.fail(path + k, "only 'hanatos2025' is supported");
          continue;
        }
        KEY_B("apply_hanatos2025_adaptation_window", m.applyWindow)
        KEY_B("apply_hanatos2025_adaptation_surface", m.applySurface)
        KEY_B("apply_stocks_specifics", m.applyStocksSpecifics)
        KEY_B("neutral_print_filters_from_database", m.neutralPrintFilters)
        KEY_I("bake_lut_resolution", m.lutResolution) KEY_B("preview_mode", m.previewMode)
        // reference accelerators / GUI-only settings: accepted and ignored (we always bake)
        KEY_IGNORE("spectral_gaussian_blur") KEY_IGNORE("use_enlarger_lut") KEY_IGNORE("use_scanner_lut")
        KEY_IGNORE("lut_resolution") KEY_IGNORE("use_fast_stats") KEY_IGNORE("preview_max_size")
        UNKNOWN()
      }
    } else if (top == "debug") {
      auto& m = p.debug;
      for (const auto& [k, v] : tv.obj) {
        KEY_B("deactivate_spatial_effects", m.deactivateSpatialEffects)
        KEY_B("deactivate_stochastic_effects", m.deactivateStochasticEffects)
        KEY_IGNORE("print_timings") KEY_IGNORE("lut_mode")
        UNKNOWN()
      }
    } else {
      return c.fail(top, "unknown section");
    }
  }
  return true;
}

void num(std::ostringstream& o, double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.10g", v);
  o << b;
}
void arr(std::ostringstream& o, const double* v, int n) {
  o << '[';
  for (int i = 0; i < n; ++i) {
    if (i) o << ',';
    num(o, v[i]);
  }
  o << ']';
}
void diff(std::ostringstream& o, const DiffusionFilterParams& d) {
  o << "{\"active\":" << (d.active ? "true" : "false") << ",\"filter_family\":\"" << d.filterFamily << "\",\"strength\":";
  num(o, d.strength);
  o << ",\"spatial_scale\":";
  num(o, d.spatialScale);
  o << ",\"halo_warmth\":";
  num(o, d.haloWarmth);
  o << ",\"core_intensity\":";
  num(o, d.coreIntensity);
  o << ",\"core_size\":";
  num(o, d.coreSize);
  o << ",\"halo_intensity\":";
  num(o, d.haloIntensity);
  o << ",\"halo_size\":";
  num(o, d.haloSize);
  o << ",\"bloom_intensity\":";
  num(o, d.bloomIntensity);
  o << ",\"bloom_size\":";
  num(o, d.bloomSize);
  o << '}';
}
const char* tf(bool b) { return b ? "true" : "false"; }

}  // namespace

bool FilmParams::validate(std::string* err) const {
  const char* badName = nullptr;
  auto chk = [&](const char* name, double v, double lo, double hi) {
    if (!badName && !(std::isfinite(v) && v >= lo && v <= hi)) badName = name;
  };
  auto chk3 = [&](const char* name, const std::array<double, 3>& a, double lo, double hi) {
    for (double v : a) chk(name, v, lo, hi);
  };
  auto diffuse = [&](const char* name, const DiffusionFilterParams& d) {
    chk(name, d.strength, 0, 100);
    chk(name, d.spatialScale, 0, 100);
    chk(name, d.haloWarmth, -10, 10);
    for (double v : {d.coreIntensity, d.coreSize, d.haloIntensity, d.haloSize, d.bloomIntensity, d.bloomSize})
      chk(name, v, 0, 100);
  };
  constexpr double kUm = 1e5;  // 10 cm on film: far beyond any physical blur
  // camera
  chk("camera.exposure_compensation_ev", camera.exposureCompensationEv, -20, 20);
  chk("camera.lens_blur_um", camera.lensBlurUm, 0, kUm);
  chk("camera.film_format_mm", camera.filmFormatMm, 1, 1000);
  chk3("camera.filter_uv", camera.filterUv, -1e5, 1e5);
  chk3("camera.filter_ir", camera.filterIr, -1e5, 1e5);
  diffuse("camera.diffusion_filter", camera.diffusionFilter);
  // film render
  chk("film_render.density_curve_gamma", filmRender.densityCurveGamma, 0.01, 100);
  chk3("film_render.density_curve_gamma_rgb", filmRender.densityCurveGammaRgb, 0.01, 100);
  {
    const auto& g = filmRender.grain;
    chk("grain.particle_area_um2", g.particleAreaUm2, 0, 1e4);
    chk3("grain.particle_scale", g.particleScale, 0, 1e3);
    chk3("grain.particle_scale_layers", g.particleScaleLayers, 0, 1e3);
    chk3("grain.density_min", g.densityMin, 0, 10);
    chk3("grain.uniformity", g.uniformity, 0, 1);
    chk("grain.blur", g.blur, 0, 64);
    chk("grain.blur_dye_clouds_um", g.blurDyeCloudsUm, 0, kUm);
    chk("grain.micro_structure", g.microStructure[0], 0, kUm);
    chk("grain.micro_structure", g.microStructure[1], 0, kUm);
    chk("grain.n_sub_layers", g.nSubLayers, 1, 16);
    const auto& h = filmRender.halation;
    chk("halation.scatter_amount", h.scatterAmount, 0, 10);
    chk("halation.scatter_spatial_scale", h.scatterSpatialScale, 0, 1e3);
    chk("halation.halation_amount", h.halationAmount, 0, 100);
    chk("halation.halation_spatial_scale", h.halationSpatialScale, 0, 1e3);
    chk3("halation.scatter_core_um", h.scatterCoreUm, 0, kUm);
    chk3("halation.scatter_tail_um", h.scatterTailUm, 0, kUm);
    chk3("halation.scatter_tail_weight", h.scatterTailWeight, 0, 1);
    chk("halation.boost_ev", h.boostEv, 0, 20);
    chk("halation.boost_range", h.boostRange, 0, 1);
    chk("halation.protect_ev", h.protectEv, 0, 20);
    chk3("halation.halation_strength", h.halationStrength, 0, 100);
    chk3("halation.halation_first_sigma_um", h.halationFirstSigmaUm, 0, kUm);
    chk("halation.halation_n_bounces", h.halationNBounces, 0, 16);
    chk("halation.halation_bounce_decay", h.halationBounceDecay, 0, 1);
    const auto& d = filmRender.dirCouplers;
    chk("dir_couplers.amount", d.amount, 0, 100);
    chk("dir_couplers.inhibition_samelayer", d.inhibitionSamelayer, 0, 100);
    chk("dir_couplers.inhibition_interlayer", d.inhibitionInterlayer, 0, 100);
    chk3("dir_couplers.gamma_samelayer_rgb", d.gammaSamelayerRgb, -10, 10);
    for (double v : d.gammaInterlayerRToGb) chk("dir_couplers.gamma_interlayer", v, -10, 10);
    for (double v : d.gammaInterlayerGToRb) chk("dir_couplers.gamma_interlayer", v, -10, 10);
    for (double v : d.gammaInterlayerBToRg) chk("dir_couplers.gamma_interlayer", v, -10, 10);
    chk("dir_couplers.diffusion_size_um", d.diffusionSizeUm, 0, kUm);
    chk("dir_couplers.diffusion_tail_um", d.diffusionTailUm, 0, kUm);
    chk("dir_couplers.diffusion_tail_weight", d.diffusionTailWeight, 0, 1);
    chk("film_render.glare.percent", filmRender.glare.percent, 0, 100);
    chk("film_render.glare.roughness", filmRender.glare.roughness, 0, 10);
    chk("film_render.glare.blur", filmRender.glare.blur, 0, 64);
  }
  // print render
  {
    const auto& g = printRender.glare;
    chk("print_render.glare.percent", g.percent, 0, 100);
    chk("print_render.glare.roughness", g.roughness, 0, 10);
    chk("print_render.glare.blur", g.blur, 0, 64);
    const auto& m = printRender.densityCurvesMorph;
    for (double v : {m.gammaFactor, m.gammaFactorFast, m.gammaFactorSlow, m.gammaFactorRed, m.gammaFactorGreen,
                     m.gammaFactorBlue})
      chk("print_render.density_curves_morph.gamma_factor", v, 0.01, 100);
  }
  // enlarger
  {
    const auto& e = enlarger;
    chk("enlarger.print_exposure", e.printExposure, 1e-3, 1e3);
    chk("enlarger.y_filter_shift", e.yFilterShift, -500, 500);
    chk("enlarger.m_filter_shift", e.mFilterShift, -500, 500);
    chk("enlarger.y_filter_neutral", e.yFilterNeutral, -500, 500);
    chk("enlarger.m_filter_neutral", e.mFilterNeutral, -500, 500);
    chk("enlarger.c_filter_neutral", e.cFilterNeutral, -500, 500);
    chk("enlarger.preflash_exposure", e.preflashExposure, 0, 100);
    chk("enlarger.preflash_y_filter_shift", e.preflashYFilterShift, -500, 500);
    chk("enlarger.preflash_m_filter_shift", e.preflashMFilterShift, -500, 500);
    diffuse("enlarger.diffusion_filter", e.diffusionFilter);
  }
  // scanner
  chk("scanner.lens_blur", scanner.lensBlur, 0, 64);
  chk("scanner.white_level", scanner.whiteLevel, -1, 2);
  chk("scanner.black_level", scanner.blackLevel, -1, 2);
  chk("scanner.unsharp_mask", scanner.unsharpMask[0], 0, 64);
  chk("scanner.unsharp_mask", scanner.unsharpMask[1], 0, 1e3);
  for (double v : {io.inputKnee.threshold, io.inputKnee.limit, io.inputKnee.power})
    chk("io.input_gamut_compress.knee", v, -1e3, 1e3);
  chk("settings.bake_lut_resolution", settings.lutResolution, 9, 129);
  chk("settings.tile_size", settings.tileWidth, 0, 1 << 16);
  chk("settings.tile_size", settings.tileHeight, 0, 1 << 16);
  if (badName) {
    if (err) *err = std::string(badName) + ": value out of range or not finite";
    return false;
  }
  return true;
}

bool FilmParams::applyJson(const std::string& json, std::string* err) {
  JValue root;
  std::string perr;
  if (!detail::parseJson(json, root, &perr)) {
    if (err) *err = perr;
    return false;
  }
  FilmParams copy = *this;
  Ctx c;
  if (!applyAll(root, copy, c)) {
    if (err) *err = c.err;
    return false;
  }
  if (!copy.validate(err)) return false;
  *this = std::move(copy);
  return true;
}

void FilmParams::disableSpatialAndStochastic() {
  debug.deactivateSpatialEffects = true;
  debug.deactivateStochasticEffects = true;
  filmRender.glare.active = false;
  printRender.glare.active = false;
  filmRender.dirCouplers.diffusionTailWeight = 0;
}

std::string FilmParams::toJson() const {
  std::ostringstream o;
  o << "{\"film\":\"" << film << "\",\"print\":\"" << paper << "\",";
  o << "\"camera\":{\"exposure_compensation_ev\":";
  num(o, camera.exposureCompensationEv);
  o << ",\"auto_exposure\":" << tf(camera.autoExposure) << ",\"auto_exposure_method\":\"" << camera.autoExposureMethod
    << "\",\"lens_blur_um\":";
  num(o, camera.lensBlurUm);
  o << ",\"film_format_mm\":";
  num(o, camera.filmFormatMm);
  o << ",\"filter_uv\":";
  arr(o, camera.filterUv.data(), 3);
  o << ",\"filter_ir\":";
  arr(o, camera.filterIr.data(), 3);
  o << ",\"diffusion_filter\":";
  diff(o, camera.diffusionFilter);
  o << "},\"film_render\":{\"density_curve_gamma\":";
  num(o, filmRender.densityCurveGamma);
  const auto& g = filmRender.grain;
  o << ",\"grain\":{\"active\":" << tf(g.active) << ",\"sublayers_active\":" << tf(g.sublayersActive)
    << ",\"particle_area_um2\":";
  num(o, g.particleAreaUm2);
  o << ",\"particle_scale\":";
  arr(o, g.particleScale.data(), 3);
  o << ",\"particle_scale_layers\":";
  arr(o, g.particleScaleLayers.data(), 3);
  o << ",\"density_min\":";
  arr(o, g.densityMin.data(), 3);
  o << ",\"uniformity\":";
  arr(o, g.uniformity.data(), 3);
  o << ",\"blur\":";
  num(o, g.blur);
  o << ",\"blur_dye_clouds_um\":";
  num(o, g.blurDyeCloudsUm);
  o << ",\"micro_structure\":";
  arr(o, g.microStructure.data(), 2);
  o << ",\"n_sub_layers\":" << g.nSubLayers << "}";
  const auto& h = filmRender.halation;
  o << ",\"halation\":{\"active\":" << tf(h.active) << ",\"scatter_amount\":";
  num(o, h.scatterAmount);
  o << ",\"scatter_spatial_scale\":";
  num(o, h.scatterSpatialScale);
  o << ",\"halation_amount\":";
  num(o, h.halationAmount);
  o << ",\"halation_spatial_scale\":";
  num(o, h.halationSpatialScale);
  o << ",\"scatter_core_um\":";
  arr(o, h.scatterCoreUm.data(), 3);
  o << ",\"scatter_tail_um\":";
  arr(o, h.scatterTailUm.data(), 3);
  o << ",\"scatter_tail_weight\":";
  arr(o, h.scatterTailWeight.data(), 3);
  o << ",\"boost_ev\":";
  num(o, h.boostEv);
  o << ",\"boost_range\":";
  num(o, h.boostRange);
  o << ",\"protect_ev\":";
  num(o, h.protectEv);
  o << ",\"halation_strength\":";
  arr(o, h.halationStrength.data(), 3);
  o << ",\"halation_first_sigma_um\":";
  arr(o, h.halationFirstSigmaUm.data(), 3);
  o << ",\"halation_n_bounces\":" << h.halationNBounces << ",\"halation_bounce_decay\":";
  num(o, h.halationBounceDecay);
  o << ",\"halation_renormalize\":" << tf(h.halationRenormalize) << "}";
  const auto& d = filmRender.dirCouplers;
  o << ",\"dir_couplers\":{\"active\":" << tf(d.active) << ",\"amount\":";
  num(o, d.amount);
  o << ",\"inhibition_samelayer\":";
  num(o, d.inhibitionSamelayer);
  o << ",\"inhibition_interlayer\":";
  num(o, d.inhibitionInterlayer);
  o << ",\"gamma_samelayer_rgb\":";
  arr(o, d.gammaSamelayerRgb.data(), 3);
  o << ",\"gamma_interlayer_r_to_gb\":";
  arr(o, d.gammaInterlayerRToGb.data(), 2);
  o << ",\"gamma_interlayer_g_to_rb\":";
  arr(o, d.gammaInterlayerGToRb.data(), 2);
  o << ",\"gamma_interlayer_b_to_rg\":";
  arr(o, d.gammaInterlayerBToRg.data(), 2);
  o << ",\"diffusion_size_um\":";
  num(o, d.diffusionSizeUm);
  o << ",\"diffusion_tail_um\":";
  num(o, d.diffusionTailUm);
  o << ",\"diffusion_tail_weight\":";
  num(o, d.diffusionTailWeight);
  o << "},\"glare\":{\"active\":" << tf(filmRender.glare.active) << ",\"percent\":";
  num(o, filmRender.glare.percent);
  o << ",\"roughness\":";
  num(o, filmRender.glare.roughness);
  o << ",\"blur\":";
  num(o, filmRender.glare.blur);
  o << "}},\"print_render\":{\"glare\":{\"active\":" << tf(printRender.glare.active) << ",\"percent\":";
  num(o, printRender.glare.percent);
  o << ",\"roughness\":";
  num(o, printRender.glare.roughness);
  o << ",\"blur\":";
  num(o, printRender.glare.blur);
  const auto& m = printRender.densityCurvesMorph;
  o << "},\"density_curves_morph\":{\"active\":" << tf(m.active) << ",\"gamma_factor\":";
  num(o, m.gammaFactor);
  o << ",\"gamma_factor_fast\":";
  num(o, m.gammaFactorFast);
  o << ",\"gamma_factor_slow\":";
  num(o, m.gammaFactorSlow);
  o << ",\"gamma_factor_red\":";
  num(o, m.gammaFactorRed);
  o << ",\"gamma_factor_green\":";
  num(o, m.gammaFactorGreen);
  o << ",\"gamma_factor_blue\":";
  num(o, m.gammaFactorBlue);
  o << ",\"developer_exhaustion\":0";
  o << "}},\"enlarger\":{\"illuminant\":\"" << enlarger.illuminant << "\",\"print_exposure\":";
  num(o, enlarger.printExposure);
  o << ",\"print_exposure_compensation\":" << tf(enlarger.printExposureCompensation)
    << ",\"normalize_print_exposure\":" << tf(enlarger.normalizePrintExposure) << ",\"y_filter_shift\":";
  num(o, enlarger.yFilterShift);
  o << ",\"m_filter_shift\":";
  num(o, enlarger.mFilterShift);
  o << ",\"y_filter_neutral\":";
  num(o, enlarger.yFilterNeutral);
  o << ",\"m_filter_neutral\":";
  num(o, enlarger.mFilterNeutral);
  o << ",\"c_filter_neutral\":";
  num(o, enlarger.cFilterNeutral);
  o << ",\"lens_blur\":0,\"preflash_exposure\":";
  num(o, enlarger.preflashExposure);
  o << ",\"preflash_y_filter_shift\":";
  num(o, enlarger.preflashYFilterShift);
  o << ",\"preflash_m_filter_shift\":";
  num(o, enlarger.preflashMFilterShift);
  o << ",\"diffusion_filter\":";
  diff(o, enlarger.diffusionFilter);
  o << "},\"scanner\":{\"lens_blur\":";
  num(o, scanner.lensBlur);
  o << ",\"white_correction\":" << tf(scanner.whiteCorrection) << ",\"black_correction\":" << tf(scanner.blackCorrection)
    << ",\"white_level\":";
  num(o, scanner.whiteLevel);
  o << ",\"black_level\":";
  num(o, scanner.blackLevel);
  o << ",\"unsharp_mask\":";
  arr(o, scanner.unsharpMask.data(), 2);
  o << "},\"io\":{\"input_color_space\":\"ProPhoto RGB\",\"input_cctf_decoding\":false,\"output_color_space\":\"sRGB\","
       "\"output_cctf_encoding\":true,\"input_gamut_compress\":{\"active\":"
    << tf(io.inputGamutCompress) << ",\"algorithm\":\"xy\",\"knee\":";
  const double ik[3] = {io.inputKnee.threshold, io.inputKnee.limit, io.inputKnee.power};
  arr(o, ik, 3);
  o << "},\"output_gamut_compress\":{\"algorithm\":\"" << (io.outputGamutCompress ? "cam16ucs" : "off") << "\",\"knee\":";
  o << "[0,1,6],\"lightness_compression\":[0.7,1,2.2]";
  o << "},\"scan_film\":" << tf(io.scanFilm) << "},\"debug\":{\"deactivate_spatial_effects\":"
    << tf(debug.deactivateSpatialEffects) << ",\"deactivate_stochastic_effects\":" << tf(debug.deactivateStochasticEffects)
    << "},\"settings\":{\"apply_hanatos2025_adaptation_window\":" << tf(settings.applyWindow)
    << ",\"apply_hanatos2025_adaptation_surface\":" << tf(settings.applySurface)
    << ",\"apply_stocks_specifics\":" << tf(settings.applyStocksSpecifics)
    << ",\"neutral_print_filters_from_database\":" << tf(settings.neutralPrintFilters)
    << ",\"bake_lut_resolution\":" << settings.lutResolution << ",\"preview_mode\":" << tf(settings.previewMode) << "}}";
  return o.str();
}

}  // namespace dapple::film
