// Local adjustments through masks (docs/engine/pipeline-architecture.md section 40).
//   dapple_local_tests <id>   exit 77: no GPU (parity ids)
//     json        JSON round trip, clamping, caps, unknown component kinds kept verbatim, version tolerance
//     components  every component kind against its analytic reference; combination ops; invert; strength
//     identity    no / disabled / zero local adjustments: no Local node, bit-identical frame; the old Renderer ignores them
//     raster      a synthetic step edge: the refined 256 px mask follows the image edge within 1 px at full resolution
//     tiles       TiledExport == Whole (CPU executor) with every component kind and every op
//     cache       plane builds: a local or global slider drag re-uses the planes, appending a stroke draws only that stroke
//     parity      GPU executor vs CPU executor (frames and a tiled GPU export vs its whole frame)
//     coords      a hard disc stays at its photo point and stays a disc under crop / rotate / flip / straighten
//     interactive the interactive-tier planes vs the exact planes (hand-over bound)
//   not in ctest: bench (N adjustments x op sets, GPU / CPU, 1080 interactive and full), jsoncost (brush JSON / hash / bind cost)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/local_adjust.h"
#include "dapple/renderer.h"
#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "frame_kernels.h"
#include "local_eval.h"
#ifdef DAPPLE_LOCAL_GPU
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"
#endif

namespace {

using namespace dapple;
using namespace dapple::engine;

int g_failures = 0;
#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (0)

const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("", "");
}

// A textured scene: sky gradient on top, a darker landscape below, a bright subject disc, coloured patches.
SourceImage makeScene(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 200.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(11);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
      float r, g, b;
      if (v < 0.45f) {  // sky
        r = 0.25f + 0.2f * v, g = 0.35f + 0.2f * v, b = 0.6f;
      } else {  // ground with texture
        const float t = (((x / 6) + (y / 6)) & 1) ? 1.15f : 0.87f;
        r = 0.05f * t, g = 0.08f * t, b = 0.03f * t;
      }
      const float dx = (u - 0.62f) * w / std::max(w, h), dy = (v - 0.6f) * h / std::max(w, h);
      if (dx * dx + dy * dy < 0.012f) r = 0.5f, g = 0.3f, b = 0.2f;  // subject (skin-ish)
      if (u > 0.1f && u < 0.2f && v > 0.7f && v < 0.85f) r = 0.05f, g = 0.25f, b = 0.04f;  // green patch
      const float n = 1.f + 0.02f * nd(rng);
      float* p = s.linear.row(y) + 3 * std::size_t(x);
      p[0] = std::max(0.f, r * n), p[1] = std::max(0.f, g * n), p[2] = std::max(0.f, b * n);
    }
  return s;
}

LocalAdjustment radialAdj(float cx, float cy, float r, float ev) {
  LocalAdjustment a;
  a.id = "r";
  MaskComponent m;
  m.kind = MaskKind::Radial;
  m.cx = cx, m.cy = cy, m.rx = r, m.ry = r * 0.8f, m.angle = 20.f, m.feather = 60.f;
  a.mask.push_back(m);
  a.ops.v[kLoExposure] = ev;
  return a;
}

// every component kind and every op (raster "t_edge" must be in the store)
std::vector<LocalAdjustment> richEdit() {
  std::vector<LocalAdjustment> L;
  {
    LocalAdjustment a;
    a.id = "sky";
    MaskComponent m;
    m.kind = MaskKind::Linear;
    m.x0 = 0.5f, m.y0 = 0.05f, m.x1 = 0.5f, m.y1 = 0.5f;
    a.mask.push_back(m);
    MaskComponent lum;
    lum.kind = MaskKind::Luminance;
    lum.op = MaskOp::Intersect;
    lum.lo = 30.f, lum.hi = 100.f, lum.lumFeather = 15.f;
    a.mask.push_back(lum);
    a.ops.v[kLoExposure] = -0.8f;
    a.ops.v[kLoTemp] = -20.f;
    a.ops.v[kLoDehaze] = 40.f;
    a.ops.v[kLoHighlights] = -40.f;
    L.push_back(a);
  }
  {
    LocalAdjustment a = radialAdj(0.62f, 0.6f, 0.14f, 0.6f);
    a.id = "subject";
    a.ops.v[kLoClarity] = 50.f;
    a.ops.v[kLoTexture] = -40.f;
    a.ops.v[kLoShadows] = 30.f;
    a.ops.v[kLoSaturation] = 20.f;
    L.push_back(a);
  }
  {
    LocalAdjustment a;
    a.id = "green";
    MaskComponent c;
    c.kind = MaskKind::Colour;
    c.hue = 140.f, c.hueRange = 30.f, c.hueFeather = 20.f, c.satMin = 10.f;
    a.mask.push_back(c);
    a.ops.v[kLoVibrance] = 50.f;
    a.ops.v[kLoColorHue] = 60.f;
    a.ops.v[kLoColorSat] = 40.f;
    L.push_back(a);
  }
  {
    LocalAdjustment a;
    a.id = "brush";
    MaskComponent b;
    b.kind = MaskKind::Brush;
    BrushStroke s;
    s.size = 0.04f, s.hardness = 40.f, s.flow = 80.f;
    for (int i = 0; i <= 20; ++i) s.pts.push_back(0.15f + 0.03f * i), s.pts.push_back(0.8f - 0.01f * i);
    b.strokes.push_back(s);
    BrushStroke e = s;
    e.erase = true;
    e.size = 0.02f;
    e.pts = {0.3f, 0.65f, 0.4f, 0.75f};
    b.strokes.push_back(e);
    a.mask.push_back(b);
    a.ops.v[kLoSharpen] = 60.f;
    a.ops.v[kLoNoise] = 50.f;
    a.ops.v[kLoContrast] = 30.f;
    a.ops.v[kLoWhites] = 20.f;
    a.ops.v[kLoBlacks] = -20.f;
    a.ops.v[kLoTint] = 15.f;
    L.push_back(a);
  }
  {
    LocalAdjustment a;
    a.id = "raster";
    MaskComponent r;
    r.kind = MaskKind::Raster;
    r.ref = "t_edge";
    r.refine = 60.f;
    a.mask.push_back(r);
    MaskComponent sub;
    sub.kind = MaskKind::Radial;
    sub.op = MaskOp::Subtract;
    sub.cx = 0.8f, sub.cy = 0.8f, sub.rx = 0.05f, sub.ry = 0.05f;
    a.mask.push_back(sub);
    a.invert = false;
    a.strength = 80.f;
    a.ops.v[kLoExposure] = 0.5f;
    a.ops.v[kLoClarity] = -30.f;
    L.push_back(a);
  }
  return L;
}

