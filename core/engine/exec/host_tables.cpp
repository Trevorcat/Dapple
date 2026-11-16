// The HostTable stage of a frame (docs/engine/pipeline-architecture.md 5.7): the table nodes of the frame's graph evaluated through their
// CpuImpl::bake, cached in the session's ResultCache under their effective keys, assembled into the frame's BakedLut (kBakeSlot). Also the
// Resize node's bake (the session source at a smaller long edge).
#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "engine/exec/engine_session_internal.h"
#include "engine/nodes/resize/resize.h"

namespace dapple::engine {

using session::combine;
using session::withOrder;

namespace {
template <class T>
std::shared_ptr<T> as(const std::shared_ptr<void>& p) {
  return std::static_pointer_cast<T>(p);
}
}  // namespace

int interactiveLatticeN() {
  // DAPPLE_INTERACTIVE_LATTICE=<n> overrides the size (0 = the requested size is baked as asked: the measurement tool compares sizes with it)
  static const int v = [] {
    if (const char* e = std::getenv("DAPPLE_INTERACTIVE_LATTICE")) return std::atoi(e);
    return kInteractiveLatticeN;
  }();
  return v;
}

std::shared_ptr<void> bakeNode(NodeType type, const void* resolved, std::span<const void* const> inputs) {
  const CpuImpl* impl = NodeRegistry::instance().desc(type).cpu;
  if (!impl || !impl->bake) return nullptr;
  void* p = nullptr;
  if (impl->bake(resolved, inputs, &p) != Status::Ok || !p) return nullptr;
  void (*fr)(void*) = impl->freePayload;
  return std::shared_ptr<void>(p, [fr](void* q) {
    if (fr) fr(q);
  });
}

std::shared_ptr<std::vector<float>> evalColourLattice(const TableEnv& env, int n) {
  const TableBakeCtx c{&env, n, false, 0.f};
  return as<std::vector<float>>(bakeNode(NodeType::ColourLattice, &c));
}

engine::point::TonePre evalToneAnalysisPre(const TableEnv& env, const std::vector<float>* lattice, int n, bool fast) {
  const TableBakeCtx c{&env, n, fast, 0.f};
  const void* in[1] = {lattice};
  const auto p = as<engine::point::TonePre>(bakeNode(NodeType::ToneAnalysisPre, &c, in));
  return p ? *p : engine::point::TonePre{};
}

ToneTableOut evalToneTable(const TableEnv& env, const engine::point::TonePre* pre) {
  const TableBakeCtx c{&env, 0, false, 0.f};
  const void* in[1] = {pre};
  const auto p = as<ToneTableOut>(bakeNode(NodeType::ToneTable, &c, in));
  return p ? *p : ToneTableOut{};
}

std::vector<float> evalLatticeOverflow(const TableEnv& env, int n, float maxV) {
  const TableBakeCtx c{&env, n, false, maxV};
  const auto p = as<std::vector<float>>(bakeNode(NodeType::LatticeOverflow, &c));
  return p ? *p : std::vector<float>{};
}

ToneAnalysis evalAnalysis(const TableEnv& env, const engine::point::TonePre* pre) {
  const TableBakeCtx c{&env, 0, false, 0.f};
  const void* in[1] = {pre};
  const auto p = as<ToneAnalysis>(bakeNode(NodeType::Analysis, &c, in));
  return p ? *p : ToneAnalysis{};
}

TableKeys effectiveTableKeys(const Graph& g, const ParamSet& ps, int nLattice) {
  TableKeys k;
  const NodeInstance* latN = g.find(NodeType::ColourLattice);
  const NodeInstance* ttN = g.find(NodeType::ToneTable);
  const NodeInstance* preN = g.find(NodeType::ToneAnalysisPre);
  const NodeInstance* anN = g.find(NodeType::Analysis);
  const NodeInstance* ovN = g.find(NodeType::LatticeOverflow);
  if (latN) k.lattice = withOrder(latN->family, nLattice);
  if (preN) {
    k.hasPre = true;
    k.pre = combine(preN->key, k.lattice);
  }
  k.tone = ttN ? ttN->key : Hash128{};
  if (ttN && ps.edit().toneStrength > 0.f && preN) k.tone = combine(ttN->key, k.pre);
  if (anN) {
    k.hasAnalysis = true;
    k.analysis = combine(anN->key, combine(k.tone, k.lattice));
  }
  if (ovN) {
    k.hasOverflow = true;
    k.overflow = ovN->key;
  }
  return k;
}

std::shared_ptr<const BakedLut> EngineSession::runHostTables(FrameJob& job, const ParamSet& ps, int n, FrameResult& out) {
  const Graph& g = graph_;
  const NodeInstance* latN = g.find(NodeType::ColourLattice);
  const NodeInstance* preN = g.find(NodeType::ToneAnalysisPre);
  const NodeInstance* anN = g.find(NodeType::Analysis);
  const NodeInstance* ovN = g.find(NodeType::LatticeOverflow);
  if (!latN) return out.error = "graph without a colour lattice", nullptr;
  int nReq = std::clamp(n, 17, 65);
  // an interactive frame that has to bake the colour lattice bakes the smaller one (section 57); a cached lattice at least that fine (a settled 65, a
  // 57 of an earlier session) is still found and reused, and a non-interactive frame asks for what it asked for
  if (const int cap = interactiveLatticeN(); job.request.interactive && cap > 0 && nReq > cap) nReq = cap;
  const TableEnv env{job.point, job.src};
  const bool move = job.params.toneStrength > 0.f;
  using session::Clock;
  auto put = [&](CacheSlot slot, const Hash128& key, const Hash128& family, int order, std::shared_ptr<void> payload, std::size_t bytes, Clock::time_point t0,
                 Hash128 ref = {}) {
    ResultCache::Entry e;
    e.slot = slot;
    e.key = key;
    e.family = family;
    e.order = order;
    e.payload = std::move(payload);
    e.hostBytes = bytes;
    e.costMs = session::msSince(t0);
    e.ref = ref;
    cache_.put(std::move(e));
  };

  // the finest cached lattice of this colour stage that is at least as fine as asked for
  std::shared_ptr<std::vector<float>> lattice;
  int latticeN = 0;
  Hash128 latticeKey;
  if (const ResultCache::Hit h = cache_.findBest(slotOf(NodeType::ColourLattice), latN->family, nReq)) {
    lattice = h.as<std::vector<float>>();
    latticeN = h.order;
    latticeKey = h.key;
  }

  TableKeys tk;  // keys under the lattice actually used (see effectiveTableKeys)
  auto deriveKeys = [&] { tk = effectiveTableKeys(g, ps, latticeN); };
  if (lattice) deriveKeys();

  engine::point::TonePre pre;
  bool preReady = false;
  auto ensurePre = [&]() -> engine::point::TonePre& {
    if (preReady) return pre;
    if (const ResultCache::Hit h = cache_.find(slotOf(NodeType::ToneAnalysisPre), tk.pre)) {
      pre = *h.as<engine::point::TonePre>();
    } else {
      const auto t0 = Clock::now();
      const TableBakeCtx bc{&env, latticeN, job.request.fastAnalysis, 0.f};
      const void* in[1] = {lattice.get()};
      const auto made = as<engine::point::TonePre>(bakeNode(NodeType::ToneAnalysisPre, &bc, in));
      pre = made ? *made : engine::point::TonePre{};
      out.cpu.preMs += session::msSince(t0);
      put(slotOf(NodeType::ToneAnalysisPre), tk.pre, tk.pre, 0, std::make_shared<engine::point::TonePre>(pre),
          sizeof(engine::point::TonePre) + pre.toneMove.size() * 4, t0);
      ++stats_.preBuilds;
    }
    preReady = true;
    return pre;
  };

  std::shared_ptr<const BakedLut> bl;
  if (lattice) {
    if (const ResultCache::Hit h = cache_.find(kBakeSlot, combine(tk.tone, tk.lattice))) {
      bl = h.as<BakedLut>();
      out.bake = FrameResult::Bake::FullHit;
    }
  }
  if (!bl) {
    if (lattice) {
      out.bake = FrameResult::Bake::LatticeReuse;
      bakeCounters().latticeReused.fetch_add(1, std::memory_order_relaxed);
    } else {
      out.bake = FrameResult::Bake::LatticeBake;
      bakeCounters().latticeEvaluated.fetch_add(1, std::memory_order_relaxed);
      const auto t0 = Clock::now();
      const TableBakeCtx bc{&env, nReq, false, 0.f};
      lattice = as<std::vector<float>>(bakeNode(NodeType::ColourLattice, &bc));
      if (!lattice) {
        out.error = "colour lattice";
        return nullptr;
      }
      out.cpu.latticeMs = session::msSince(t0);
      latticeN = nReq;
      latticeKey = withOrder(latN->family, nReq);
      put(slotOf(NodeType::ColourLattice), latticeKey, latN->family, nReq, lattice, lattice->size() * sizeof(float), t0);
      deriveKeys();
    }
    ToneTableOut tt;
    if (const ResultCache::Hit h = cache_.find(slotOf(NodeType::ToneTable), tk.tone)) {
      tt = *h.as<ToneTableOut>();
    } else {
      const auto t0 = Clock::now();
      const engine::point::TonePre* p = (move && preN) ? &ensurePre() : nullptr;
      const TableBakeCtx bc{&env, latticeN, false, 0.f};
      const void* in[1] = {p};
      const auto made = as<ToneTableOut>(bakeNode(NodeType::ToneTable, &bc, in));
      tt = made ? *made : ToneTableOut{};
      out.cpu.toneMs = session::msSince(t0) - out.cpu.preMs;
      put(slotOf(NodeType::ToneTable), tk.tone, tk.tone, 0, std::make_shared<ToneTableOut>(tt), (tt.tone.size() + tt.toneMove.size()) * 4, t0);
      ++stats_.toneTableBuilds;
    }
    const auto t0 = Clock::now();
    auto nb = std::make_shared<BakedLut>();
    nb->n = latticeN;
    nb->rgb = lattice;  // shared with the ColourLattice entry (that entry accounts its bytes)
    nb->tone = std::move(tt.tone);
    nb->toneMove = std::move(tt.toneMove);
    const Hash128 ak = combine(tk.tone, tk.lattice);
    nb->key = ak.hex() + "#" + std::to_string(latticeN);
    nb->colourKey = latN->family.hex();
    nb->latticeKey = latticeKey.hex();
    put(kBakeSlot, ak, tk.tone, latticeN, nb, nb->tone.size() * 4 + nb->toneMove.size() * 4 + sizeof(BakedLut), t0, latticeKey);
    // a lattice lives exactly as long as a retained bake refers to it (one pass over the entries with a set of the bakes' lattices: the nested scan of
    // every lattice against every bake grew with the session)
    std::unordered_set<Hash128, Hash128Hash> live;
    for (const Hash128& r : cache_.refs(kBakeSlot)) live.insert(r);
    cache_.eraseIf([&](const ResultCache::Entry& le) { return le.slot == slotOf(NodeType::ColourLattice) && !live.count(le.key); });
    bl = nb;
    out.cpu.assembleMs = session::msSince(t0);
  }

  if (ovN && !cache_.contains(slotOf(NodeType::LatticeOverflow), tk.overflow)) {  // overflow lattice for a GPU executor: cached by its own key
    const auto t0 = Clock::now();
    const TableBakeCtx bc{&env, 33, false, 8.f};
    auto v = as<std::vector<float>>(bakeNode(NodeType::LatticeOverflow, &bc));
    if (!v) v = std::make_shared<std::vector<float>>();
    out.cpu.overflowMs = session::msSince(t0);
    put(slotOf(NodeType::LatticeOverflow), tk.overflow, tk.overflow, 0, v, v->size() * 4, t0);
  }

  if (anN) {
    if (const ResultCache::Hit h = cache_.find(slotOf(NodeType::Analysis), tk.analysis)) {
      out.analysis = *h.as<ToneAnalysis>();
    } else {
      const auto t0 = Clock::now();
      const engine::point::TonePre* p = preN ? &ensurePre() : nullptr;
      const TableBakeCtx bc{&env, latticeN, false, 0.f};
      const void* in[1] = {p};
      const auto made = as<ToneAnalysis>(bakeNode(NodeType::Analysis, &bc, in));
      out.analysis = made ? *made : ToneAnalysis{};
      out.cpu.analysisMs = session::msSince(t0);
      put(slotOf(NodeType::Analysis), tk.analysis, tk.analysis, 0, std::make_shared<ToneAnalysis>(out.analysis), out.analysis.histogram.size() * 4 + 128, t0);
      ++stats_.analysisBuilds;
    }
    out.hasAnalysis = true;
  }
  if (bl) out.bakeN = bl->n;
  return bl;
}

// LatticeOverflow payload of the frame (cached by runHostTables when the request asked for it), or null.
std::shared_ptr<const std::vector<float>> EngineSession::overflowFor(const ParamSet& ps, int n) {
  const TableKeys tk = effectiveTableKeys(graph_, ps, n);
  if (!tk.hasOverflow) return nullptr;
  return cache_.find(slotOf(NodeType::LatticeOverflow), tk.overflow).as<const std::vector<float>>();
}

// ---- the Resize node (CpuImpl::bake): the session source at a smaller long edge, cached ------------------------------------------------
std::shared_ptr<const SourceImage> EngineSession::resizedSource(const SourceImage& full, int longEdge, bool cacheLarge) {
  const int own = std::max(full.linear.width, full.linear.height);
  if (longEdge <= 0 || longEdge >= own) return nullptr;
  Hasher h;
  h.u64(id_);
  h.i32(longEdge);
  h.i32(full.linear.width);
  h.i32(full.linear.height);
  const Hash128 key = h.finish();
  std::lock_guard<std::mutex> lock(resizeMtx_);
  if (const ResultCache::Hit hit = cache_.find(slotOf(NodeType::Resize), key)) return hit.as<const SourceImage>();  // a hit is the most recently used
  const auto t0 = session::Clock::now();
  const resize::BakeCtx ctx{&full, longEdge};
  std::shared_ptr<void> made = bakeNode(NodeType::Resize, &ctx);
  if (!made) return nullptr;
  auto img = as<const SourceImage>(made);
  const std::size_t bytes = img->linear.data.size() * sizeof(float);
  if (!cacheLarge && bytes > (64u << 20)) return img;
  ResultCache::Entry ne;
  ne.slot = slotOf(NodeType::Resize);
  ne.key = ne.family = key;
  ne.order = longEdge;
  ne.payload = made;
  ne.hostBytes = bytes;
  ne.costMs = session::msSince(t0);
  cache_.put(std::move(ne));
  return img;
}

}  // namespace dapple::engine
