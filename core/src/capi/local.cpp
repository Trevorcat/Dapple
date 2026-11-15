#include "session.h"

// ---- local adjustments (docs/engine/pipeline-architecture.md section 40) ----------------------------------------------------------------
int32_t dapple_mask_raster_set(const char* id, int32_t w, int32_t h, const uint8_t* bytes, int32_t stride) {
  return guarded(
      [&]() -> int32_t {
        if (!id || !bytes || !local::rasterSet(id, w, h, bytes, stride)) {
          setError("mask raster: bad arguments (id, 1..8192 px, bytes)");
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
int32_t dapple_mask_raster_remove(const char* id) { return guarded([&]() -> int32_t { return id && local::rasterRemove(id) ? DAPPLE_OK : DAPPLE_ERROR; }, DAPPLE_ERROR); }
int32_t dapple_mask_raster_has(const char* id) { return guarded([&]() -> int32_t { return id && local::rasterHas(id) ? 1 : 0; }, 0); }

namespace {
// The source of a mask query at long_edge (the session's scaled copy, kept by the Resize cache) and the edit's kernels on it (exact path:
// the same geometry, gain and HDR as a frame).
struct MaskView {
  std::shared_ptr<const SourceImage> hold;
  const SourceImage* src = nullptr;
  std::unique_ptr<engine::point::Params> point;
  std::unique_ptr<engine::point::CpuFrame> fk;
};
bool maskView(DappleSession* s, const EditParams& params, int longEdge, MaskView& mv) {
  if (!ensureSource(s)) return false;
  if (longEdge <= 0) longEdge = 1024;
  mv.hold = s->engine.resizedSource(s->source, longEdge, false);
  mv.src = mv.hold ? mv.hold.get() : &s->source;
  if (mv.src->linear.empty()) return false;
  mv.point = std::make_unique<engine::point::Params>(engine::point::resolve(params, *s->camera, nullptr));
  FastOptions fo;
  fo.spatial = SpatialQuality::Off;
  mv.fk = std::make_unique<engine::point::CpuFrame>(*mv.point, *mv.src, nullptr, fo);
  return mv.fk->valid();
}
}  // namespace

int32_t dapple_session_mask_preview(DappleSession* s, const char* params_json, int32_t index, int32_t long_edge, int32_t flags, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!s || !out) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        if (index < 0 || index >= int(params.local.size())) {
          setError("mask preview: no local adjustment at that index");
          return DAPPLE_ERROR;
        }
        if (flags & 1) {  // the photo in its own orientation
          params.crop = "orig";
          params.rotate = 0;
          params.straighten = 0.f;
          params.flip = false;
        }
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, long_edge, mv)) {
          setError("mask preview: no source");
          return DAPPLE_ERROR;
        }
        const std::shared_ptr<const local::Frame> f = local::bind(params.local, *mv.src, false, true);
        const int w = mv.fk->width(), h = mv.fk->height();
        auto* px = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
        if (!px) {
          setError("out of memory");
          return DAPPLE_ERROR;
        }
        parallelFor(h, [&](int y) {
          std::vector<std::uint8_t> row(std::size_t(w), 0);
          if (f) local::maskRow(*f, *mv.fk, index, y, row.data());
          std::uint8_t* o = px + std::size_t(y) * w * 4;
          for (int x = 0; x < w; ++x) o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = row[std::size_t(x)], o[4 * x + 3] = 255;
        });
        out->width = w;
        out->height = h;
        out->rgba = px;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_session_map_point(DappleSession* s, const char* params_json, int32_t long_edge, float x, float y, int32_t direction, float* u, float* v) {
  return guarded(
      [&]() -> int32_t {
        if (!s || !u || !v) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        if (!ensureSource(s)) return DAPPLE_ERROR;
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        params.local.clear();
        // portrait reshaping (section 45): the geometry part is affine; the field is applied on the photo side of it
        const ReshapeParams reshape = params.reshape;
        params.reshape = ReshapeParams{};
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, long_edge, mv)) {
          setError("map point: no source");
          return DAPPLE_ERROR;
        }
        std::shared_ptr<const warp::Field> wf;
        if (reshape.active()) {
          const bool par = mv.src->parent && mv.src->parent->fullW > 0 && mv.src->parent->fullH > 0;
          wf = warp::bind(reshape, par ? mv.src->parent->fullW : mv.src->linear.width, par ? mv.src->parent->fullH : mv.src->linear.height, false);
        }
        // a region window's (u, v) are fractions of the window; the field is in the parent's
        const bool par = wf && mv.src->parent && mv.src->parent->fullW > 0;
        const double rkx = par ? double(mv.src->linear.width) / mv.src->parent->fullW : 1.0, rbx = par ? double(mv.src->parent->x0) / mv.src->parent->fullW : 0.0;
        const double rky = par ? double(mv.src->linear.height) / mv.src->parent->fullH : 1.0, rby = par ? double(mv.src->parent->y0) / mv.src->parent->fullH : 0.0;
        auto fieldMap = [&](bool fwd, double iu, double iv, double& ou, double& ov) {
          double pu = iu * rkx + rbx, pv = iv * rky + rby;
          if (fwd) warp::forwardUV(*wf, pu, pv, pu, pv);
          else warp::backwardUV(*wf, pu, pv, pu, pv);
          ou = (pu - rbx) / rkx, ov = (pv - rby) / rky;
        };
        // the frame -> photo mapping is affine (rotate 90, straighten, flip, crop): from three probes of srcUV at pixel centres
        float u0, v0, ux, vx, uy, vy;
        mv.fk->srcUV(0, 0, u0, v0);
        mv.fk->srcUV(1, 0, ux, vx);
        mv.fk->srcUV(0, 1, uy, vy);
        const double a = ux - u0, b = uy - u0, c = vx - v0, d = vy - v0;  // (u, v) = (u0, v0) + [a b; c d] (px - 0.5, py - 0.5)
        if (direction == 0) {
          double pu = u0 + a * (x - 0.5) + b * (y - 0.5), pv = v0 + c * (x - 0.5) + d * (y - 0.5);
          if (wf) fieldMap(false, pu, pv, pu, pv);  // the content shown there comes from here
          *u = float(pu);
          *v = float(pv);
        } else {
          const double det = a * d - b * c;
          if (std::fabs(det) < 1e-20) {
            setError("map point: degenerate geometry");
            return DAPPLE_ERROR;
          }
          double px = x, py = y;
          if (wf) fieldMap(true, x, y, px, py);  // where the photo point is shown after the reshaping
          const double du = px - u0, dv = py - v0;
          *u = float((d * du - b * dv) / det + 0.5);
          *v = float((-c * du + a * dv) / det + 0.5);
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_session_mask_sample_json(DappleSession* s, const char* params_json, float u, float v) {
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        if (!ensureSource(s)) return nullptr;
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        params.crop = "orig";
        params.rotate = 0;
        params.straighten = 0.f;
        params.flip = false;
        params.local.clear();
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, 1024, mv)) {
          setError("mask sample: no source");
          return nullptr;
        }
        const int x = std::clamp(int(u * float(mv.fk->width())), 0, mv.fk->width() - 1), y = std::clamp(int(v * float(mv.fk->height())), 0, mv.fk->height() - 1);
        float px[3];
        mv.fk->sourceRow(px, 1, x, y);
        mv.fk->gain(px, 1);
        mv.fk->hdrBlock(px, 1, x, y);
        float L = 0, hue = 0, sat = 0;
        local::sampleRange(px, L, hue, sat);
        char buf[160];
        std::snprintf(buf, sizeof buf, "{\"L\":%.2f,\"hue\":%.1f,\"sat\":%.2f}", L, hue, std::min(sat / 0.25f * 100.f, 400.f));
        return dupString(buf);
      },
      static_cast<char*>(nullptr));
}