void putEdgeRaster(const char* id, int w, int h, float edgeU, float blurPx) {
  std::vector<std::uint8_t> px(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float d = ((x + 0.5f) / w - edgeU) * w / std::max(blurPx, 0.5f);
      px[std::size_t(y) * w + x] = std::uint8_t(std::clamp(0.5f + 0.5f * std::tanh(-d), 0.f, 1.f) * 255.f + 0.5f);  // 1 left of the edge
    }
  local::rasterSet(id, w, h, px.data(), w);
}

bool exactRender(EngineSession& es, const EditParams& p, const SourceImage& src, ExecMode mode, int tile, Image8& out, ExecStats* st = nullptr) {
  const Renderer r(p, cam(), nullptr);
  EngineSession::ExactOptions o;
  o.mode = mode;
  o.tile = tile;
  o.sessionCache = false;
  std::string err;
  const bool ok = es.renderExact(p, r, src, o, out, &err, st);
  if (!ok) std::printf("  renderExact failed: %s\n", err.c_str());
  return ok;
}

// The Full graph of an exact frame of the edit (what the executors run)
Graph fullGraph(const EditParams& p, const SourceImage& src) {
  const Renderer r(p, cam(), nullptr);
  FastOptions fo;
  FrameKernels fk(r, src, nullptr, fo);
  const FramePlan fp = fk.framePlan();
  ParamInputs in;
  in.edit = p;
  in.frame.exact = true;
  in.ctx.srcW = src.linear.width;
  in.ctx.srcH = src.linear.height;
  const ParamSet ps = ParamSet::make(std::move(in));
  return GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);
}

bool cpuFrame(EngineSession& es, const EditParams& p, const SourceImage& src, bool interactive, FrameResult& res) {
  Renderer r(p, cam(), nullptr);
  FrameJob job{p, r, src, nullptr, cam(), nullptr, {}, {}, {}};
  job.request.bakeSize = 33;
  job.request.fastAnalysis = false;
  job.request.noise = false;
  job.request.interactive = interactive;
  job.fastOptions.spatial = interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
  job.fastOptions.noise = false;
  return es.renderFrame(job, res);
}

// ---- json -------------------------------------------------------------------------------------------------------------------------
void testJson() {
  EditParams p;
  p.local = richEdit();
  p.local[1].name = "主体 \"quoted\"";
  MaskComponent unk;
  unk.kind = MaskKind::Unknown;
  unk.raw = "{\"kind\":\"depth3d\",\"op\":\"add\",\"planes\":[1,2,3],\"x\":{\"y\":true}}";
  p.local[0].mask.push_back(unk);
  const std::string j = toJson(p);
  auto back = fromJson(j);
  CHECK(bool(back));
  if (!back) return;
  CHECK(back->local.size() == p.local.size());
  CHECK(toJson(*back) == j);  // stable ordering: a round trip is byte-identical
  CHECK(back->local[0].mask.back().kind == MaskKind::Unknown);
  CHECK(back->local[0].mask.back().raw.find("depth3d") != std::string::npos);
  CHECK(back->local[1].name == p.local[1].name);
  CHECK(localHash(back->local) == localHash(p.local));
  // clamping, version tolerance, caps
  auto c = fromJson(R"({"local":{"v":7,"items":[{"id":"a","strength":250,"mask":[{"kind":"radial","rx":-3,"feather":500,"future":1}],
      "adjust":{"exposure":999,"contrast":-400,"color_hue":-30,"noise":-5}}]}})");
  CHECK(bool(c));
  if (c) {
    CHECK(c->local.size() == 1);
    const LocalAdjustment& a = c->local[0];
    CHECK(a.strength == 100.f);
    CHECK(a.mask.size() == 1 && a.mask[0].rx >= 0.001f && a.mask[0].feather == 100.f);
    CHECK(a.ops.v[kLoExposure] == 4.f && a.ops.v[kLoContrast] == -100.f && a.ops.v[kLoNoise] == 0.f);
    CHECK(a.ops.v[kLoColorHue] >= 0.f && a.ops.v[kLoColorHue] <= 360.f);
  }
  std::string many = R"({"local":[)";
  for (int i = 0; i < 20; ++i) many += std::string(i ? "," : "") + R"({"id":"x","mask":[{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"},{"kind":"linear"}],"adjust":{"exposure":20}})";
  many += "]}";
  auto m = fromJson(many);
  CHECK(bool(m) && m->local.size() == std::size_t(kMaxLocalAdjustments) && m->local[0].mask.size() == std::size_t(kMaxMaskComponents));
  // an edit without local adjustments writes no "local" key
  CHECK(toJson(EditParams{}).find("local") == std::string::npos);
  std::printf("  json %zu bytes, round trip identical\n", j.size());
}

// ---- components -------------------------------------------------------------------------------------------------------------------
float smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

