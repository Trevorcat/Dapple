// rawprobe: one RAW file in, one JSON line out (LibRaw facts + the Dapple engine's decode / auto develop result).
//   rawprobe <raw> <out_dir> <tag>     writes <out_dir>/<tag>.jpg (engine, auto, 1600 px) and <tag>_prev.jpg (embedded preview, 1600 px)
// The input is untrusted data: it is only ever parsed by LibRaw / the core decoders, never executed.
// Built by scripts/samples/build_probe.sh against the core of the current tree (see docs/engine/raw-compat.md).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dapple/developer.h"
#include "dapple/encode.h"
#include "dapple/illuminant.h"
#include "libraw/libraw.h"

namespace fs = std::filesystem;
using namespace dapple;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

static std::string esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 32) o += ' ';
    else o += char(c);
  }
  return o;
}

// duv: signed distance of (x,y) from the Planckian/daylight locus at its own CCT in CIE 1960 uv (positive = above the locus = green)
static void uv(double x, double y, double& u, double& v) {
  const double d = -2 * x + 12 * y + 3;
  u = 4 * x / d;
  v = 6 * y / d;
}
static double duvOf(double x, double y, double T) {
  double lx, ly, u, v, lu, lv;
  cctToXy(T, lx, ly);
  uv(x, y, u, v);
  uv(lx, ly, lu, lv);
  const double d = std::hypot(u - lu, v - lv);
  return v >= lv ? d : -d;
}

static double illumCct(int code) {
  switch (code) {
    case 1: case 4: case 9: return 5500;
    case 2: return 4200;
    case 3: case 17: return 2856;
    case 10: case 12: return 6500;
    case 11: return 7500;
    case 13: return 5000;
    case 14: return 4100;
    case 15: return 3450;
    case 16: return 2700;
    case 18: return 4874;
    case 19: return 6774;
    case 20: return 5503;
    case 21: return 6504;
    case 22: return 7504;
    case 23: return 5003;
    case 24: return 3200;
    default: return 0;
  }
}
static double presetCct(int id) {
  switch (id) {
    case LIBRAW_WBI_Daylight: case LIBRAW_WBI_FineWeather: return 5200;
    case LIBRAW_WBI_Cloudy: return 6000;
    case LIBRAW_WBI_Shade: return 7500;
    case LIBRAW_WBI_Tungsten: case LIBRAW_WBI_Ill_A: return 2850;
    case LIBRAW_WBI_Flash: return 5500;
    case LIBRAW_WBI_Fluorescent: return 4000;
    case LIBRAW_WBI_FL_D: return 6500;
    case LIBRAW_WBI_FL_N: return 5000;
    case LIBRAW_WBI_FL_W: return 4200;
    case LIBRAW_WBI_FL_WW: return 3500;
    case LIBRAW_WBI_D55: return 5500;
    case LIBRAW_WBI_D65: return 6500;
    case LIBRAW_WBI_D75: return 7500;
    case LIBRAW_WBI_D50: return 5000;
    case LIBRAW_WBI_StudioTungsten: return 3200;
    default: return 0;
  }
}

static bool inv3(const double m[9], double o[9]) {
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (std::fabs(det) < 1e-12) return false;
  o[0] = (m[4] * m[8] - m[5] * m[7]) / det; o[1] = (m[2] * m[7] - m[1] * m[8]) / det; o[2] = (m[1] * m[5] - m[2] * m[4]) / det;
  o[3] = (m[5] * m[6] - m[3] * m[8]) / det; o[4] = (m[0] * m[8] - m[2] * m[6]) / det; o[5] = (m[2] * m[3] - m[0] * m[5]) / det;
  o[6] = (m[3] * m[7] - m[4] * m[6]) / det; o[7] = (m[1] * m[6] - m[0] * m[7]) / det; o[8] = (m[0] * m[4] - m[1] * m[3]) / det;
  return true;
}

