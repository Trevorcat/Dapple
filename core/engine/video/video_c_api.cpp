// C API of VideoFrameSession (dapple_c.h, "Edited Motion Photo clips"): compiled into libdapple itself (the exports must live in the shared
// library, see core/CMakeLists.txt). The frames come from the video core (DV_RGBAF16) through a direct buffer; nothing here touches FFmpeg.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"
#include "dapple/ultrahdr.h"
#include "engine/video/video_frame_session.h"

namespace {

using dapple::engine::video::ColourKind;
using dapple::engine::video::InputFrame;
using dapple::engine::video::PackFormat;
using dapple::engine::video::VideoFrameSession;

thread_local std::string g_err;
void setErr(std::string m) { g_err = std::move(m); }

}  // namespace

struct DappleVideoEdit {
  std::unique_ptr<VideoFrameSession> s;
};

namespace {

// Minimal extraction of a numeric / string value from the flat options object (no nesting): the keys are ours.
bool findKey(const std::string& j, const char* key, size_t& pos) {
  const std::string k = std::string("\"") + key + "\"";
  const size_t a = j.find(k);
  if (a == std::string::npos) return false;
  size_t c = j.find(':', a + k.size());
  if (c == std::string::npos) return false;
  ++c;
  while (c < j.size() && (j[c] == ' ' || j[c] == '\t')) ++c;
  pos = c;
  return true;
}
double numOpt(const std::string& j, const char* key, double def) {
  size_t p;
  if (!findKey(j, key, p)) return def;
  if (j.compare(p, 4, "true") == 0) return 1;
  if (j.compare(p, 5, "false") == 0) return 0;
  return std::atof(j.c_str() + p);
}
std::string strOpt(const std::string& j, const char* key) {
  size_t p;
  if (!findKey(j, key, p) || p >= j.size() || j[p] != '"') return {};
  const size_t e = j.find('"', p + 1);
  return e == std::string::npos ? std::string() : j.substr(p + 1, e - p - 1);
}

InputFrame frameOf(const uint16_t* rgba, int32_t w, int32_t h, int32_t kind, int32_t rot) {
  InputFrame f;
  f.rgbaF16 = rgba;
  f.width = w;
  f.height = h;
  f.kind = static_cast<ColourKind>(kind);
  f.rotationDeg = rot;
  return f;
}

}  // namespace