void testComponents() {
  const SourceImage src = makeScene(300, 200);  // aspect 3:2: sx = 1, sy = 2/3
  const float sx = 1.f, sy = 200.f / 300.f;
  const float grey[3] = {0.18f, 0.18f, 0.18f};
  auto one = [&](const MaskComponent& m) {
    LocalAdjustment a;
    a.mask.push_back(m);
    a.ops.v[kLoExposure] = 1.f;
    return std::vector<LocalAdjustment>{a};
  };
  float worst = 0.f;
  // linear
  {
    MaskComponent m;
    m.kind = MaskKind::Linear;
    m.x0 = 0.2f, m.y0 = 0.3f, m.x1 = 0.6f, m.y1 = 0.7f;
    auto f = local::bind(one(m), src, false);
    for (float u : {0.1f, 0.3f, 0.45f, 0.9f})
      for (float v : {0.2f, 0.5f, 0.8f}) {
        const float dx = (m.x1 - m.x0) * sx, dy = (m.y1 - m.y0) * sy;
        const float t = ((u - m.x0) * sx * dx + (v - m.y0) * sy * dy) / (dx * dx + dy * dy);
        const float ref = 1.f - smooth(0.f, 1.f, t);
        worst = std::max(worst, std::fabs(local::maskOf(*f, 0, u, v, grey, grey) - ref));
      }
  }
  // radial (a circle in the metric: rx = ry, unrotated), hard and feathered
  for (float feather : {0.f, 50.f}) {
    MaskComponent m;
    m.kind = MaskKind::Radial;
    m.cx = 0.5f, m.cy = 0.5f, m.rx = m.ry = 0.2f, m.feather = feather, m.angle = 33.f;
    auto f = local::bind(one(m), src, false);
    for (float u : {0.5f, 0.6f, 0.65f, 0.69f, 0.71f, 0.9f}) {
      const float d = std::fabs(u - 0.5f) * sx / 0.2f;
      const float ref = feather == 0.f ? (d <= 1.f ? 1.f : 0.f) : 1.f - smooth(0.5f, 1.f, d);
      worst = std::max(worst, std::fabs(local::maskOf(*f, 0, u, 0.5f, grey, grey) - ref));
    }
  }
  // luminance: L* of the stage input
  {
    MaskComponent m;
    m.kind = MaskKind::Luminance;
    m.refine = 0.f;  // per-pixel L* (the analytic formula); spatial coherence (section 40.10) is tested in retouch_tests rangesmooth
    m.lo = 40.f, m.hi = 70.f, m.lumFeather = 10.f;
    auto f = local::bind(one(m), src, false);
    for (float Y : {0.02f, 0.1f, 0.18f, 0.3f, 0.45f, 0.6f, 0.9f}) {
      const float lin[3] = {Y, Y, Y};
      const float L = (1.16f * std::cbrt(Y) - 0.16f);
      const float ref = smooth(0.3f, 0.4f, L) * (1.f - smooth(0.7f, 0.8f, L));
      worst = std::max(worst, std::fabs(local::maskOf(*f, 0, 0.5f, 0.5f, lin, lin) - ref));
    }
  }
  // colour: a green selects a green hue, a red / a grey do not
  {
    float Lr, hue, sat;
    const float green[3] = {0.05f, 0.25f, 0.04f}, red[3] = {0.4f, 0.05f, 0.04f};
    local::sampleRange(green, Lr, hue, sat);
    MaskComponent m;
    m.kind = MaskKind::Colour;
    m.hue = hue, m.hueRange = 20.f, m.hueFeather = 10.f, m.satMin = 10.f;
    auto f = local::bind(one(m), src, false);
    const float mg = local::maskOf(*f, 0, 0.5f, 0.5f, green, green), mr = local::maskOf(*f, 0, 0.5f, 0.5f, red, red),
                mn = local::maskOf(*f, 0, 0.5f, 0.5f, grey, grey);
    std::printf("  colour: green hue %.1f sat %.2f -> mask %.3f, red %.3f, grey %.3f\n", hue, sat, mg, mr, mn);
    CHECK(mg > 0.99f && mr < 0.01f && mn < 0.01f);
  }
  // brush: centre of a hard dab = flow, outside = 0; erase
  {
    MaskComponent m;
    m.kind = MaskKind::Brush;
    BrushStroke s;
    s.size = 0.1f, s.hardness = 100.f, s.flow = 70.f;
    s.pts = {0.5f, 0.5f};
    m.strokes.push_back(s);
    auto f = local::bind(one(m), src, false);
    const float c = local::maskOf(*f, 0, 0.5f, 0.5f, grey, grey), o = local::maskOf(*f, 0, 0.8f, 0.5f, grey, grey),
                e = local::maskOf(*f, 0, 0.58f, 0.5f, grey, grey);
    CHECK(std::fabs(c - 0.7f) < 0.01f && o == 0.f && std::fabs(e - 0.7f) < 0.05f);
    BrushStroke er = s;
    er.erase = true, er.flow = 100.f, er.size = 0.03f;
    m.strokes.push_back(er);
    auto f2 = local::bind(one(m), src, false);
    CHECK(local::maskOf(*f2, 0, 0.5f, 0.5f, grey, grey) < 0.01f && std::fabs(local::maskOf(*f2, 0, 0.58f, 0.5f, grey, grey) - e) < 0.01f);
  }
  // raster, no refinement: bilinear of the plane
  {
    putEdgeRaster("t_comp", 64, 64, 0.5f, 0.5f);
    MaskComponent m;
    m.kind = MaskKind::Raster;
    m.ref = "t_comp";
    m.refine = 0.f;
    auto f = local::bind(one(m), src, false);
    CHECK(local::maskOf(*f, 0, 0.2f, 0.5f, grey, grey) > 0.99f && local::maskOf(*f, 0, 0.8f, 0.5f, grey, grey) < 0.01f);
    m.ref = "t_absent";
    auto f2 = local::bind(one(m), src, false);
    CHECK(f2 && local::maskOf(*f2, 0, 0.2f, 0.5f, grey, grey) == 0.f);
  }
  // combination ops, invert, strength
  {
    MaskComponent a, b;
    a.kind = b.kind = MaskKind::Radial;
    a.cx = 0.4f, b.cx = 0.6f, a.cy = b.cy = 0.5f, a.rx = a.ry = b.rx = b.ry = 0.15f, a.feather = b.feather = 0.f;
    const float pa[2] = {0.3f, 0.5f}, pab[2] = {0.5f, 0.5f}, pb[2] = {0.7f, 0.5f};
    struct Op {
      MaskOp op;
      float ea, eab, eb;
    };
    for (const Op& o : {Op{MaskOp::Add, 1, 1, 1}, Op{MaskOp::Subtract, 1, 0, 0}, Op{MaskOp::Intersect, 0, 1, 0}}) {
      b.op = o.op;
      LocalAdjustment adj;
      adj.mask = {a, b};
      adj.ops.v[kLoExposure] = 1.f;
      auto f = local::bind({adj}, src, false);
      CHECK(std::fabs(local::maskOf(*f, 0, pa[0], pa[1], grey, grey) - o.ea) < 1e-4f);
      CHECK(std::fabs(local::maskOf(*f, 0, pab[0], pab[1], grey, grey) - o.eab) < 1e-4f);
      CHECK(std::fabs(local::maskOf(*f, 0, pb[0], pb[1], grey, grey) - o.eb) < 1e-4f);
      adj.invert = true;
      adj.strength = 40.f;
      auto fi = local::bind({adj}, src, false);
      CHECK(std::fabs(local::maskOf(*fi, 0, pab[0], pab[1], grey, grey) - 0.4f * (1.f - o.eab)) < 1e-4f);
    }
  }
  std::printf("  analytic components: worst |mask - reference| %.2e\n", worst);
  CHECK(worst < 1e-4f);
}

