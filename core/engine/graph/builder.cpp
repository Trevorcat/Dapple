// GraphBuilder: ParamSet + the frame's resolved blocks -> Graph (docs/engine/pipeline-architecture.md 4.5, 4.6, section 50).
// A pure function; nothing is executed here.
//
// The builder spells out the frame's skeleton: source, Resize, the table nodes, the normal branch (Gain -> ToneColour, or ToneColourExact for
// the exact variant), the film branch (its tables and image nodes), DisplayStages + Blend, the 8-bit pack. Every other node is optional and
// placed by its own declaration (NodeDesc::wire / order / wanted): the builder walks the registry and wires a node into its slot when its
// family wants it for this frame (by default: when its resolved block is not the identity). Adding such a node touches nothing here.
#include <algorithm>

#include "engine/graph/graph.h"

namespace dapple::engine {

namespace {

struct Tables {
  NodeId lattice = 0, overflow = 0, pre = 0, tone = 0, analysis = 0;
  bool hasOverflow = false, hasPre = false, hasAnalysis = false;
};

// Table nodes. Wiring decisions (4.4 "conditional dependencies by wiring"):
//  - ToneTable gets the ToneAnalysisPre edge only when the tonal-class move is on (toneStrength > 0), so an exposure drag leaves its key alone;
//  - ToneAnalysisPre exists when the move needs it, or when the analysis of an interactive frame is read through the lattice (fast path).
Tables buildTables(Graph& g, const ParamSet& ps) {
  const EditParams& e = ps.edit();
  const FrameRequest& f = ps.frame();
  const ParamInputs& in = ps.inputs();
  Tables t;
  t.lattice = g.add(NodeType::ColourLattice);
  if (f.wantOverflow) {
    t.overflow = g.add(NodeType::LatticeOverflow, {Edge{t.lattice, false}});
    t.hasOverflow = true;
  }
  const bool move = e.toneStrength > 0.f;
  const bool fastEligible = f.fastAnalysis && !in.filmOnly && in.ctx.hasProxy && !in.filmActive;
  t.hasPre = move || (f.wantAnalysis && fastEligible);
  if (t.hasPre) t.pre = g.add(NodeType::ToneAnalysisPre, {Edge{t.lattice, false}});
  if (move) t.tone = g.add(NodeType::ToneTable, {Edge{t.pre, true}});
  else t.tone = g.add(NodeType::ToneTable);
  if (f.wantAnalysis) {
    t.hasAnalysis = true;
    if (t.hasPre) t.analysis = g.add(NodeType::Analysis, {Edge{t.pre, false}});
    else t.analysis = g.add(NodeType::Analysis);
  }
  return t;
}

// The film branch (8.1): three table nodes (each with its own minimal mask), then the film image nodes.
NodeId addFilm(Graph& g, NodeId cur) {
  const NodeId tRaw = g.add(NodeType::FilmTableRaw);
  const NodeId tCur = g.add(NodeType::FilmTableCurves);
  const NodeId tPrint = g.add(NodeType::FilmTablePrintScan);
  const NodeId fe = g.add(NodeType::FilmExposure, {Edge{cur}, Edge{tRaw}});
  const NodeId fb = g.add(NodeType::FilmBlurExposure, {Edge{fe}});
  const NodeId fq = g.add(NodeType::FilmDirField, {Edge{fb}, Edge{tCur}});
  const NodeId fd = g.add(NodeType::FilmDevelop, {Edge{fb}, Edge{fq}, Edge{tCur}});
  const NodeId fg = g.add(NodeType::FilmGrain, {Edge{fd}});
  const NodeId fpe = g.add(NodeType::FilmPrintExposure, {Edge{fg}, Edge{tPrint}});
  const NodeId feb = g.add(NodeType::FilmEnlargerBlur, {Edge{fpe}});
  const NodeId fp2 = g.add(NodeType::FilmPrintScan, {Edge{feb}, Edge{tPrint}});
  const NodeId fl = g.add(NodeType::FilmGlare, {Edge{fp2}, Edge{tPrint}});
  return g.add(NodeType::FilmScan, {Edge{fl}});
}

// The optional nodes of a wire slot, in their declared order.
std::vector<NodeType> slotNodes(Wire w) {
  const NodeRegistry& reg = NodeRegistry::instance();
  std::vector<NodeType> v;
  for (int t = 0; t < kNodeTypeCount; ++t)
    if (reg.desc(NodeType(t)).wire == w) v.push_back(NodeType(t));
  std::stable_sort(v.begin(), v.end(), [&](NodeType a, NodeType b) { return reg.desc(a).order < reg.desc(b).order; });
  return v;
}

}  // namespace

Graph GraphBuilder::build(const ParamSet& ps, const FrameView& view) {
  static const std::vector<NodeType> kSlots[4] = {{}, slotNodes(Wire::Source), slotNodes(Wire::Scene), slotNodes(Wire::Display)};
  const NodeRegistry& reg = NodeRegistry::instance();
  const BuildCtx ctx{ps, view};
  Graph g;
  auto chain = [&](Wire w, NodeId cur) {
    for (NodeType t : kSlots[int(w)]) {
      const NodeDesc& d = reg.desc(t);
      const void* blk = reg.block(t, view);
      const bool want = d.wanted ? d.wanted(ctx) : (blk && !(d.behaviour && d.behaviour->isIdentity && d.behaviour->isIdentity(blk)));
      if (want) cur = g.add(t, {Edge{cur}});
    }
    return cur;
  };

  const FrameRequest& f = ps.frame();
  const ParamInputs& in = ps.inputs();
  const EditParams& e = ps.edit();
  const NodeId src = g.add(NodeType::Source);
  NodeId cur = chain(Wire::Source, g.add(NodeType::Resize, {Edge{src}}));

  const float strength = e.filmStrength / 100.f;
  const bool useFilm = in.filmActive && strength > 0.f;  // 4.6: strength 0 = main branch only
  const bool needMain = !useFilm || strength < 1.f;
  Tables t;
  if (!f.exact) t = buildTables(g, ps);  // the exact variant evaluates tone and colour per pixel (ToneColourExact), no table nodes
  NodeId mainOut = 0;
  if (needMain) {
    const NodeId scene = chain(Wire::Scene, g.add(NodeType::Gain, {Edge{cur}}));
    if (f.exact) mainOut = g.add(NodeType::ToneColourExact, {Edge{scene}});
    else if (t.hasOverflow) mainOut = g.add(NodeType::ToneColour, {Edge{scene}, Edge{t.tone}, Edge{t.lattice}, Edge{t.overflow}});
    else mainOut = g.add(NodeType::ToneColour, {Edge{scene}, Edge{t.tone}, Edge{t.lattice}});
  }
  if (useFilm) {
    const NodeId disp = g.add(NodeType::DisplayStages, {Edge{addFilm(g, cur)}});
    cur = needMain ? g.add(NodeType::Blend, {Edge{mainOut}, Edge{disp}}) : disp;
  } else {
    cur = mainOut;
  }
  if (f.exact) {
    if (f.wantAnalysis) g.markOutput(g.add(NodeType::Analysis), OutputRole::Analysis);
  } else {
    // the tables of a pruned main branch are demanded as table outputs (no dead nodes; the session skips what nothing reads)
    if (!needMain) {
      g.markOutput(t.lattice, OutputRole::Table);
      g.markOutput(t.tone, OutputRole::Table);
      if (t.hasOverflow) g.markOutput(t.overflow, OutputRole::Table);
    }
    if (t.hasAnalysis) g.markOutput(t.analysis, OutputRole::Analysis);
  }
  cur = g.add(NodeType::Pack8, {Edge{chain(Wire::Display, cur)}});
  g.markOutput(cur, OutputRole::Frame);
  return g;
}

}  // namespace dapple::engine
