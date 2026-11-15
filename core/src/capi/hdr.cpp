#include "session.h"

namespace {

// The bright rendition of an edit: the same params with the exposure lowered by H (point::resolve clamps exposure to -5, so H is cut to what is left).
bool renderBright(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float H, Image8& out) {
  EditParams p = params;
  p.exposure -= H;
  return renderSessionParams(s, p, lutPath, longEdge, out);
}

// EV of the modern rendering's white point against the standard curve's 1.0 (0 for any other base): the exposure the bright rendition is lowered by
// on top of H, so that the picture's own white point sits at SDR white.
float modernWhiteEv(DappleSession* s, const EditParams& params) {
  if (!isModernId(params.filmStock) || !(params.filmStrength > 0.f)) return 0.f;
  if (!s->proxy) {
    s->proxy = std::make_shared<SourceImage>();
    s->proxy->sceneReferred = s->source.sceneReferred;
    s->proxy->linear = resizeToLongEdge(s->source.linear, 256);
  }
  engine::point::Params r = engine::point::resolve(params, *s->camera, nullptr);
  engine::point::setToneProxy(r, s->proxy.get());
  return std::log2(std::max(1e-3f, engine::point::modernSdrWhite(r, s->source)));
}

}  // namespace

namespace capi {

// Decides H (requested 0 = from the scene) and renders the bright rendition at `longEdge`.
bool hdrDecide(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float requested, HdrDecision& d) {
  d = HdrDecision{};
  if (!params.filmStock.empty() && !isModernId(params.filmStock)) {
    d.reason = 2;
    return true;
  }
  // A modern rendering (section 37): display white is the scene's own white point, not the standard curve's. The bright rendition is the
  // standard rendering of the same edit with the exposure lowered so that the modern white point sits at 1.0 (and by H more): headroom is
  // what lies above the white point of THIS picture.
  EditParams bp = params;
  if (isModernId(params.filmStock)) {
    bp.filmStock.clear();
    bp.filmStrength = 100.f;
    bp.exposure -= modernWhiteEv(s, params);
  }
  const float room = std::max(0.f, bp.exposure + 5.f);  // exposure is clamped to -5 by point::resolve
  const float probeH = std::min(dapple::uhdr::kMaxHeadroomStops, room);
  if (probeH < 0.5f) {
    d.reason = 1;
    return true;
  }
  Image8 probe;
  if (!renderBright(s, bp, lutPath, longEdge, probeH, probe)) return false;
  d.peak = dapple::uhdr::brightPeak(probe, probeH, params.outputSpace == 1);
  float H = requested > 0.f ? std::min(dapple::uhdr::kMaxHeadroomStops, std::max(0.5f, requested)) : dapple::uhdr::autoHeadroom(d.peak);
  H = std::min(H, probeH);
  if (H < 0.5f || d.peak < 1.0001f) {
    d.reason = 1;
    return true;
  }
  d.H = H;
  if (H == probeH) d.bright = std::move(probe);
  else if (!renderBright(s, bp, lutPath, longEdge, H, d.bright)) return false;
  return true;
}

void fillHdrInfo(DappleHdrInfo* info, const HdrDecision& d, bool written, float gainMax, float capMax) {
  if (!info) return;
  info->written = written ? 1 : 0;
  info->reason = d.reason;
  info->headroom_stops = d.H;
  info->peak = d.peak;
  info->gain_max = gainMax;
  info->capacity_max = capMax;
}

bool writeBytesAtomic(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    std::error_code ec2;
    fs::rename(tmp, path, ec2);
    if (ec2) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  return true;
}

}  // namespace capi

int32_t dapple_session_hdr_headroom(DappleSession* session, const char* params_json, const char* lut_path, DappleHdrInfo* info) {
  return guarded(
      [&]() -> int32_t {
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        HdrDecision d;
        if (!hdrDecide(session, params, lut_path, 384, 0.f, d)) return DAPPLE_ERROR;
        fillHdrInfo(info, d, d.reason == 0, 0.f, 0.f);
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

// Share of the picture (0..1) whose colour after the edit lies outside sRGB by more than 0.03 in linear light (a Display P3 render of a 384 px
// proxy, converted back to linear sRGB). Film edits are authored for sRGB and report 0.
int32_t dapple_session_wide_gamut_fraction(DappleSession* session, const char* params_json, const char* lut_path, float* fraction) {
  return guarded(
      [&]() -> int32_t {
        if (!session || !fraction) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        params.outputSpace = 1;
        Image8 img;
        if (!renderSessionParams(session, params, lut_path, 384, img)) return DAPPLE_ERROR;
        static const std::array<float, 256> dec = [] {
          std::array<float, 256> t{};
          for (int i = 0; i < 256; ++i) t[std::size_t(i)] = dapple::color::srgbDecode(float(i) / 255.f);
          return t;
        }();
        const std::size_t n = std::size_t(img.width) * img.height;
        std::size_t out = 0;
        for (std::size_t i = 0; i < n; ++i) {
          const std::uint8_t* px = img.data.data() + 3 * i;
          const float q[3] = {dec[px[0]], dec[px[1]], dec[px[2]]};
          float s[3];
          dapple::color::p3ToSrgb().apply(q, s);
          if (std::min({s[0], s[1], s[2]}) < -0.03f || std::max({s[0], s[1], s[2]}) > 1.03f) ++out;
        }
        *fraction = n ? float(double(out) / double(n)) : 0.f;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

// Not part of the C API (not in dapple_c.h): the test suite reads the bright-rendition shift of a modern rendering (EV, see modernWhiteEv).
extern "C" float dapple_internal_modern_white_ev(DappleSession* session, const char* params_json) {
  return guarded(
      [&]() -> float {
        EditParams params;
        if (!session || !parseParams(params_json, params) || !ensureSource(session)) return 0.f;
        return modernWhiteEv(session, params);
      },
      0.f);
}