// ---- identity ---------------------------------------------------------------------------------------------------------------------
void testIdentity() {
  const SourceImage src = makeScene(320, 214);
  EditParams base;
  base.exposure = 0.3f;
  base.clarity = 15.f;
  EngineSession es;
  FrameResult r0;
  CHECK(cpuFrame(es, base, src, false, r0));
  for (int variant = 0; variant < 3; ++variant) {
    EditParams p = base;
    p.local = {radialAdj(0.5f, 0.5f, 0.2f, 1.f)};
    if (variant == 0) p.local[0].enabled = false;
    if (variant == 1) p.local[0].ops.v[kLoExposure] = 0.f;
    if (variant == 2) p.local[0].strength = 0.f;
    EngineSession es2;
    FrameResult r1;
    CHECK(cpuFrame(es2, p, src, false, r1));
    CHECK(r0.w == r1.w && r0.h == r1.h && std::memcmp(r0.rgba, r1.rgba, std::size_t(r0.w) * r0.h * 4) == 0);
    CHECK(!fullGraph(p, src).find(NodeType::LocalLinear) && !fullGraph(p, src).find(NodeType::LocalDetail));
    std::free(r1.rgba);
  }
  // an active one changes the frame and adds the node
  {
    EditParams p = base;
    p.local = {radialAdj(0.5f, 0.5f, 0.2f, 1.f)};
    EngineSession es2;
    FrameResult r1;
    CHECK(cpuFrame(es2, p, src, false, r1));
    CHECK(std::memcmp(r0.rgba, r1.rgba, std::size_t(r0.w) * r0.h * 4) != 0);
    std::free(r1.rgba);
  }
  // the old Renderer (DAPPLE_ENGINE=off oracle) ignores them
  {
    EditParams p = base;
    p.local = richEdit();
    const Image8 a = Renderer(base, cam(), nullptr).render(src), b = Renderer(p, cam(), nullptr).render(src);
    CHECK(a.data == b.data);
  }
  std::free(r0.rgba);
}

// ---- raster refinement --------------------------------------------------------------------------------------------------------------
void testRaster() {
  // full-resolution picture with a vertical step edge at x = 503.5 (not on a low-res cell boundary), textured both sides
  const int W = 1200, H = 800;
  const float edgeX = 503.f;
  SourceImage src;
  src.sceneReferred = true;
  src.linear = ImageF(W, H);
  std::mt19937 rng(3);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const bool left = x < int(edgeX);
      const float t = 1.f + 0.04f * nd(rng);
      float* p = src.linear.row(y) + 3 * std::size_t(x);
      if (left) p[0] = 0.45f * t, p[1] = 0.32f * t, p[2] = 0.25f * t;  // subject
      else p[0] = 0.08f * t, p[1] = 0.12f * t, p[2] = 0.2f * t;        // background
    }
  // a 256 px mask as a network gives it: the edge where it should be up to the low-res quantisation (0.6 low-res px = 2.8 full-res px off)
  // and soft over about 1.5 low-res px
  const int rw = 256, rh = 171;
  const float offLow = std::getenv("T_OFF") ? float(std::atof(std::getenv("T_OFF"))) : 0.6f;
  putEdgeRaster("t_edge", rw, rh, (edgeX + offLow * W / rw) / W, 1.5f);
  for (float refine : {0.f, 50.f, 100.f}) {
    LocalAdjustment a;
    MaskComponent m;
    m.kind = MaskKind::Raster;
    m.ref = "t_edge";
    m.refine = refine;
    a.mask.push_back(m);
    a.ops.v[kLoExposure] = 1.f;
    auto f = local::bind({a}, src, false);
    double sumErr = 0, maxErr = 0, width = 0;
    int rows = 0;
    for (int y = 40; y < H - 40; y += 37) {
      std::vector<float> mrow(W);
      for (int x = 0; x < W; ++x) {
        const float* p = src.linear.row(y) + 3 * std::size_t(x);
        mrow[std::size_t(x)] = local::maskOf(*f, 0, (x + 0.5f) / W, (y + 0.5f) / H, p, p);
      }
      // the 0.5 crossing (subpixel) and the 10-90 % width
      float cross = -1, x10 = -1, x90 = -1;
      for (int x = 1; x < W; ++x) {
        const float a0 = mrow[std::size_t(x - 1)], a1 = mrow[std::size_t(x)];
        if (cross < 0 && a0 >= 0.5f && a1 < 0.5f) cross = (x - 1) + (a0 - 0.5f) / (a0 - a1) + 0.5f;
        if (x90 < 0 && a1 < 0.9f) x90 = float(x);
        if (x10 < 0 && a1 < 0.1f) x10 = float(x);
      }
      const double e = std::fabs(cross - edgeX);
      sumErr += e, maxErr = std::max(maxErr, e), width += x10 - x90, ++rows;
    }
    std::printf("  refine %3.0f: edge error mean %.2f px, max %.2f px, 90-10 %% width %.1f px\n", refine, sumErr / rows, maxErr, width / rows);
    if (refine > 0.f) CHECK(maxErr <= 1.0);
    else CHECK(maxErr > 2.0);  // the unrefined mask is where the network put it
  }
}

// ---- tiles --------------------------------------------------------------------------------------------------------------------------
void testTiles() {
  const SourceImage src = makeScene(613, 421);
  putEdgeRaster("t_edge", 200, 140, 0.55f, 2.f);
  EditParams p;
  p.exposure = 0.2f;
  p.clarity = 10.f;
  p.sharpen = 30.f;
  p.local = richEdit();
  for (const char* crop : {"orig", "4:5"}) {
    p.crop = crop;
    p.straighten = std::string(crop) == "orig" ? 0.f : 3.f;
    EngineSession es;
    Image8 whole;
    CHECK(exactRender(es, p, src, ExecMode::Whole, 0, whole));
    {
      const Graph g = fullGraph(p, src);
      CHECK(g.find(NodeType::LocalLinear) != nullptr && g.find(NodeType::LocalDetail) != nullptr);
    }
    for (int tile : {96, 200}) {
      Image8 t;
      ExecStats st;
      CHECK(exactRender(es, p, src, ExecMode::TiledExport, tile, t, &st));
      CHECK(t.width == whole.width && t.height == whole.height);
      int maxCode = 0;
      std::size_t differing = 0;
      for (std::size_t i = 0; i < whole.data.size() && i < t.data.size(); ++i) {
        const int d = std::abs(int(whole.data[i]) - int(t.data[i]));
        maxCode = std::max(maxCode, d);
        differing += d != 0;
      }
      std::printf("  crop %-4s tile %3d: %d tiles, max code %d, differing bytes %zu of %zu\n", crop, tile, st.tiles, maxCode, differing, whole.data.size());
      CHECK(st.tiles > 1);
      CHECK(maxCode <= 1);
    }
  }
}