extern "C" {

DAPPLE_API DappleVideoEdit* dapple_video_edit_open(const char* params_json, const char* lut_path, const char* options_json) {
  dapple::EditParams params;
  if (params_json && params_json[0]) {
    auto parsed = dapple::fromJson(params_json);
    if (!parsed) {
      setErr("params: " + parsed.error().message);
      return nullptr;
    }
    params = std::move(*parsed);
  }
  const std::string opt = options_json ? options_json : "";
  dapple::engine::video::Config cfg;
  cfg.params = params;
  const dapple::CameraRegistry cams = dapple::CameraRegistry::withBuiltins();
  cfg.camera = cams.resolve(strOpt(opt, "cameraMake"), strOpt(opt, "cameraModel"));
  const std::string lutFile = (lut_path && lut_path[0]) ? std::string(lut_path) : params.lutPath;  // the edit's own look LUT when none is given
  if (!lutFile.empty()) {
    auto lut = dapple::Lut3D::loadCube(std::filesystem::u8path(lutFile));
    if (!lut) {
      setErr("lut: " + lut.error().message);
      return nullptr;
    }
    cfg.lut = std::make_shared<const dapple::Lut3D>(std::move(*lut));
  }
  cfg.sceneIlluminantX = static_cast<float>(numOpt(opt, "sceneX", 0));
  cfg.sceneIlluminantY = static_cast<float>(numOpt(opt, "sceneY", 0));
  cfg.maxLongEdge = static_cast<int>(numOpt(opt, "maxLongEdge", 0));
  cfg.stillAspect = static_cast<float>(numOpt(opt, "stillAspect", 0));
  cfg.stillLongEdge = static_cast<int>(numOpt(opt, "stillLongEdge", 0));
  cfg.allowGpu = numOpt(opt, "allowGpu", 0) != 0;
  cfg.allowHdr = numOpt(opt, "hdr", 1) != 0;
  auto* h = new DappleVideoEdit;
  h->s = std::make_unique<VideoFrameSession>(std::move(cfg));
  return h;
}

DAPPLE_API void dapple_video_edit_close(DappleVideoEdit* h) { delete h; }

DAPPLE_API int32_t dapple_video_edit_set_reference(DappleVideoEdit* h, const uint16_t* rgba_f16, int32_t w, int32_t h_px, int32_t colour_kind,
                                                   int32_t rotation_deg) {
  if (!h || !rgba_f16) {
    setErr("null argument");
    return DAPPLE_ERROR;
  }
  std::string err;
  if (!h->s->setReference(frameOf(rgba_f16, w, h_px, colour_kind, rotation_deg), &err)) {
    setErr(err);
    return DAPPLE_ERROR;
  }
  return DAPPLE_OK;
}

DAPPLE_API int32_t dapple_video_edit_output_size(DappleVideoEdit* h, int32_t out_wh[2]) {
  if (!h || !out_wh || !h->s->hasReference()) {
    setErr("no reference frame yet");
    return DAPPLE_ERROR;
  }
  out_wh[0] = h->s->outputWidth();
  out_wh[1] = h->s->outputHeight();
  return DAPPLE_OK;
}

DAPPLE_API int64_t dapple_video_edit_frame_bytes(DappleVideoEdit* h, int32_t format) {
  if (!h || !h->s->hasReference() || format < 0 || format > 5) return -1;
  return static_cast<int64_t>(VideoFrameSession::packedBytes(static_cast<PackFormat>(format), h->s->outputWidth(), h->s->outputHeight()));
}

DAPPLE_API int32_t dapple_video_edit_render(DappleVideoEdit* h, const uint16_t* rgba_f16, int32_t w, int32_t h_px, int32_t colour_kind,
                                            int32_t rotation_deg, uint32_t frame_index, int32_t format, uint8_t* dst, int64_t dst_size) {
  if (!h || !rgba_f16 || !dst || format < 0 || format > 5) {
    setErr("bad argument");
    return DAPPLE_ERROR;
  }
  std::vector<uint8_t> out;
  std::string err;
  if (!h->s->render(frameOf(rgba_f16, w, h_px, colour_kind, rotation_deg), frame_index, static_cast<PackFormat>(format), out, nullptr, &err)) {
    setErr(err);
    return DAPPLE_ERROR;
  }
  if (static_cast<int64_t>(out.size()) > dst_size) {
    setErr("destination buffer too small");
    return DAPPLE_ERROR;
  }
  std::memcpy(dst, out.data(), out.size());
  return DAPPLE_OK;
}

DAPPLE_API int32_t dapple_video_edit_output_kind(DappleVideoEdit* h, int32_t tags[4], const char** note) {
  if (!h || !h->s->hasReference()) {
    setErr("no reference frame yet");
    return -1;
  }
  if (tags) h->s->outputTags(tags);
  if (note) *note = h->s->outputNote();
  return static_cast<int32_t>(h->s->outputKind());
}

namespace {

bool readAll(const char* path, std::vector<uint8_t>& out) {
  std::ifstream f(std::filesystem::u8path(path ? path : ""), std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}
bool writeAllAtomic(const char* path, const std::vector<uint8_t>& b) {
  const std::filesystem::path p = std::filesystem::u8path(path ? path : "");
  const std::filesystem::path tmp = p.string() + ".part";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    f.flush();
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    std::filesystem::remove(p, ec);
    std::filesystem::rename(tmp, p, ec);
  }
  return !ec;
}
// Pixel size of a baseline / progressive JPEG from its SOF marker.
bool jpegSize(const std::vector<uint8_t>& j, int& w, int& h) {
  size_t i = 2;
  while (i + 9 < j.size()) {
    if (j[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = j[i + 1];
    if (m == 0xFF) {
      ++i;
      continue;
    }
    if (m >= 0xC0 && m <= 0xC2) {
      h = (j[i + 5] << 8) | j[i + 6];
      w = (j[i + 7] << 8) | j[i + 8];
      return w > 0 && h > 0;
    }
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      i += 2;
      continue;
    }
    i += 2 + size_t((j[i + 2] << 8) | j[i + 3]);
  }
  return false;
}
// Area average of a per-pixel plane into gw x gh.
std::vector<float> boxDown(const std::vector<float>& src, int w, int h, int gw, int gh) {
  std::vector<double> acc(size_t(gw) * gh, 0.0);
  std::vector<int> cnt(size_t(gw) * gh, 0);
  for (int y = 0; y < h; ++y) {
    const int gy = std::min(gh - 1, int(int64_t(y) * gh / h));
    for (int x = 0; x < w; ++x) {
      const int gx = std::min(gw - 1, int(int64_t(x) * gw / w));
      acc[size_t(gy) * gw + gx] += src[size_t(y) * w + x];
      ++cnt[size_t(gy) * gw + gx];
    }
  }
  std::vector<float> out(acc.size());
  for (size_t i = 0; i < out.size(); ++i) out[i] = cnt[i] ? float(acc[i] / cnt[i]) : 0.f;
  return out;
}

}  // namespace

DAPPLE_API int32_t dapple_video_edit_cover_uhdr(DappleVideoEdit* h, const uint16_t* rgba_f16, int32_t w, int32_t h_px, int32_t colour_kind,
                                                int32_t rotation_deg, const char* sdr_jpeg_path, const char* out_path, int32_t force_hdr,
                                                DappleHdrInfo* info) {
  if (info) *info = DappleHdrInfo{0, 0, 0.f, 0.f, 0.f, 0.f};
  if (!h || !rgba_f16 || !sdr_jpeg_path || !out_path) {
    setErr("null argument");
    return DAPPLE_ERROR;
  }
  try {
    std::vector<uint8_t> base;
    if (!readAll(sdr_jpeg_path, base) || base.size() < 4 || base[0] != 0xFF || base[1] != 0xD8) {
      setErr(std::string("cover JPEG unreadable: ") + sdr_jpeg_path);
      return DAPPLE_ERROR;
    }
    auto plain = [&](int reason, float peak) -> int32_t {
      if (!writeAllAtomic(out_path, base)) {
        setErr(std::string("cannot write ") + out_path);
        return DAPPLE_ERROR;
      }
      if (info) {
        info->written = 0;
        info->reason = reason;
        info->peak = peak;
      }
      return DAPPLE_OK;
    };
    std::string err;
    const InputFrame f = frameOf(rgba_f16, w, h_px, colour_kind, rotation_deg);
    if (!h->s->hasReference() && !h->s->setReference(f, &err)) {
      setErr(err);
      return DAPPLE_ERROR;
    }
    if (h->s->outputKind() == dapple::engine::video::OutputKind::Sdr709) return plain(h->s->heldParams().filmStock.empty() ? 1 : 2, 0.f);
    dapple::ImageF sdr, hdr;
    if (!h->s->renderStillHdr(f, sdr, hdr, &err)) {
      setErr(err);
      return DAPPLE_ERROR;
    }
    int bw = 0, bh = 0;
    if (!jpegSize(base, bw, bh)) return plain(3, 0.f);
    const float ab = float(bw) / float(bh), asp = float(sdr.width) / float(sdr.height);
    if (std::fabs(ab - asp) / ab > 0.02f) return plain(3, 0.f);
    // gain map at a quarter of the base picture (never above the rendered size), luminance only: linear luminance is linear, so the area average of
    // the luminance is the luminance of the average
    const int longG = std::max(8, std::min((std::max(bw, bh) + 3) / 4, std::max(sdr.width, sdr.height)));
    const int gw = std::max(1, sdr.width >= sdr.height ? longG : int(std::lround(double(longG) * sdr.width / sdr.height)));
    const int gh = std::max(1, sdr.width >= sdr.height ? int(std::lround(double(longG) * sdr.height / sdr.width)) : longG);
    const size_t n = size_t(sdr.width) * sdr.height;
    std::vector<float> ys(n), yh(n);
    auto dec = [](float e) { return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f); };
    for (size_t i = 0; i < n; ++i) {
      const float* s = &sdr.data[3 * i];
      const float* x = &hdr.data[3 * i];
      float r = dec(std::min(1.f, std::max(0.f, s[0]))), g = dec(std::min(1.f, std::max(0.f, s[1]))), b = dec(std::min(1.f, std::max(0.f, s[2])));
      ys[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      yh[i] = 0.2126f * x[0] + 0.7152f * x[1] + 0.0722f * x[2];
    }
    const std::vector<float> Ys = boxDown(ys, sdr.width, sdr.height, gw, gh), Yh = boxDown(yh, sdr.width, sdr.height, gw, gh);
    const size_t gn = size_t(gw) * gh;
    std::vector<float> sorted = Yh;
    const size_t k = std::min(gn - 1, size_t(double(gn) * 0.999));
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(k), sorted.end());
    const float peak = sorted[k];
    if (!(peak >= (force_hdr ? 1.1f : dapple::uhdr::kWorthPeak))) return plain(1, peak);
    // gain log2((Yh + o) / (Ys + o)) >= 0 with the edge guard, one shared function with the still export
    const dapple::uhdr::GainMap gm = dapple::uhdr::makeGainMapFromLuma(Ys, Yh, gw, gh);
    if (gm.meta.gainMax <= 0.f) return plain(1, peak);
    const float gmax = gm.meta.gainMax;
    auto file = dapple::uhdr::assemble(base, gm);
    if (!file) {
      setErr(file.error().message);
      return DAPPLE_ERROR;
    }
    if (!writeAllAtomic(out_path, *file)) {
      setErr(std::string("cannot write ") + out_path);
      return DAPPLE_ERROR;
    }
    if (info) {
      info->written = 1;
      info->reason = 0;
      info->peak = peak;
      info->headroom_stops = std::log2(std::max(1.f, peak));
      info->gain_max = gmax;
      info->capacity_max = gmax;
    }
    return DAPPLE_OK;
  } catch (const std::exception& e) {
    setErr(std::string("cover_uhdr: ") + e.what());
    return DAPPLE_ERROR;
  }
}

DAPPLE_API const char* dapple_video_edit_last_error(void) { return g_err.c_str(); }

}  // extern "C"
