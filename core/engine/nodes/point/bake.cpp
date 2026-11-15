// The point family's table bakes (docs/engine/pipeline-architecture.md section 50): colour lattice, tonal analysis (+ tonal-class move),
// tone table, analysis, overflow lattice; bake() performs them in sequence. The engine's table nodes (ColourLattice, ToneAnalysisPre,
// ToneTable, Analysis, LatticeOverflow) run the pieces on their own keys.
#include <mutex>
#include <optional>

#include "engine/graph/node.h"
#include "engine/nodes/point/kernels.h"
#include "engine/nodes/vignette/vignette_kernel.h"
#include "parallel.h"
#include "tone_hist.h"

namespace dapple {

BakeCounters& bakeCounters() {
  static BakeCounters c;
  return c;
}

}  // namespace dapple

namespace dapple::engine::point {

namespace {
ToneAnalysis analyseThroughLattice(const Params& rp, const SourceImage& source, const BakedLut& lut);
// A non-owning view of a lattice the caller keeps alive (a cache payload) for the scratch BakedLut of the fast analysis.
std::shared_ptr<const std::vector<float>> borrow(const std::vector<float>* v) { return std::shared_ptr<const std::vector<float>>(std::shared_ptr<void>(), v); }
}  // namespace

std::shared_ptr<const BakedLut> bake(const Params& rp, const SourceImage& source, int n, const BakedLut* colourFrom,
                                               bool fastAnalysis) {
  const std::string ck = colourKey(rp);
  if (colourFrom && (colourFrom->colourKey != ck || colourFrom->latticeFloats() == 0)) colourFrom = nullptr;
  n = colourFrom ? colourFrom->n : std::clamp(n, 17, 65);
  auto lut = std::make_shared<BakedLut>();
  lut->n = n;
  lut->key = pointKey(rp) + (fastAnalysis ? "#f" : "#x") + std::to_string(n);  // fast (lattice) and exact analysis are different bakes
  lut->colourKey = ck;
  lut->latticeKey = ck + "#" + std::to_string(n);
  // The colour lattice first: it does not depend on the tone stage, the tonal analysis or the tonal-class move, so
  // it is either reused (same colourKey) or evaluated with the tone parameters neutralised.
  if (colourFrom) {
    bakeCounters().latticeReused.fetch_add(1, std::memory_order_relaxed);
    lut->rgb = colourFrom->rgb;  // same colour stage: only the tone table and the analysis change
  } else {
    bakeCounters().latticeEvaluated.fetch_add(1, std::memory_order_relaxed);
    lut->rgb = std::make_shared<const std::vector<float>>(bakeColourLattice(rp, source, n));
  }

  // Tonal analysis (histogram + the tonal-class move built from it). Exact: the 256 px proxy through the whole point
  // pipeline (~40 ms on a Snapdragon 680 class phone, on every drag frame). Fast (interactive frames): the proxy
  // through the tone table without the move + the lattice just made, ~20x cheaper; edit-bench measures the same
  // dE2000 vs exact either way. Final frames always use the exact analysis.
  const TonePre pre = fastAnalysis ? bakeTonePre(rp, source, lut->rgb.get(), n, true) : TonePre{};
  BakedLut preL;
  preL.analysis = pre.analysis;
  preL.toneMove = pre.toneMove;
  const PointOp op = makePointOp(rp, source, pre.has ? &preL : nullptr);
  lut->analysis = op.hasAnalysis ? op.analysis : analyzeTone(rp.toneProxy ? *rp.toneProxy : source, *rp.camera, rp.p);
  if (!op.toneMove.identity) lut->toneMove = op.toneMove.table;
  fillToneTable(lut->tone, op);
  bakeCounters().toneTables.fetch_add(1, std::memory_order_relaxed);
  return lut;
}

bool latticeFilmOnly(const Params& rp) {
  const FilmRun fr = resolveFilm(rp.p);
  return fr.stock && fr.strength >= 1.f;
}

std::vector<float> bakeColourLattice(const Params& rp, const SourceImage& source, int n) {
  n = std::clamp(n, 17, 65);
  const bool filmOnly = latticeFilmOnly(rp);
  Params r0 = rp;
  r0.p.toneStrength = 0.f;
  r0.p.toneTarget.clear();
  r0.toneProxy = nullptr;
  const PointOp op0 = makePointOp(r0, source, nullptr);
  std::vector<float> rgb(filmOnly ? 3 : std::size_t(n) * n * n * 3);
  std::vector<float> node(n);
  for (int i = 0; i < n; ++i) {
    const float u = float(i) / float(n - 1);
    node[i] = kBakeMax * u * u;
  }
  float* table = rgb.data();
  if (!filmOnly)
    parallelFor(n * n, [&](int bg) {
      const int b = bg / n, g = bg % n;
      float* o = table + std::size_t(bg) * n * 3;
      for (int r = 0; r < n; ++r) {
        const float v[3] = {node[r], node[g], node[b]};
        op0.colour(v, o + 3 * r);
      }
    });
  return rgb;
}

TonePre bakeTonePre(const Params& rp, const SourceImage& source, const std::vector<float>* rgb, int n, bool fast) {
  TonePre pre;
  const FilmRun fr = resolveFilm(rp.p);
  const bool filmOnly = fr.stock && fr.strength >= 1.f;
  if (fast && rgb && !filmOnly && rp.toneProxy && !fr.stock && !rp.toneProxy->linear.empty()) {
    Params r0 = rp;
    r0.p.toneStrength = 0.f;
    r0.p.toneTarget.clear();
    r0.toneProxy = nullptr;
    const PointOp op0 = makePointOp(r0, source, nullptr);
    BakedLut t;
    t.n = n;
    t.rgb = borrow(rgb);
    fillToneTable(t.tone, op0);
    pre.analysis = analyseThroughLattice(r0, *rp.toneProxy, t);
    if (rp.p.toneStrength > 0.f) {
      const auto pts = toneTargetCurve(pre.analysis, rp.p.toneTarget, rp.p.toneStrength / 100.f, rp.toneReach);
      if (!pts.empty()) pre.toneMove = sampleMonotoneCurve(pts);
    }
    pre.has = true;
  } else if (rp.p.toneStrength > 0.f) {
    pre.analysis = analyzeTone(rp.toneProxy ? *rp.toneProxy : source, *rp.camera, rp.p);
    const auto pts = toneTargetCurve(pre.analysis, rp.p.toneTarget, rp.p.toneStrength / 100.f, rp.toneReach);
    if (!pts.empty()) pre.toneMove = sampleMonotoneCurve(pts);
    pre.has = true;
  }
  return pre;
}

void bakeToneTable(const Params& rp, const SourceImage& source, const TonePre& pre, BakedLut& out) {
  BakedLut preL;
  preL.analysis = pre.analysis;
  preL.toneMove = pre.toneMove;
  const PointOp op = makePointOp(rp, source, pre.has ? &preL : nullptr);
  out.toneMove.clear();
  if (!op.toneMove.identity) out.toneMove = op.toneMove.table;
  fillToneTable(out.tone, op);
  bakeCounters().toneTables.fetch_add(1, std::memory_order_relaxed);
}

ToneAnalysis bakeAnalysis(const Params& rp, const SourceImage& source, const TonePre& pre) {
  return pre.has ? pre.analysis : analyzeTone(rp.toneProxy ? *rp.toneProxy : source, *rp.camera, rp.p);
}

namespace {

// The tonal analysis of the 256 px tone proxy through a lattice (fast tonal analysis of drag frames, bakeTonePre): the point-wise stages of
// the baked variant (geometry, gain, lattice with the exact-colour overflow, vignette, clamp), no film, no neighbourhood stages, float. The
// histogram is counted in the same pass (per block of rows, integer counts summed: the counts of analyzeToneImage over the same pixels),
// so the proxy image is never stored and the luminance encode runs on all cores. This is the only per-pixel loop of the kernel class;
// frames are rendered by the engine (core/engine).
ToneAnalysis analyseThroughLattice(const Params& rp, const SourceImage& source, const BakedLut& lut) {
  const ImageF& in = source.linear;
  if (in.empty() || lut.n < 2) return analyzeToneImage(ImageF{});
  const Geometry geo = resolveGeometry(rp.p, in.width, in.height, RenderRegion{});
  const int w = geo.outW, h = geo.outH;
  if (w < 1 || h < 1) return analyzeToneImage(ImageF{});
  const float expo = std::exp2(rp.p.exposure + (source.sceneReferred ? rp.camera->baselineExposureEv : 0.f));
  const float gain[3] = {rp.wb[0] * expo, rp.wb[1] * expo, rp.wb[2] * expo};
  const engine::vignette::Block vig = engine::vignette::resolve(rp.p, geometryFacts(geo));
  const bool useGeo = geo.useGeo;
  const LutSampler sample(lut);
  // Tone-mapped colours above the lattice's range take the exact colour path; the operator is only built when such a pixel shows up.
  std::once_flag opOnce;
  std::optional<PointOp> overflowOp;
  constexpr int kRows = 8;
  const int blocks = (h + kRows - 1) / kRows;
  std::vector<ToneCounts> counts(static_cast<std::size_t>(blocks));
  parallelFor(blocks, [&](int k) {
    ToneCounts& c = counts[std::size_t(k)];
    c.fill(0);
    for (int y = k * kRows; y < std::min(h, (k + 1) * kRows); ++y) {
      const float* srcRow = useGeo ? nullptr : in.row(y);
      for (int x = 0; x < w; ++x) {
        float v[3];
        if (!useGeo) {
          v[0] = srcRow[3 * x];
          v[1] = srcRow[3 * x + 1];
          v[2] = srcRow[3 * x + 2];
        } else {
          float sx, sy;
          mapPixel(geo, x, y, sx, sy);
          if (geo.exact) {
            const float* px = in.row(std::clamp(int(std::floor(sy)), 0, in.height - 1)) + 3 * std::clamp(int(std::floor(sx)), 0, in.width - 1);
            v[0] = px[0], v[1] = px[1], v[2] = px[2];
          } else {
            sampleCubic(in, sx, sy, v);
          }
        }
        for (int ch = 0; ch < 3; ++ch) v[ch] = std::max(0.f, v[ch] * gain[ch]);
        float e3[3], tm[3];
        if (!sample(v, e3, tm)) {
          std::call_once(opOnce, [&] { overflowOp.emplace(makePointOp(rp, source, &lut)); });
          overflowOp->colour(tm, e3);
        }
        if (vig.active()) engine::vignette::applyAt(vig, e3, x, y, vig.p3 == 1);
        for (int ch = 0; ch < 3; ++ch) e3[ch] = std::clamp(e3[ch], 0.f, 1.f);
        ++c[std::size_t(toneFineBin(e3))];
      }
    }
  });
  ToneCounts fine{};
  for (const ToneCounts& c : counts)
    for (int i = 0; i < kToneFine; ++i) fine[std::size_t(i)] += c[std::size_t(i)];
  return toneAnalysisFromCounts(fine, std::size_t(w) * std::size_t(h));
}

}  // namespace

std::vector<float> bakeOverflow(const Params& rp, const SourceImage& source, const BakedLut& base, int n, float maxV) {
  if (latticeFilmOnly(rp) || base.latticeFloats() < 4) return {};
  n = std::clamp(n, 9, 65);
  const PointOp op = makePointOp(rp, source, &base);
  std::vector<float> out(std::size_t(n) * n * n * 3);
  std::vector<float> node(n);
  for (int i = 0; i < n; ++i) {
    const float u = float(i) / float(n - 1);
    node[i] = maxV * u * u;
  }
  float* table = out.data();
  parallelFor(n * n, [&](int bg) {
    const int b = bg / n, g = bg % n;
    float* o = table + std::size_t(bg) * n * 3;
    for (int r = 0; r < n; ++r) {
      const float v[3] = {node[r], node[g], node[b]};
      op.colour(v, o + 3 * r);
    }
  });
  return out;
}

// ---- the table nodes: CpuImpl::bake (the HostTable stage; docs/engine/pipeline-architecture.md 10.7 item 4) ----------------------------
// `resolved` is the per-frame TableBakeCtx, `inputs` the payloads of the node's table inputs in edge order (null = edge not wired). The
// payload is a heap object the caller wraps in a shared_ptr with `freePayload`.
namespace {
const TableBakeCtx& tctx(const void* r) { return *static_cast<const TableBakeCtx*>(r); }
template <class T>
void freeAs(void* p) {
  delete static_cast<T*>(p);
}
Status bakeLatticeNode(const void* r, std::span<const void* const>, void** out) {
  const TableBakeCtx& c = tctx(r);
  *out = new std::vector<float>(bakeColourLattice(c.env->point, c.env->src, c.n));
  return Status::Ok;
}
Status bakePreNode(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  // the fast analysis reads the lattice (borrowed by a scratch BakedLut)
  const auto* lattice = in.empty() ? nullptr : static_cast<const std::vector<float>*>(in[0]);
  *out = new TonePre(bakeTonePre(c.env->point, c.env->src, lattice, c.n, c.fast));
  return Status::Ok;
}
Status bakeToneTableNode(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  const auto* pre = in.empty() ? nullptr : static_cast<const TonePre*>(in[0]);
  BakedLut tmp;
  bakeToneTable(c.env->point, c.env->src, pre ? *pre : TonePre{}, tmp);
  *out = new ToneTableOut{std::move(tmp.tone), std::move(tmp.toneMove)};
  return Status::Ok;
}
Status bakeAnalysisNode(const void* r, std::span<const void* const> in, void** out) {
  const TableBakeCtx& c = tctx(r);
  const auto* pre = in.empty() ? nullptr : static_cast<const TonePre*>(in[0]);
  *out = new ToneAnalysis(bakeAnalysis(c.env->point, c.env->src, pre ? *pre : TonePre{}));
  return Status::Ok;
}
Status bakeOverflowNode(const void* r, std::span<const void* const>, void** out) {
  const TableBakeCtx& c = tctx(r);
  BakedLut base;
  base.rgb = std::make_shared<const std::vector<float>>(4);  // only its presence matters: the overflow lattice is a function of the colour stage alone
  *out = new std::vector<float>(bakeOverflow(c.env->point, c.env->src, base, c.n, c.maxV));
  return Status::Ok;
}
const CpuImpl kLattice{nullptr, nullptr, nullptr, bakeLatticeNode, freeAs<std::vector<float>>};
const CpuImpl kPre{nullptr, nullptr, nullptr, bakePreNode, freeAs<TonePre>};
const CpuImpl kToneTable{nullptr, nullptr, nullptr, bakeToneTableNode, freeAs<ToneTableOut>};
const CpuImpl kAnalysis{nullptr, nullptr, nullptr, bakeAnalysisNode, freeAs<ToneAnalysis>};
const CpuImpl kOverflow{nullptr, nullptr, nullptr, bakeOverflowNode, freeAs<std::vector<float>>};
}  // namespace

const CpuImpl& latticeCpu() { return kLattice; }
const CpuImpl& tonePreCpu() { return kPre; }
const CpuImpl& toneTableCpu() { return kToneTable; }
const CpuImpl& analysisCpu() { return kAnalysis; }
const CpuImpl& overflowCpu() { return kOverflow; }

}  // namespace dapple::engine::point