// ---- cache --------------------------------------------------------------------------------------------------------------------------
void testCache() {
  const SourceImage src = makeScene(400, 300);
  putEdgeRaster("t_edge", 128, 96, 0.5f, 2.f);
  local::resetPlaneCache();
  EditParams p;
  p.local = richEdit();
  EngineSession es;
  auto frame = [&](const EditParams& e) {
    FrameResult r;
    CHECK(cpuFrame(es, e, src, true, r));
    std::free(r.rgba);
  };
  frame(p);
  const local::Counters c0 = local::counters();
  EditParams q = p;
  q.local[1].ops.v[kLoExposure] = 1.3f;  // a local slider
  frame(q);
  q.exposure = 0.7f;  // a global slider
  frame(q);
  q.local[0].mask[0].y1 = 0.6f;  // a gradient handle: no plane
  frame(q);
  const local::Counters c1 = local::counters();
  std::printf("  after 3 drags: brush builds %llu -> %llu, raster builds %llu -> %llu, plane hits %llu -> %llu\n",
              (unsigned long long)c0.brushBuilds, (unsigned long long)c1.brushBuilds, (unsigned long long)c0.rasterBuilds,
              (unsigned long long)c1.rasterBuilds, (unsigned long long)c0.planeHits, (unsigned long long)c1.planeHits);
  CHECK(c1.brushBuilds == c0.brushBuilds && c1.rasterBuilds == c0.rasterBuilds && c1.planeHits >= c0.planeHits + 6);
  // live painting: a new stroke, then the same stroke growing
  BrushStroke s;
  s.size = 0.03f;
  s.pts = {0.6f, 0.2f, 0.62f, 0.22f};
  q.local[3].mask[0].strokes.push_back(s);
  frame(q);
  q.local[3].mask[0].strokes.back().pts.insert(q.local[3].mask[0].strokes.back().pts.end(), {0.65f, 0.25f});
  frame(q);
  const local::Counters c2 = local::counters();
  std::printf("  two painting frames: brush builds +%llu, of them from the prefix plane +%llu\n", (unsigned long long)(c2.brushBuilds - c1.brushBuilds),
              (unsigned long long)(c2.brushPrefixHits - c1.brushPrefixHits));
  CHECK(c2.brushBuilds - c1.brushBuilds == 2 && c2.brushPrefixHits - c1.brushPrefixHits == 2);
  CHECK(c2.rasterBuilds == c1.rasterBuilds);
}

// ---- coords -------------------------------------------------------------------------------------------------------------------------
// A hard disc at a photo point stays at that photo point and stays a disc under every geometry (crop / rotate / flip / straighten).
void testCoords() {
  const SourceImage src = makeScene(1200, 900);
  const float W = 1200.f, H = 900.f, longE = 1200.f;
  const float pu = 0.3f, pv = 0.4f, rad = 0.08f;
  struct G {
    const char* crop;
    int rot;
    bool flip;
    float st;
  };
  const G geos[] = {{"orig", 0, false, 0},   {"orig", 90, false, 0},  {"orig", 180, false, 0}, {"orig", 270, false, 0}, {"orig", 0, true, 0},
                    {"orig", 90, true, 0},   {"orig", 0, false, 7},   {"orig", 0, false, -7},  {"1:1", 0, false, 0},    {"1:1", 90, true, 5},
                    {"4:5", 0, false, 0},    {"4:5", 270, false, -7}, {"16:9", 0, false, 0},   {"16:9", 180, true, 7},  {"16:9", 90, false, -7},
                    {"4:5", 90, true, 7},    {"1:1", 180, false, -7}, {"orig", 270, true, 7}};
  int checked = 0, clipped = 0;
  for (const G& g : geos) {
    EditParams p;
    p.crop = g.crop;
    p.rotate = g.rot;
    p.flip = g.flip;
    p.straighten = g.st;
    LocalAdjustment a;
    MaskComponent m;
    m.kind = MaskKind::Radial;
    m.cx = pu, m.cy = pv, m.rx = m.ry = rad, m.angle = 0.f, m.feather = 0.f;
    a.mask.push_back(m);
    a.ops.v[kLoExposure] = 1.f;
    p.local = {a};
    const Renderer r(p, cam(), nullptr);
    FastOptions fo;
    FrameKernels fk(r, src, nullptr, fo);
    auto f = local::bind(p.local, src, false, true);
    CHECK(f != nullptr);
    if (!f) continue;
    const int ow = fk.width(), oh = fk.height();
    std::vector<std::uint8_t> row(static_cast<std::size_t>(ow));
    double sx = 0, sy = 0, n = 0;
    bool touches = false;
    for (int y = 0; y < oh; ++y) {
      local::maskRow(*f, fk, 0, y, row.data());
      for (int x = 0; x < ow; ++x)
        if (row[std::size_t(x)] > 127) {
          sx += x + 0.5, sy += y + 0.5, n += 1;
          if (x == 0 || y == 0 || x == ow - 1 || y == oh - 1) touches = true;
        }
    }
    // source pixels per output pixel (the geometry's scale)
    float u0, v0, u1, v1;
    fk.srcUV(ow / 2, oh / 2, u0, v0);
    fk.srcUV(ow / 2 + 1, oh / 2, u1, v1);
    const double scale = std::hypot((u1 - u0) * W, (v1 - v0) * H);
    if (n == 0 || touches) {
      std::printf("  %-5s rot %3d flip %d straighten %+d: disc %s the frame, skipped\n", g.crop, g.rot, g.flip, int(g.st), n == 0 ? "outside" : "clipped by");
      ++clipped;
      continue;
    }
    // centroid in output pixel coordinates (x + 0.5 = pixel centre) -> photo uv, affine from the pixel containing it
    const float cx = float(sx / n), cy = float(sy / n);
    const int ix = int(std::floor(cx)), iy = int(std::floor(cy));
    float cu, cv, cu2, cv2, cu3, cv3;
    fk.srcUV(ix, iy, cu, cv);
    fk.srcUV(ix + 1, iy, cu2, cv2);
    fk.srcUV(ix, iy + 1, cu3, cv3);
    const float fx = cx - 0.5f - float(ix), fy = cy - 0.5f - float(iy);
    const float eu = cu + (cu2 - cu) * fx + (cu3 - cu) * fy, ev = cv + (cv2 - cv) * fx + (cv3 - cv) * fy;
    const double errPx = std::hypot((eu - pu) * W, (ev - pv) * H) / scale;
    const double area = n, ref = 3.14159265358979 * double(rad * longE) * double(rad * longE) / (scale * scale);
    std::printf("  %-5s rot %3d flip %d straighten %+d: out %dx%d, centroid error %.2f px, area %.0f vs %.0f (%+.2f %%)\n", g.crop, g.rot, g.flip, int(g.st), ow,
                oh, errPx, area, ref, 100.0 * (area / ref - 1.0));
    CHECK(errPx <= 1.5);
    CHECK(std::fabs(area / ref - 1.0) <= 0.03);
    ++checked;
  }
  std::printf("  %d geometries checked, %d skipped (disc leaves the frame)\n", checked, clipped);
  CHECK(checked >= 12);
}

