#include "session.h"

namespace {

// What the AI service needs of a session: the photo's identity (hash of the decoded scene-linear picture) and its neutral rendition: default
// params (white balance as shot, exposure anchor, standard base, no look, no local adjustments), geometry "orig", sRGB, <= 1024 px. It never depends
// on the user's edit, so a plane is valid for every edit of the photo.
aimask::Source aiSource(DappleSession* s) {
  aimask::Source src;
  src.owner = s;
  src.photoId = aimask::photoIdOf(s->source.linear.data.data(), s->source.linear.width, s->source.linear.height);
  src.render = [s](aimask::Rendition& r, std::string& err) {
    EditParams p;
    p.crop = "orig", p.rotate = 0, p.straighten = 0.f, p.flip = false, p.outputSpace = 0;
    p.local.clear();
    Image8 img;
    if (!renderSessionParams(s, p, nullptr, aimask::kPlaneLongEdge, img) || img.data.empty()) {
      err = lastError().empty() ? "render failed" : lastError();
      return false;
    }
    r.w = img.width, r.h = img.height, r.rgb = std::move(img.data);
    return true;
  };
  return src;
}
std::vector<std::string> localRasterIds(const EditParams& p) {
  std::vector<std::string> ids;
  for (const LocalAdjustment& a : p.local) {
    if (!a.enabled || a.strength <= 0.f || !(a.ops.anyLinear() || a.ops.anyWindow())) continue;
    for (const MaskComponent& m : a.mask)
      if (m.kind == MaskKind::Raster && !m.ref.empty() && std::find(ids.begin(), ids.end(), m.ref) == ids.end()) ids.push_back(m.ref);
  }
  // the person plane of the reshaping's background protection (section 45) obeys the same rule
  if (p.reshape.active() && !p.reshape.matte.empty() && std::find(ids.begin(), ids.end(), p.reshape.matte) == ids.end()) ids.push_back(p.reshape.matte);
  if (p.reshape.active() && !p.reshape.luma.empty() && std::find(ids.begin(), ids.end(), p.reshape.luma) == ids.end()) ids.push_back(p.reshape.luma);
  return ids;
}

}  // namespace

namespace capi {

std::vector<std::string> missingRasterIds(const EditParams& p) {
  std::vector<std::string> out;
  for (const std::string& id : localRasterIds(p))
    if (!local::rasterHas(id)) out.push_back(id);
  return out;
}
// Export rule: every raster plane the edit uses must exist, else it is re-created (disk cache, then recomputation); if that fails the export fails.
bool ensureLocalRasters(DappleSession* s, const EditParams& p) {
  const std::vector<std::string> missing = missingRasterIds(p);
  if (missing.empty()) return true;
  aimask::Control ctl;
  ctl.yield = false;
  std::string why;
  const std::vector<std::string> still = aimask::ensure(aiSource(s), missing, ctl, &why);
  if (still.empty()) return true;
  setError("export: local mask plane '" + still[0] + "' is missing and could not be re-created (" + why + "); nothing was written");
  return false;
}
// A foreground exact frame never shows a silently empty AI mask: a plane of this photo that the edit references and the store no longer holds
// (evicted by the raster store's caps) is re-created first (disk cache, else recomputation). Failures are not errors of the frame: the mask
// stays empty (logged once) and the app's missing-plane check offers 重新分析.
void recreateLocalRastersSoft(DappleSession* s, const EditParams& p) {
  const std::vector<std::string> missing = missingRasterIds(p);
  if (missing.empty()) return;
  aimask::Control ctl;
  ctl.yield = false;
  std::string why;
  const std::vector<std::string> still = aimask::ensure(aiSource(s), missing, ctl, &why);
  if (!still.empty()) local::logLine("local adjustments: plane '" + still[0].substr(0, 40) + "' could not be re-created (" + why + ")");
}

}  // namespace capi

