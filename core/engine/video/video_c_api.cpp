// C API of VideoFrameSession (dapple_c.h, "Edited Motion Photo clips"): compiled into libdapple itself (the exports must live in the shared
// library, see core/CMakeLists.txt). The frames come from the video core (DV_RGBAF16) through a direct buffer; nothing here touches FFmpeg.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "dapple/lut.h"
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

DAPPLE_API const char* dapple_video_edit_last_error(void) { return g_err.c_str(); }

}  // extern "C"