// ---- interactive ----------------------------------------------------------------------------------------------------------------------
// The hand-over from the interactive tier (coarse planes) to the exact frame changes masks by plane resolution only.
void testInteractive() {
  const SourceImage src = makeScene(1600, 1067);
  // variant 0: richEdit as it is (brush size 0.02 -> 400 px plane, raster 256 px: below both interactive caps, so the tiers coincide);
  // variant 1: a fine brush (size 0.004 -> 2000 px plane at the exact tier, 1024 at the interactive one) and a 1024 px raster
  // (grid 1024 exact, 512 interactive): the planes really differ.
  for (int variant = 0; variant < 2; ++variant) {
  if (variant == 0) putEdgeRaster("t_edge", 256, 171, 0.5f, 1.5f);
  else putEdgeRaster("t_edge", 1024, 683, 0.5f, 2.f);
  std::vector<LocalAdjustment> L = richEdit();
  if (variant == 1) {
    for (BrushStroke& st : L[3].mask[0].strokes) st.size = st.erase ? 0.003f : 0.004f;
    L[4].mask[0].refine = 60.f;
  }
  std::printf("  %s\n", variant ? "fine brush (0.004) + 1024 px raster" : "richEdit as is");
  auto fi = local::bind(L, src, true), fe = local::bind(L, src, false);
  CHECK(fi && fe);
  if (!fi || !fe) return;
  CHECK(fi->adj.size() == L.size() && fe->adj.size() == L.size());
  const int G = 200;
  for (std::size_t a = 0; a < fi->adj.size() && a < fe->adj.size(); ++a) {
    double sum = 0, mx = 0;
    for (int j = 0; j < G; ++j)
      for (int i = 0; i < G; ++i) {
        const float u = (i + 0.5f) / G, v = (j + 0.5f) / G;
        const int x = std::min(src.linear.width - 1, int(u * float(src.linear.width))), y = std::min(src.linear.height - 1, int(v * float(src.linear.height)));
        const float* px = src.linear.row(y) + 3 * std::size_t(x);
        const double d = std::fabs(local::maskOf(*fi, int(a), u, v, px, px) - local::maskOf(*fe, int(a), u, v, px, px));
        sum += d, mx = std::max(mx, d);
      }
    std::printf("  adjustment %zu (%s): mean |interactive - exact| %.5f, max %.4f\n", a, L[std::size_t(fi->adj[a].srcIndex)].id.c_str(), sum / (G * G), mx);
    CHECK(sum / (G * G) <= 0.01);
    CHECK(mx <= 0.2);
  }
  }
}

// ---- jsoncost (not in ctest) ----------------------------------------------------------------------------------------------------------
void testJsonCost() {
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
  EditParams p;
  LocalAdjustment a;
  a.id = "b";
  MaskComponent b;
  b.kind = MaskKind::Brush;
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> ud(0.f, 1.f);
  for (int s = 0; s < 200; ++s) {
    BrushStroke st;
    st.size = 0.02f;
    float x = ud(rng), y = ud(rng);
    for (int i = 0; i < 250; ++i) {
      x = std::clamp(x + 0.01f * (ud(rng) - 0.5f), 0.f, 1.f), y = std::clamp(y + 0.01f * (ud(rng) - 0.5f), 0.f, 1.f);
      st.pts.push_back(x), st.pts.push_back(y);
    }
    b.strokes.push_back(st);
  }
  a.mask.push_back(b);
  a.ops.v[kLoExposure] = 0.5f;
  p.local = {a};
  auto t0 = Clock::now();
  const std::string j = toJson(p);
  const double tTo = ms(t0);
  t0 = Clock::now();
  auto back = fromJson(j);
  const double tFrom = ms(t0);
  t0 = Clock::now();
  const std::uint64_t h = localHash(p.local);
  const double tHash = ms(t0);
  CHECK(bool(back) && h == localHash(back->local));
  const SourceImage src = makeScene(1600, 1067);
  local::resetPlaneCache();
  t0 = Clock::now();
  auto f0 = local::bind(p.local, src, true);
  const double tCold = ms(t0);
  t0 = Clock::now();
  auto f1 = local::bind(p.local, src, true);
  const double tWarm = ms(t0);
  auto& last = p.local[0].mask[0].strokes.back().pts;
  last.insert(last.end(), {0.5f, 0.5f, 0.51f, 0.5f, 0.52f, 0.5f, 0.53f, 0.5f, 0.54f, 0.5f});
  t0 = Clock::now();
  auto f2 = local::bind(p.local, src, true);
  const double tApp = ms(t0);
  const local::Counters c = local::counters();
  std::printf("  json %zu bytes (200 strokes x 250 points): toJson %.2f ms, fromJson %.2f ms, localHash %.3f ms\n", j.size(), tTo, tFrom, tHash);
  std::printf("  bind: cold %.2f ms, warm %.3f ms, after appending 5 points to the last stroke %.2f ms (prefix hits %llu, builds %llu)\n", tCold, tWarm, tApp,
              (unsigned long long)c.brushPrefixHits, (unsigned long long)c.brushBuilds);
  CHECK(f0 && f1 && f2);
}