namespace {

std::string jsonEsc(const std::string& in) {
  std::string o;
  for (char c : in) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

}  // namespace

int32_t dapple_ai_configure(const char* model_dir, const char* cache_dir) {
  return guarded(
      [&]() -> int32_t {
        aimask::configure(model_dir ? model_dir : "", cache_dir ? cache_dir : "");
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
char* dapple_ai_status_json(void) { return guarded([&]() -> char* { return dupString(aimask::availabilityJson()); }, nullptr); }
char* dapple_session_ai_photo_id(DappleSession* s) {
  if (!s) return nullptr;
  return guarded([&]() -> char* { return dupString(aiSource(s).photoId); }, nullptr);
}
int32_t dapple_session_ai_start(DappleSession* s, int32_t tasks, int32_t flags, int32_t max_faces) {
  return guarded(
      [&]() -> int32_t {
        if (!s) {
          setError("null session");
          return -1;
        }
        const aimask::rt::Avail a = aimask::rt::availability(aimask::modelDir());
        if (!(a.person || a.faces || a.depth || a.tap || aimask::rt::compiled())) {
          setError("AI masks unavailable: " + a.reason);
          return -1;
        }
        return aimask::startJob(aiSource(s), tasks & 63, max_faces > 0 ? max_faces : aimask::kMaxFaces, (flags & 1) != 0);
      },
      -1);
}
char* dapple_ai_job_json(int32_t job) { return guarded([&]() -> char* { return dupString(aimask::jobJson(job)); }, nullptr); }
void dapple_ai_job_cancel(int32_t job) { guardedVoid([&] { aimask::cancelJob(job); }); }

char* dapple_session_ai_tap_json(DappleSession* s, float u, float v, int32_t op) {
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        aimask::Control ctl;
        const aimask::TapResult r = aimask::tap(aiSource(s), u, v, op, ctl);
        char b[96];
        std::string j = std::string("{\"ok\":") + (r.ok ? "true" : "false") + ",\"message\":\"" + jsonEsc(r.message) + "\",\"id\":\"" + jsonEsc(r.id) + "\",\"w\":" +
                        std::to_string(r.w) + ",\"h\":" + std::to_string(r.h) + ",\"taps\":[";
        for (std::size_t i = 0; i < r.taps.size(); ++i) {
          std::snprintf(b, sizeof b, "%s[%.4f,%.4f,%d]", i ? "," : "", r.taps[i].x, r.taps[i].y, r.taps[i].add ? 1 : 0);
          j += b;
        }
        std::snprintf(b, sizeof b, "],\"ms\":%.1f,\"prepare_ms\":%.1f}", r.ms, r.prepareMs);
        return dupString(j + b);
      },
      nullptr);
}

int32_t dapple_ai_plane_preview(const char* id, int32_t long_edge, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!id || !out) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        const std::shared_ptr<const local::Raster> r = local::rasterGet(id);
        if (!r) {
          setError("plane preview: id not in the raster store");
          return DAPPLE_ERROR;
        }
        const int L = long_edge > 0 ? long_edge : 128, m = std::max(r->w, r->h);
        const int w = std::max(1, int(std::lround(double(r->w) * L / m))), h = std::max(1, int(std::lround(double(r->h) * L / m)));
        const aimask::Plane p = aimask::resizePlane(r->px.data(), r->w, r->h, w, h);
        auto* px = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
        if (!px) {
          setError("out of memory");
          return DAPPLE_ERROR;
        }
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = p.px[i], px[4 * i + 3] = 255;
        out->width = w, out->height = h, out->rgba = px;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_session_ai_missing_json(DappleSession* s, const char* params_json) {
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        const std::string photo = aimask::photoIdOf(s->source.linear.data.data(), s->source.linear.width, s->source.linear.height);
        std::string j = "{\"missing\":[";
        bool first = true;
        for (const std::string& id : missingRasterIds(params)) {
          const aimask::ParsedId p = aimask::parseId(id);
          j += std::string(first ? "" : ",") + "{\"id\":\"" + jsonEsc(id) + "\",\"ai\":" + (p.ok ? "true" : "false") + ",\"photo\":" + (p.ok && p.photo == photo ? "true" : "false") + "}";
          first = false;
        }
        return dupString(j + "]}");
      },
      nullptr);
}

int32_t dapple_session_ai_ensure(DappleSession* s, const char* params_json, int32_t flags) {
  return guarded(
      [&]() -> int32_t {
        if (!s) {
          setError("null session");
          return -1;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return -1;
        const std::vector<std::string> missing = missingRasterIds(params);
        if (missing.empty()) return 0;
        if (!(flags & 1)) {
          aimask::Control ctl;
          ctl.yield = false;
          std::string why;
          const auto still = aimask::ensure(aiSource(s), missing, ctl, &why);
          if (!still.empty()) setError(why);
          return int32_t(still.size());
        }
        int tasks = 0;
        std::vector<std::string> taps;
        for (const std::string& id : missing) {
          const aimask::ParsedId p = aimask::parseId(id);
          if (!p.ok) continue;
          if (p.group == "person") tasks |= aimask::kTaskPerson;
          else if (p.group == "faces") tasks |= aimask::kTaskFaces;
          else if (p.group == "depth") tasks |= aimask::kTaskDepth;
          else if (p.group == "sky") tasks |= aimask::kTaskSky;
          else taps.push_back(id);
        }
        if (!taps.empty()) {  // tap planes: the disk cache is read, else the taps are replayed (encoder + decoder, a second or two)
          aimask::Control ctl;
          ctl.yield = false;
          std::string why;
          aimask::ensure(aiSource(s), taps, ctl, &why);
        }
        return tasks ? aimask::startJob(aiSource(s), tasks, aimask::kMaxFaces, false) : 0;
      },
      -1);
}
void dapple_ai_release(int32_t all) {
  guardedVoid([&] {
    if (all) aimask::releaseAll();
    else aimask::releaseIdle();
  });
}
