// GraphBuilder: EditParams + FrameRequest + SessionCtx -> Graph (docs/engine/pipeline-architecture.md 4.5, 4.6).
// A pure function; nothing is executed here.
#include "engine/graph/graph.h"

namespace dapple::engine {

namespace {

bool grainFamily(const std::string& key) { return key == "grain" || key == "film_grain"; }

struct Tables {
  NodeId lattice = 0, overflow = 0, pre = 0, tone = 0, analysis = 0;
  bool hasOverflow = false, hasPre = false, hasAnalysis = false;
};

// Table nodes shared by both detail levels. Wiring decisions (4.4 "conditional dependencies by wiring"):
//  - ToneTable gets the ToneAnalysisPre edge only when the tonal-class move is on (toneStrength > 0), so an exposure
//    drag leaves its key alone;
//  - ToneAnalysisPre exists when the move needs it, or when the analysis of an interactive frame is read through the
//    lattice (fast path).
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

}  // namespace

Graph GraphBuilder::build(const ParamSet& ps, GraphDetail detail, const FramePlan* plan) {
  Graph g;
  const FrameRequest& f = ps.frame();
  const ParamInputs& in = ps.inputs();
  const NodeId src = g.add(NodeType::Source);

  if (detail == GraphDetail::Legacy) {
    if (f.exact) {
      const NodeId r = g.add(NodeType::LegacyRender, {Edge{src}});
      g.markOutput(r, OutputRole::Frame);
      if (f.wantAnalysis) g.markOutput(g.add(NodeType::Analysis), OutputRole::Analysis);
      return g;
    }
    const Tables t = buildTables(g, ps);
    NodeId r;
    if (t.hasOverflow) r = g.add(NodeType::LegacyRender, {Edge{src}, Edge{t.tone}, Edge{t.lattice}, Edge{t.overflow}});
    else r = g.add(NodeType::LegacyRender, {Edge{src}, Edge{t.tone}, Edge{t.lattice}});
    g.markOutput(r, OutputRole::Frame);
    if (t.hasAnalysis) g.markOutput(t.analysis, OutputRole::Analysis);
    return g;
  }

  // ---- Full: every stage of renderBakedImpl as its own node ---------------------------------------------------------------
  FramePlan fallback;
  const FramePlan& fp = plan ? *plan : fallback;
  const EditParams& e = ps.edit();
  NodeId cur = g.add(NodeType::Resize, {Edge{src}});
  if (fp.useGeo) cur = g.add(NodeType::Geometry, {Edge{cur}});

  if (f.exact) {
    cur = g.add(NodeType::ToneColourExact, {Edge{cur}});
    if (f.wantAnalysis) g.markOutput(g.add(NodeType::Analysis), OutputRole::Analysis);
  } else {
    const Tables t = buildTables(g, ps);
    const float strength = e.filmStrength / 100.f;
    NodeId mainOut = 0;
    const bool useFilm = in.filmActive && strength > 0.f;  // 4.6: strength 0 = main branch only
    const bool needMain = !useFilm || strength < 1.f;
    if (needMain) {
      const NodeId gain = g.add(NodeType::Gain, {Edge{cur}});
      if (t.hasOverflow) mainOut = g.add(NodeType::ToneColour, {Edge{gain}, Edge{t.tone}, Edge{t.lattice}, Edge{t.overflow}});
      else mainOut = g.add(NodeType::ToneColour, {Edge{gain}, Edge{t.tone}, Edge{t.lattice}});
    }
    if (useFilm) {
      const NodeId film = g.add(NodeType::FilmSim, {Edge{cur}});
      const NodeId disp = g.add(NodeType::DisplayStages, {Edge{film}});
      cur = needMain ? g.add(NodeType::Blend, {Edge{mainOut}, Edge{disp}}) : disp;
    } else {
      cur = mainOut;
    }
    // unused tables of a pruned main branch still have to be consumed by something to keep the graph free of dead
    // nodes: they are demanded as table outputs (the executor skips them when nothing needs them).
    if (!needMain) {
      g.markOutput(t.lattice, OutputRole::Table);
      g.markOutput(t.tone, OutputRole::Table);
    }
    if (t.hasAnalysis) g.markOutput(t.analysis, OutputRole::Analysis);
    if (t.hasOverflow && !needMain) g.markOutput(t.overflow, OutputRole::Table);
  }

  if (fp.vignette != 0.f) cur = g.add(NodeType::Vignette, {Edge{cur}});
  if (fp.nrLuma > 0.f) cur = g.add(NodeType::NRLuma, {Edge{cur}});
  if (fp.nrColor > 0.f) cur = g.add(NodeType::NRChroma, {Edge{cur}});
  if (fp.dehaze != 0.f) cur = g.add(NodeType::Dehaze, {Edge{cur}});
  if (fp.clarity != 0.f || fp.texture != 0.f || fp.sharpen > 0.f) cur = g.add(NodeType::Detail, {Edge{cur}});
  // 4.6 / section 14 (Q8): grain is dropped at the interactive tier unless a grain-family slider is being dragged
  const bool keepGrain = f.interactive && grainFamily(f.activeParam) && e.grain > 0.f;
  if (fp.grain > 0.f || keepGrain) cur = g.add(NodeType::Grain, {Edge{cur}});
  cur = g.add(NodeType::Pack8, {Edge{cur}});
  g.markOutput(cur, OutputRole::Frame);
  return g;
}

}  // namespace dapple::engine