// DNG-spec estimate: iterate neutral -> xy with the ColorMatrix1/2 pair interpolated by mired between the two calibration illuminants.
// Returns CCT (0 = not a dual-matrix DNG). This is the camera profile's own statement of the illuminant (independent of the engine).
static double dngCct(const LibRaw& r, double* duv) {
  const auto& c = r.imgdata.color;
  const double t1 = illumCct(c.dng_color[0].illuminant), t2 = illumCct(c.dng_color[1].illuminant);
  if (t1 <= 0 || t2 <= 0 || c.dng_color[0].colormatrix[0][0] == 0.f || c.dng_color[1].colormatrix[0][0] == 0.f || t1 == t2) return 0;
  float cm[4] = {c.cam_mul[0], c.cam_mul[1], c.cam_mul[2], 0};
  if (cm[0] <= 0 || cm[1] <= 0 || cm[2] <= 0) return 0;
  const double n[3] = {1.0 / cm[0], 1.0 / cm[1], 1.0 / cm[2]};
  double T = 5000;
  for (int it = 0; it < 30; ++it) {
    double w = (1.0 / T - 1.0 / std::max(t1, t2)) / (1.0 / std::min(t1, t2) - 1.0 / std::max(t1, t2));
    w = std::min(1.0, std::max(0.0, w));
    const int lo = t1 < t2 ? 0 : 1, hi = 1 - lo;  // lo = lower CCT matrix
    double m[9];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) m[i * 3 + j] = w * c.dng_color[lo].colormatrix[i][j] + (1 - w) * c.dng_color[hi].colormatrix[i][j];
    double iv[9];
    if (!inv3(m, iv)) return 0;
    const double X = iv[0] * n[0] + iv[1] * n[1] + iv[2] * n[2], Y = iv[3] * n[0] + iv[4] * n[1] + iv[5] * n[2], Z = iv[6] * n[0] + iv[7] * n[1] + iv[8] * n[2];
    const double s = X + Y + Z;
    if (s <= 0) return 0;
    const double x = X / s, y = Y / s, Tn = xyToCct(x, y);
    if (!(Tn > 1500 && Tn < 20000)) return 0;
    if (std::fabs(Tn - T) < 1) { T = Tn; if (duv) *duv = duvOf(x, y, T); return T; }
    T = 0.5 * (T + Tn);
  }
  return T;
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fputs("usage: rawprobe <raw> <out_dir> <tag>\n", stderr); return 2; }
  const fs::path in = fs::path(std::u8string(argv[1], argv[1] + std::strlen(argv[1])));
  const fs::path outDir = argv[2];
  const std::string tag = argv[3];
  std::error_code ec;
  fs::create_directories(outDir, ec);

  std::string j = "{";
  auto kv = [&](const char* k, const std::string& v) { j += std::string(j.size() > 1 ? "," : "") + "\"" + k + "\":\"" + esc(v) + "\""; };
  auto kn = [&](const char* k, double v) { char b[64]; std::snprintf(b, sizeof b, "%.6g", v); j += std::string(j.size() > 1 ? "," : "") + "\"" + k + "\":" + b; };
  auto kraw = [&](const char* k, const std::string& v) { j += std::string(j.size() > 1 ? "," : "") + "\"" + k + "\":" + v; };
  kv("file", argv[1]);

  // ---- LibRaw facts ----
  {
    LibRaw r;
    const int rc = r.open_file(argv[1]);
    if (rc != LIBRAW_SUCCESS) {
      kv("libraw_open_error", libraw_strerror(rc));
    } else {
      const int urc = r.unpack();
      if (urc != LIBRAW_SUCCESS) kv("libraw_unpack_error", libraw_strerror(urc));
      const auto& d = r.imgdata;
      kv("lr_make", d.idata.make);
      kv("lr_model", d.idata.model);
      kn("lr_filters", double(d.idata.filters));
      kn("lr_colors", d.idata.colors);
      kn("lr_raw_count", d.idata.raw_count);
      kn("lr_dng_version", d.idata.dng_version);
      kn("lr_is_foveon", d.idata.is_foveon);
      kn("lr_raw_w", d.sizes.raw_width); kn("lr_raw_h", d.sizes.raw_height);
      kn("lr_w", d.sizes.width); kn("lr_h", d.sizes.height);
      kn("lr_margin_l", d.sizes.left_margin); kn("lr_margin_t", d.sizes.top_margin);
      kn("lr_flip", d.sizes.flip);
      kn("lr_raw_bps", d.color.raw_bps);
      kn("lr_black", d.color.black);
      char b[160];
      std::snprintf(b, sizeof b, "[%u,%u,%u,%u,%u,%u]", d.color.cblack[0], d.color.cblack[1], d.color.cblack[2], d.color.cblack[3], d.color.cblack[4], d.color.cblack[5]);
      kraw("lr_cblack", b);
      kn("lr_maximum", d.color.maximum);
      kn("lr_data_maximum", d.color.data_maximum);
      std::snprintf(b, sizeof b, "[%u,%u,%u,%u]", d.color.linear_max[0], d.color.linear_max[1], d.color.linear_max[2], d.color.linear_max[3]);
      kraw("lr_linear_max", b);
      std::snprintf(b, sizeof b, "[%.4f,%.4f,%.4f,%.4f]", d.color.cam_mul[0], d.color.cam_mul[1], d.color.cam_mul[2], d.color.cam_mul[3]);
      kraw("lr_cam_mul", b);
      kn("lr_has_wbct", [&] { int n = 0; for (int i = 0; i < 64; ++i) n += d.color.WBCT_Coeffs[i][0] > 500.f; return double(n); }());
      kn("lr_dng_pf1", d.color.dng_color[0].parsedfields);
      kn("lr_dng_pf2", d.color.dng_color[1].parsedfields);
      std::snprintf(b, sizeof b, "[%.4f,%.4f,%.4f]", d.color.cam_xyz[0][0], d.color.cam_xyz[0][1], d.color.cam_xyz[0][2]);
      kraw("lr_cam_xyz0", b);
      kn("lr_dng_illum1", d.color.dng_color[0].illuminant);
      kn("lr_dng_illum2", d.color.dng_color[1].illuminant);
      kn("lr_dng_whitelevel0", d.color.dng_levels.dng_whitelevel[0]);
      kn("lr_baseline_exposure", d.color.dng_levels.baseline_exposure);
      std::snprintf(b, sizeof b, "[%u,%u,%u]", d.color.dng_levels.rawopcodes[0].len, d.color.dng_levels.rawopcodes[1].len, d.color.dng_levels.rawopcodes[2].len);
      kraw("lr_rawopcode_len", b);
      // camera-stated CCT: Sony WBCT table (as-shot multipliers on the table), else a named in-camera preset that equals the as-shot multipliers
      double camCct = 0;
      std::string camSrc;
      if (dngCct(r, nullptr) > 0) {
        double duv = 0;
        camCct = dngCct(r, &duv);
        camSrc = "dng-colormatrix-pair";
        kn("cam_duv", duv);
      }
      if (camCct <= 0 && d.color.cam_mul[1] > 0) {
        const double g = d.color.cam_mul[1];
        const double asr = d.color.cam_mul[0] / g, asb = d.color.cam_mul[2] / g;
        for (int i = 0; i < 256 && camCct <= 0; ++i) {
          const int* w = d.color.WB_Coeffs[i];
          if (w[1] <= 0 || presetCct(i) <= 0) continue;
          if (std::fabs(double(w[0]) / w[1] / asr - 1) < 0.01 && std::fabs(double(w[2]) / w[1] / asb - 1) < 0.01) {
            camCct = presetCct(i);
            camSrc = "preset-" + std::to_string(i);
          }
        }
      }
      kn("cam_cct", camCct);
      kv("cam_cct_src", camSrc);
    }
  }

  // ---- engine: probe, full decode time, auto develop ----
  const Developer dev;
  const ImageDecoder* dec = dev.decoders().find(in);
  if (!dec) {
    kv("engine_error", "unsupported file type (no decoder accepts the extension)");
    std::printf("%s}\n", j.c_str());
    return 0;
  }
  kv("decoder", dec->name());
  {
    auto t0 = Clock::now();
    auto full = dec->decode(in, DecodeOptions{DecodeQuality::Full});
    kn("decode_full_ms", ms(t0));
    if (!full) { std::fprintf(stderr, "ERR %zu [%s]\n", full.error().message.size(), full.error().message.c_str()); kv("decode_full_error", full.error().message); }
    else { kn("full_w", full->linear.width); kn("full_h", full->linear.height); }
  }
  DevelopRequest req;
  req.input = in;
  req.autoAdjust = AutoOptions{};
  req.quality = DecodeQuality::Preview;
  req.longEdge = 1600;
  auto t0 = Clock::now();
  auto res = dev.develop(req);
  kn("develop_total_ms", ms(t0));
  if (!res) {
    kv("engine_error", res.error().message);
  } else {
    const ShotMetadata& m = res->meta;
    kv("make", m.make); kv("model", m.model); kv("lens", m.lens);
    kn("iso", m.iso);
    kv("cfa", toString(m.cfa));
    kn("sensor_w", m.sensorWidth); kn("sensor_h", m.sensorHeight);
    kn("bits", m.bitsPerSample);
    kv("raw_format", m.rawFormat);
    char b[96];
    std::snprintf(b, sizeof b, "[%.4f,%.4f,%.4f]", m.asShotWb[0], m.asShotWb[1], m.asShotWb[2]);
    kraw("as_shot_wb", b);
    kn("eng_cct", m.asShotCct);
    kn("eng_x", m.asShotIlluminantXy[0]); kn("eng_y", m.asShotIlluminantXy[1]);
    if (m.asShotIlluminantXy[0] > 0) kn("eng_matrix_cct", xyToCct(m.asShotIlluminantXy[0], m.asShotIlluminantXy[1]));
    if (m.asShotIlluminantXy[0] > 0) kn("eng_duv", duvOf(m.asShotIlluminantXy[0], m.asShotIlluminantXy[1], m.asShotCct > 0 ? m.asShotCct : xyToCct(m.asShotIlluminantXy[0], m.asShotIlluminantXy[1])));
    kv("camera_profile", res->camera ? res->camera->id : "");
    kn("out_w", res->image.width); kn("out_h", res->image.height);
    kn("decode_preview_ms", res->timings.decodeMs);
    kn("analyze_ms", res->timings.analyzeMs); kn("render_ms", res->timings.renderMs);
    if (res->autoResult) {
      const auto& s = res->autoResult->stats;
      kn("auto_median", s.medianLuma); kn("auto_p99", s.p99Luma); kn("auto_clipped", s.clippedFraction);
      kn("auto_cast_a", s.neutralCastA); kn("auto_cast_b", s.neutralCastB);
      std::string notes;
      for (auto& n : res->autoResult->notes) notes += n + "; ";
      kv("auto_notes", notes);
    }
    auto w = writeJpeg(res->image, outDir / (tag + ".jpg"), JpegOptions{92});
    if (!w) kv("write_error", w.error().message);
  }
  auto pv = extractEmbeddedPreview(in, 1600);
  if (!pv) kv("preview_error", pv.error().message);
  else {
    kn("preview_w", pv->width); kn("preview_h", pv->height);
    auto w = writeJpeg(*pv, outDir / (tag + "_prev.jpg"), JpegOptions{92});
    if (!w) kv("preview_write_error", w.error().message);
  }
  std::printf("%s}\n", j.c_str());
  return 0;
}