// ---- film frames (docs section 40.7): stage L on the film's scene input, stage W after the film; GPU parity -------------------------------
bool filmLibrary() {
  static int ok = -1;
  if (ok < 0) {
    const char* paths[] = {"core/data/film/dapple.dfp", "core/data/film/profiles"};
    ok = 0;
    for (const char* pth : paths)
      if (std::filesystem::exists(pth)) {
        film::setLibraryPathLazy(pth);
        ok = film::defaultLibrary() != nullptr;
        if (ok) break;
      }
  }
  return ok == 1;
}
double regionMean(const std::uint8_t* px, int w, int h, float u0, float v0, float u1, float v1) {
  double s = 0;
  int n = 0;
  for (int y = int(v0 * h); y < int(v1 * h); ++y)
    for (int x = int(u0 * w); x < int(u1 * w); ++x) s += 0.2126 * px[(y * w + x) * 4] + 0.7152 * px[(y * w + x) * 4 + 1] + 0.0722 * px[(y * w + x) * 4 + 2], ++n;
  return n ? s / n : 0;
}
void testFilm() {
  if (!filmLibrary()) {
    std::printf("  no film library: skipped\n");
    std::exit(77);
  }
  const SourceImage src = makeScene(640, 448);
  for (float strength : {100.f, 50.f}) {
    for (bool window : {false, true}) {
      EditParams a;
      a.filmStock = "kodak_portra_400";
      a.filmStrength = strength;
      EditParams b = a;
      b.local = {radialAdj(0.3f, 0.3f, 0.12f, 1.5f)};
      b.local[0].mask[0].feather = 10.f;
      if (window) b.local[0].ops.v[kLoClarity] = 40.f;
      EngineSession e1, e2;
      FrameResult ra, rb;
      CHECK(cpuFrame(e1, a, src, false, ra) && cpuFrame(e2, b, src, false, rb));
      if (!ra.rgba || !rb.rgba) continue;
      const double in = regionMean(rb.rgba, rb.w, rb.h, 0.27f, 0.27f, 0.33f, 0.33f) - regionMean(ra.rgba, ra.w, ra.h, 0.27f, 0.27f, 0.33f, 0.33f);
      const double out = regionMean(rb.rgba, rb.w, rb.h, 0.7f, 0.7f, 0.9f, 0.9f) - regionMean(ra.rgba, ra.w, ra.h, 0.7f, 0.7f, 0.9f, 0.9f);
      std::printf("  film strength %3.0f%s: inside +%.1f levels, far away %+.2f\n", strength, window ? " + clarity" : "", in, out);
      CHECK(in > 10.0 && std::fabs(out) < 1.0);
      std::free(ra.rgba);
      std::free(rb.rgba);
    }
  }
}

#ifdef DAPPLE_LOCAL_GPU
std::shared_ptr<engine::gpu::Context> gpuContext() {
  std::string err;
  engine::gpu::ContextOptions co;
  co.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") != nullptr;
  auto ctx = engine::gpu::Context::create(co, err);
  if (!ctx) {
    std::printf("no usable GPU context: %s\n", err.c_str());
    std::exit(77);
  }
  std::printf("device: %s\n", ctx->core().dev().caps().text().c_str());
  return ctx;
}

void testParity() {
  auto ctx = gpuContext();
  filmLibrary();
  putEdgeRaster("t_edge", 200, 140, 0.55f, 2.f);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool interactive;
  };
  const std::vector<Case> cases = {
      {"radial exposure", [](EditParams& p) { p.local = {radialAdj(0.5f, 0.5f, 0.2f, 1.f)}; }, false},
      {"all kinds + ops", [](EditParams& p) { p.local = richEdit(); }, false},
      {"all kinds, crop / straighten / flip", [](EditParams& p) { p.local = richEdit(); p.crop = "4:5"; p.straighten = 4.f; p.flip = true; p.rotate = 90; }, false},
      {"all kinds + hdr + global detail", [](EditParams& p) { p.local = richEdit(); p.hdr = 60.f; p.clarity = 20.f; p.dehaze = 15.f; }, false},
      {"all kinds interactive", [](EditParams& p) { p.local = richEdit(); }, true},
      {"film portra + all kinds", [](EditParams& p) { p.local = richEdit(); p.filmStock = "kodak_portra_400"; }, false},
      {"film portra 50 + all kinds, crop", [](EditParams& p) { p.local = richEdit(); p.filmStock = "kodak_portra_400"; p.filmStrength = 50.f; p.crop = "4:5"; }, false},
  };
  for (const std::pair<int, int>& sz : {std::pair<int, int>{640, 448}, std::pair<int, int>{1536, 1024}}) {
    const SourceImage src = makeScene(sz.first, sz.second);
    dapple::gpu::Rig rig(ctx, src, cam());
    EngineSession cpuEs;
    for (const Case& c : cases) {
      EditParams p;
      c.set(p);
      Renderer r(p, cam(), nullptr);
      FastOptions fo;
      fo.spatial = c.interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
      fo.noise = false;
      std::string err;
      int gw = 0, gh = 0;
      engine::GpuStats st;
      std::uint8_t* gpuPx = rig.render(p, r, src, fo, 33, gw, gh, &st, err);
      FrameResult res;
      const bool okCpu = cpuFrame(cpuEs, p, src, c.interactive, res);
      if (!gpuPx || !okCpu || res.w != gw || res.h != gh) {
        std::printf("  %-38s %dx%d: render failed (%s %s)\n", c.name, sz.first, sz.second, err.c_str(), res.error.c_str());
        ++g_failures;
      } else {
        const dapple::gpu::DeStats d = dapple::gpu::compareRgba8(gpuPx, res.rgba, std::size_t(gw) * gh);
        std::printf("  %-38s %4dx%-4d mean %.4f p99 %.3f max %.2f  differing %.1f%%  (%s)\n", c.name, gw, gh, d.mean, d.p99, d.max, d.differing * 100,
                    res.executor.c_str());
        CHECK(d.mean <= 0.15 && d.p99 <= 1.5);
      }
      std::free(gpuPx);
      std::free(res.rgba);
    }
    // a tiled GPU export against the same export in one tile
    EditParams p;
    p.local = richEdit();
    Renderer r(p, cam(), nullptr);
    std::vector<std::uint8_t> a, b;
    int ew = 0, eh = 0;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<std::uint8_t>& dst = pass ? b : a;
      std::string err;
      engine::GpuStats st;
      const bool ok = rig.exportTiled(
          p, r, src, 0, 65, pass ? 192 : 4096, false, 0.f, [&](int w, int h) { ew = w, eh = h, dst.assign(std::size_t(w) * h * 4, 0); },
          [&](int x0, int y0, int tw, int th, const std::uint8_t* px, std::size_t stride) {
            for (int j = 0; j < th; ++j) std::memcpy(&dst[(std::size_t(y0 + j) * ew + x0) * 4], px + std::size_t(j) * stride, std::size_t(tw) * 4);
            return true;
          },
          &st, err);
      if (!ok) std::printf("  export failed: %s\n", err.c_str());
      CHECK(ok);
    }
    std::size_t diff = 0;
    int mx = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const int d = std::abs(int(a[i]) - int(b[i]));
      diff += d != 0, mx = std::max(mx, d);
    }
    std::printf("  GPU tiled export (192 px tiles) vs one tile %dx%d: max code %d, differing bytes %zu\n", ew, eh, mx, diff);
    CHECK(mx <= 1);
  }
}
#endif

// ---- bench (not in ctest) -------------------------------------------------------------------------------------------------------------
std::vector<LocalAdjustment> benchList(int n, bool window) {
  std::vector<LocalAdjustment> L;
  for (int i = 0; i < n; ++i) {
    LocalAdjustment a;
    const float o = 0.05f * float(i / 4);
    switch (i % 4) {
      case 0:
        a = radialAdj(0.35f + o, 0.45f + o, 0.18f, 0.6f);
        break;
      case 1: {
        MaskComponent m;
        m.kind = MaskKind::Linear;
        m.x0 = 0.5f, m.y0 = o, m.x1 = 0.5f, m.y1 = 0.5f;
        a.mask.push_back(m);
        MaskComponent lum;
        lum.kind = MaskKind::Luminance;
        lum.op = MaskOp::Intersect;
        lum.lo = 30.f, lum.hi = 100.f;
        a.mask.push_back(lum);
        break;
      }
      case 2: {
        MaskComponent c;
        c.kind = MaskKind::Colour;
        c.hue = 140.f + 20.f * float(i / 4), c.hueRange = 40.f;
        a.mask.push_back(c);
        break;
      }
      default: {
        MaskComponent b;
        b.kind = MaskKind::Brush;
        BrushStroke s;
        s.size = 0.05f, s.hardness = 50.f;
        for (int k = 0; k <= 30; ++k) s.pts.push_back(0.1f + 0.026f * float(k)), s.pts.push_back(0.7f + 0.1f * std::sin(0.3f * float(k)) + o);
        b.strokes.push_back(s);
        a.mask.push_back(b);
        break;
      }
    }
    a.id = "b" + std::to_string(i);
    a.ops.v[kLoExposure] = i % 2 ? -0.5f : 0.5f;
    a.ops.v[kLoContrast] = 20.f;
    a.ops.v[kLoShadows] = 30.f;
    a.ops.v[kLoSaturation] = 20.f;
    if (window) a.ops.v[kLoClarity] = 30.f, a.ops.v[kLoTexture] = 20.f;
    L.push_back(a);
  }
  return L;
}


double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void testBench() {
  using Clock = std::chrono::steady_clock;
  const int Ns[] = {0, 1, 4, 8};
  const SourceImage big = makeScene(6000, 4000);
  SourceImage small = big;
  small.linear = resizeToLongEdge(big.linear, 1080);
#ifdef DAPPLE_LOCAL_GPU
  auto ctx = gpuContext();
  dapple::gpu::Rig rig(ctx, big, cam());
#endif
  int exactLong = 6000;
  SourceImage exactSrc;  // only when the full-resolution exact render is too slow
  for (int ws = 0; ws < 2; ++ws) {
    struct Row {
      int n;
      double gi[2], gf[2], ci, cf;  // gpu (device, wall), cpu
    };
    std::vector<Row> rows;
    for (int n : Ns) {
      EditParams p;
      p.local = benchList(n, ws == 1);
      Row r{};
      r.n = n;
      r.gi[0] = r.gi[1] = r.gf[0] = r.gf[1] = -1;
#ifdef DAPPLE_LOCAL_GPU
      Renderer rd(p, cam(), nullptr);
      for (int mode = 0; mode < 2; ++mode) {
        const SourceImage& s = mode ? big : small;
        FastOptions fo;
        fo.spatial = mode ? SpatialQuality::Full : SpatialQuality::Interactive;
        fo.noise = false;
        std::vector<double> dev, wall;
        for (int i = 0; i < 9; ++i) {
          std::string err;
          engine::GpuStats st;
          int w = 0, h = 0;
          std::uint8_t* px = rig.render(p, rd, s, fo, 33, w, h, &st, err, 0);
          if (!px) {
            std::printf("  gpu render failed: %s\n", err.c_str());
            break;
          }
          std::free(px);
          if (i >= 2) dev.push_back(st.gpuMs > 0 ? st.gpuMs : st.wallMs), wall.push_back(st.wallMs);
        }
        if (dev.size() >= 7) (mode ? r.gf : r.gi)[0] = median(dev), (mode ? r.gf : r.gi)[1] = median(wall);
      }
#endif
      {
        EngineSession es;
        std::vector<double> v;
        for (int i = 0; i < 9; ++i) {
          FrameResult res;
          const auto t0 = Clock::now();
          if (!cpuFrame(es, p, small, true, res)) break;
          const double d = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
          std::free(res.rgba);
          if (i >= 2) v.push_back(d);
        }
        r.ci = v.size() >= 7 ? median(v) : -1;
      }
      {
        EngineSession es;
        Image8 out;
        auto t0 = Clock::now();
        exactRender(es, p, exactLong == 6000 ? big : exactSrc, ExecMode::TiledExport, 0, out);
        double d = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (exactLong == 6000 && d > 60000.0) {
          std::printf("  CPU exact full resolution took %.1f s: the CPU 'full' column uses 3000 px from here on\n", d / 1000);
          exactLong = 3000;
          exactSrc = big;
          exactSrc.linear = resizeToLongEdge(big.linear, 3000);
          EngineSession es2;
          t0 = Clock::now();
          exactRender(es2, p, exactSrc, ExecMode::TiledExport, 0, out);
          d = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }
        r.cf = d;
      }
      rows.push_back(r);
    }
    std::printf("\n  op set %s, medians of 7 runs (GPU: device ms / wall ms)\n",
                ws ? "(ii) scene-linear + clarity 30, texture 20" : "(i) scene-linear (exposure, contrast, shadows, saturation)");
    std::printf("  %-3s | %-22s | %-22s | %-14s | %-16s\n", "N", "GPU 1080 interactive", "GPU 6000x4000", "CPU 1080 int", exactLong == 6000 ? "CPU exact 6000x4000" : "CPU exact 3000 px");
    for (const Row& r : rows)
      std::printf("  %-3d | %8.2f / %-11.2f | %8.2f / %-11.2f | %10.1f ms | %12.0f ms\n", r.n, r.gi[0], r.gi[1], r.gf[0], r.gf[1], r.ci, r.cf);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"json", testJson}, {"components", testComponents}, {"identity", testIdentity}, {"raster", testRaster}, {"tiles", testTiles},
                     {"cache", testCache}, {"coords", testCoords}, {"interactive", testInteractive}, {"film", testFilm},
#ifdef DAPPLE_LOCAL_GPU
                     {"parity", testParity},
#endif
  };
  const std::string id = argc > 1 ? argv[1] : "";
  bool ran = false;
  if (id == "bench" || id == "jsoncost") {  // not in ctest, not in the default run
    std::printf("[%s]\n", id.c_str());
    if (id == "bench") testBench();
    else testJsonCost();
    return g_failures ? 1 : 0;
  }
  for (const T& t : tests)
    if (id.empty() || id == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      ran = true;
    }
  if (!ran) {
    std::printf("unknown test id '%s'\n", id.c_str());
    return 2;
  }
  std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
  return g_failures ? 1 : 0;
}
