// Engine tests, stage (a) of docs/engine/pipeline-architecture.md section 10.2:
//   a1: hashing, registry, graph validation, graph snapshots, key-level declaration conformance
//   a3: table-node output conformance (equal key => bit-identical output), ResultCache, lattice-reuse counts on the
//       edit-bench drag sequences (engine vs the old bakes[3]), planner / graph overhead micro-benchmark
//
//   dapple_engine_tests <id>      ids: hash registry validate snapshots keys conformance cache reuse overhead
//   DAPPLE_UPDATE_SNAPSHOTS=1     rewrites tests/engine/graph_snapshots.txt
//
// The C API is compiled into the executable (like dapple_film_tests): process singletons.
#include "../../core/src/c_api.cpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "edit_params_fields.h"
#include "engine/exec/engine_session.h"
#include "engine/graph/graph.h"

namespace {

using namespace dapple;
using namespace dapple::engine;

int g_failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::shared_ptr<const film::FilmLibrary> g_lib;
bool loadFilm() {
  const fs::path pack = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film" / "dapple.dfp";
  if (!fs::exists(pack)) return false;
  const std::string ps = pack.string();
  if (dapple_film_load_pack(ps.c_str()) != DAPPLE_OK) return false;
  g_lib = film::defaultLibrary();
  return g_lib != nullptr;
}

// ---- synthetic input -------------------------------------------------------------------------------------------------------
std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
SourceImage makeScene(int w, int h, bool scene, std::uint32_t seed = 5) {
  SourceImage s;
  s.sceneReferred = scene;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float* p = s.linear.row(y) + 3 * x;
      const float n = float(hash32(seed * 7919u + std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      p[0] = (0.05f + 0.95f * u * (1.f - 0.4f * v)) * (0.9f + 0.2f * n);
      p[1] = (0.04f + 0.7f * (1.f - u) * v + 0.15f * u) * (0.9f + 0.2f * n);
      p[2] = (0.03f + 0.8f * (1.f - v) * (1.f - 0.5f * u)) * (0.9f + 0.2f * n);
      if ((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f) < 0.01f) p[0] = 2.2f, p[1] = 1.9f, p[2] = 1.5f;
      if (!scene)
        for (int c = 0; c < 3; ++c) p[c] = std::min(p[c], 1.f);
    }
  return s;
}

const CameraProfile& camA() { return cameras().resolve("SONY", "ILCE-7M4"); }
const CameraProfile& camB() { return cameras().resolve("Canon", "EOS R5"); }

std::string writeCube() {
  const fs::path p = fs::temp_directory_path() / "dapple_engine_test.cube";
  std::ofstream f(p);
  f << "TITLE \"t\"\nLUT_3D_SIZE 4\n";
  for (int b = 0; b < 4; ++b)
    for (int g = 0; g < 4; ++g)
      for (int r = 0; r < 4; ++r) {
        const float rr = float(r) / 3.f, gg = float(g) / 3.f, bb = float(b) / 3.f;
        f << rr * 0.9f + 0.05f * gg << " " << gg * 0.95f + 0.04f * bb << " " << bb * 0.85f + 0.1f * rr << "\n";
      }
  return p.string();
}

EditParams edit0() {
  EditParams p;
  p.exposure = 0.3f;
  p.contrast = 10.f;
  p.highlights = -25.f;
  p.shadows = 20.f;
  p.whites = 5.f;
  p.blacks = -5.f;
  p.saturation = 6.f;
  p.vibrance = 10.f;
  return p;
}

ParamInputs inputs(const EditParams& e, bool interactive, bool analysis, bool fast = false) {
  ParamInputs in;
  in.edit = e;
  in.frame.interactive = interactive;
  in.frame.wantAnalysis = analysis;
  in.frame.fastAnalysis = fast;
  in.frame.bakeSize = interactive ? 57 : 65;
  in.frame.longEdge = 1080;
  in.ctx.sourceId = 1;
  in.ctx.sceneReferred = true;
  in.ctx.hasProxy = true;
  in.ctx.cameraId = camA().id;
  in.ctx.filmLib = g_lib.get();
  in.ctx.srcW = 640;
  in.ctx.srcH = 427;
  return in;
}

// ---- hash / registry / validation -----------------------------------------------------------------------------------------
void testHash() {
  Hasher a, b;
  a.f32(0.f);
  b.f32(-0.f);
  CHECK(a.finish() == b.finish());  // -0 -> +0
  Hasher c, d;
  c.f32(std::nanf("1"));
  d.f32(std::nanf("2"));
  CHECK(c.finish() == d.finish());  // one canonical NaN
  Hasher e;
  e.f32(1.f);
  Hasher f;
  f.f32(1.0000001f);
  CHECK(e.finish() != f.finish());
  // lanes are independent and no collisions over a structured set
  std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
  std::set<std::uint64_t> laneA, laneB;
  for (int i = 0; i < 200000; ++i) {
    Hasher h;
    h.i32(i);
    h.f32(float(i) * 0.001f);
    h.str(i % 7 ? "abc" : "abd");
    const Hash128 k = h.finish();
    seen.insert({k.a, k.b});
    laneA.insert(k.a);
    laneB.insert(k.b);
  }
  CHECK(seen.size() == 200000);
  CHECK(laneA.size() == 200000 && laneB.size() == 200000);
  // order and boundaries matter
  Hasher s1, s2;
  s1.str("ab");
  s1.str("c");
  s2.str("a");
  s2.str("bc");
  CHECK(s1.finish() != s2.finish());
  std::printf("  hash ok: 200000 structured keys, no 64-bit lane collision\n");
}

void testRegistry() {
  const ParamRegistry& P = ParamRegistry::instance();
  const NodeRegistry& N = NodeRegistry::instance();
  std::set<std::string> names;
  for (std::size_t k = 0; k < P.size(); ++k) CHECK(names.insert(P.info(ParamKey(k)).name).second);
  CHECK(P.size() >= paramFields().size());
  for (const ParamField& f : paramFields()) CHECK(P.find(f.key) != kNoKey);  // every slider has a key
  // every key is declared by at least one node (otherwise it cannot influence any cache key)
  ParamMask used;
  for (int t = 0; t < kNodeTypeCount; ++t) {
    const NodeDesc& d = N.desc(NodeType(t));
    CHECK(d.name && d.name[0]);
    CHECK(d.type == NodeType(t));
    used.add(d.deps);
    for (std::size_t k = 0; k < kMaxKeys; ++k) CHECK(!d.deps.bits.test(k) || k < P.size());
    if (d.orderKey != kNoKey) used.add(d.orderKey);
  }
  std::string unused;
  for (std::size_t k = 0; k < P.size(); ++k)
    if (!used.bits.test(k) && P.info(ParamKey(k)).name != "ctx.activeParam") unused += P.info(ParamKey(k)).name + " ";
  std::printf("  %zu ParamKeys (%zu sliders), %d node types; keys no node declares: %s\n", P.size(), paramFields().size(),
              kNodeTypeCount, unused.empty() ? "(none)" : unused.c_str());
  CHECK(unused.empty() || unused == "lutPath ");  // lutPath is covered by ctx.lutIdentity (the loaded LUT)
  // ParamSet hashing: equal inputs -> equal hashes, a change moves exactly that key
  const ParamSet a = ParamSet::make(inputs(edit0(), false, false));
  const ParamSet b = ParamSet::make(inputs(edit0(), false, false));
  for (std::size_t k = 0; k < P.size(); ++k) CHECK(a.hash(ParamKey(k)) == b.hash(ParamKey(k)));
  EditParams e = edit0();
  e.clarity = 12.f;
  const ParamSet c = ParamSet::make(inputs(e, false, false));
  int moved = 0;
  for (std::size_t k = 0; k < P.size(); ++k) moved += a.hash(ParamKey(k)) != c.hash(ParamKey(k));
  CHECK(moved == 1);
  CHECK(a.hash(ParamKey(P.mustFind("clarity"))) != c.hash(ParamKey(P.mustFind("clarity"))));
  // the palette point only counts when the palette is used
  EditParams p1 = edit0(), p2 = edit0();
  p2.paletteX = 0.2f;
  CHECK(ParamSet::make(inputs(p1, 0, 0)).hash(P.mustFind("palette")) == ParamSet::make(inputs(p2, 0, 0)).hash(P.mustFind("palette")));
}

void testValidate() {
  const NodeRegistry& N = NodeRegistry::instance();
  {
    Graph g;
    const NodeId s = g.add(NodeType::Source);
    const NodeId r = g.add(NodeType::LegacyRender, {Edge{s}});
    g.markOutput(r, OutputRole::Frame);
    CHECK(g.validate(N).ok);
  }
  {  // dead node
    Graph g;
    const NodeId s = g.add(NodeType::Source);
    g.add(NodeType::ColourLattice);
    const NodeId r = g.add(NodeType::LegacyRender, {Edge{s}});
    g.markOutput(r, OutputRole::Frame);
    const auto v = g.validate(N);
    CHECK(!v.ok && v.error.find("dead node") != std::string::npos);
  }
  {  // wrong input count
    Graph g;
    const NodeId r = g.add(NodeType::LegacyRender);
    g.markOutput(r, OutputRole::Frame);
    CHECK(!g.validate(N).ok);
  }
  {  // domain: a Table where an Image is required
    Graph g;
    const NodeId l = g.add(NodeType::ColourLattice);
    const NodeId r = g.add(NodeType::LegacyRender, {Edge{l}});
    g.markOutput(r, OutputRole::Frame);
    const auto v = g.validate(N);
    CHECK(!v.ok && v.error.find("domain") != std::string::npos);
  }
  {  // cycle / order
    Graph g;
    const NodeId s = g.add(NodeType::Source);
    const NodeId a = g.add(NodeType::Resize, {Edge{s}});
    g.add(NodeType::Resize, {Edge{a + 5}});
    g.markOutput(a, OutputRole::Frame);
    CHECK(!g.validate(N).ok);
  }
  {  // no output
    Graph g;
    g.add(NodeType::Source);
    CHECK(!g.validate(N).ok);
  }
  // structure hash: parameter values do not move it, wiring does
  const ParamSet a = ParamSet::make(inputs(edit0(), true, true, true));
  EditParams e2 = edit0();
  e2.exposure = 1.2f;
  const ParamSet b = ParamSet::make(inputs(e2, true, true, true));
  CHECK(GraphBuilder::build(a).structureHash() == GraphBuilder::build(b).structureHash());
  EditParams e3 = edit0();
  e3.toneStrength = 50.f;
  const ParamSet c = ParamSet::make(inputs(e3, true, true, true));
  CHECK(GraphBuilder::build(a).structureHash() != GraphBuilder::build(c).structureHash());
}

// ---- graph snapshots --------------------------------------------------------------------------------------------------------
struct GScenario {
  std::string name;
  ParamInputs in;
  bool film = false;
};

std::vector<GScenario> graphScenarios() {
  std::vector<GScenario> v;
  auto add = [&](const std::string& n, ParamInputs in, bool film = false) { v.push_back({n, std::move(in), film}); };
  add("default_final", inputs(EditParams{}, false, false));
  add("final_analysis", inputs(edit0(), false, true));
  add("interactive", inputs(edit0(), true, false, true));
  add("interactive_analysis_fast", inputs(edit0(), true, true, true));
  {
    EditParams e = edit0();
    e.toneStrength = 60.f;
    e.toneTarget = "low-long";
    add("tone_move_interactive", inputs(e, true, true, true));
    add("tone_move_final", inputs(e, false, true, false));
  }
  {
    ParamInputs in = inputs(edit0(), false, true);
    in.frame.exact = true;
    add("exact", in);
  }
  {
    ParamInputs in = inputs(edit0(), false, false);
    in.frame.wantOverflow = true;
    add("overflow_gpu", in);
  }
  {
    EditParams e = edit0();
    e.crop = "4:5";
    e.straighten = 3.f;
    e.vignette = -25.f;
    add("geometry_vignette", inputs(e, false, false));
  }
  {
    ParamInputs in = inputs(edit0(), false, false);
    in.frame.region[0] = 0.2f; in.frame.region[1] = 0.1f; in.frame.region[2] = 0.7f; in.frame.region[3] = 0.6f;
    in.frame.regionLongEdge = 640;
    add("region", in);
  }
  {
    EditParams e = edit0();
    e.clarity = 30.f; e.texture = 20.f; e.dehaze = 15.f; e.nrLuma = 25.f; e.nrColor = 30.f; e.sharpen = 60.f; e.grain = 20.f;
    add("detail_all_final", inputs(e, false, false));
    add("detail_all_interactive", inputs(e, true, false, true));
    ParamInputs in = inputs(e, true, false, true);
    in.frame.noise = false;
    add("detail_interactive_no_noise", in);
    ParamInputs g = inputs(e, true, false, true);
    g.frame.activeParam = "grain";
    add("grain_dragged_interactive", g);
  }
  {
    ParamInputs in = inputs(edit0(), false, false);
    in.ctx.sceneReferred = false;
    add("jpeg_source", in);
  }
  for (float strength : {100.f, 50.f, 0.f}) {
    EditParams e = edit0();
    e.filmStock = "kodak_portra_400";
    e.filmStrength = strength;
    e.clarity = 20.f;
    add("film_strength_" + std::to_string(int(strength)), inputs(e, false, true), true);
  }
  return v;
}

// FramePlan for a scenario (what the renderer resolves: spatial after quality / region, geometry, vignette)
FramePlan planFor(const ParamSet& ps) {
  const FrameRequest& f = ps.frame();
  Renderer r(ps.edit(), camA(), nullptr);
  FastOptions fo;
  fo.spatial = f.interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
  fo.noise = f.noise;
  if (f.region[2] > f.region[0]) {
    fo.region.x0 = f.region[0]; fo.region.y0 = f.region[1]; fo.region.x1 = f.region[2]; fo.region.y1 = f.region[3];
    fo.region.outLongEdge = f.regionLongEdge;
  }
  return r.framePlan(ps.ctx().srcW, ps.ctx().srcH, ps.ctx().sceneReferred, fo);
}

std::string snapshotText(bool haveFilm) {
  std::string out;
  for (GScenario& s : graphScenarios()) {
    if (s.film && !haveFilm) continue;
    const ParamSet ps = ParamSet::make(s.in);
    const FramePlan plan = planFor(ps);
    for (GraphDetail d : {GraphDetail::Legacy, GraphDetail::Full}) {
      Graph g = GraphBuilder::build(ps, d, &plan);
      const auto v = g.validate();
      if (!v.ok) {
        std::printf("  scenario %s (%s) invalid: %s\n", s.name.c_str(), d == GraphDetail::Full ? "full" : "legacy", v.error.c_str());
        ++g_failures;
      }
      out += "== " + s.name + (d == GraphDetail::Full ? " [full]\n" : " [legacy]\n") + g.dump(false);
    }
  }
  return out;
}

void testSnapshots() {
  const bool haveFilm = g_lib != nullptr;
  const std::string got = snapshotText(haveFilm);
  const fs::path file = fs::path(DAPPLE_SOURCE_DIR) / "tests" / "engine" / "graph_snapshots.txt";
  if (std::getenv("DAPPLE_UPDATE_SNAPSHOTS")) {
    std::ofstream(file, std::ios::binary) << got;
    std::printf("  snapshots written: %s (%zu bytes)\n", file.string().c_str(), got.size());
    return;
  }
  std::ifstream f(file, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string want = ss.str();
  if (want.empty()) {
    std::printf("  snapshot file missing: %s\n", file.string().c_str());
    ++g_failures;
    return;
  }
  // compare per scenario so that a mismatch names it; scenarios absent on this machine (no film pack) are skipped
  auto split = [](const std::string& t) {
    std::map<std::string, std::string> m;
    std::string cur, name;
    std::istringstream is(t);
    std::string line;
    while (std::getline(is, line)) {
      if (line.rfind("== ", 0) == 0) {
        if (!name.empty()) m[name] = cur;
        name = line.substr(3);
        cur.clear();
      } else {
        cur += line + "\n";
      }
    }
    if (!name.empty()) m[name] = cur;
    return m;
  };
  const auto G = split(got), W = split(want);
  int compared = 0;
  for (const auto& [name, text] : G) {
    const auto it = W.find(name);
    if (it == W.end()) {
      std::printf("  snapshot missing for %s\n", name.c_str());
      ++g_failures;
    } else if (it->second != text) {
      std::printf("  SNAPSHOT MISMATCH %s\n--- want\n%s--- got\n%s", name.c_str(), it->second.c_str(), text.c_str());
      ++g_failures;
    } else {
      ++compared;
    }
  }
  std::printf("  %d graph snapshots match (%zu in file)\n", compared, W.size());
}

// ---- key-level declaration conformance -----------------------------------------------------------------------------------------
// For every node of every scenario graph and every ParamKey that can be varied: the node's content key changes only if
// the key is declared by the node or by one of its keyed ancestors, or the wiring of the node changed; and a key the node
// declares itself does change it.
void collectEffective(const Graph& g, const NodeRegistry& N, NodeId id, ParamMask& m) {
  const NodeInstance& n = g.nodes()[id];
  const NodeDesc& d = N.desc(n.type);
  m.add(d.deps);
  if (d.orderKey != kNoKey) m.add(d.orderKey);
  for (const Edge& e : n.in)
    if (e.keyed) collectEffective(g, N, e.from, m);
}

// shape of the keyed ancestor closure (types and wiring): equal shapes mean a key can only move through parameters
std::string wiring(const Graph& g, const NodeInstance& n) {
  std::string s = std::string("(") + NodeRegistry::name(n.type) + ":";
  for (const Edge& e : n.in) s += (e.keyed ? "+" : "~") + (e.keyed ? wiring(g, g.nodes()[e.from]) : std::string(NodeRegistry::name(g.nodes()[e.from].type))) + ",";
  return s + ")";
}

void testKeys() {
  const ParamRegistry& P = ParamRegistry::instance();
  const NodeRegistry& N = NodeRegistry::instance();
  long checked = 0, declaredChecked = 0;
  for (GScenario& s : graphScenarios()) {
    const ParamSet base = ParamSet::make(s.in);
    const FramePlan plan = planFor(base);
    for (GraphDetail d : {GraphDetail::Legacy, GraphDetail::Full}) {
      Graph g0 = GraphBuilder::build(base, d, &plan);
      g0.computeKeys(base);
      for (std::size_t k = 0; k < P.size(); ++k)
        for (int variant = 0; variant < 2; ++variant) {
          ParamInputs in2 = s.in;
          if (!P.perturb(ParamKey(k), in2, variant)) continue;
          const ParamSet ps2 = ParamSet::make(in2);
          Graph g1 = GraphBuilder::build(ps2, d, &plan);
          g1.computeKeys(ps2);
          if (ps2.hash(ParamKey(k)) == base.hash(ParamKey(k))) continue;  // value did not actually change
          ParamMask moved;  // the key itself and every derived key that moved with it (film flags, quality)
          for (std::size_t q = 0; q < P.size(); ++q)
            if (ps2.hash(ParamKey(q)) != base.hash(ParamKey(q))) moved.add(ParamKey(q));
          for (const NodeInstance& n : g0.nodes()) {
            const NodeInstance* m = g1.find(n.type);
            if (!m) continue;
            ParamMask eff;
            collectEffective(g0, N, n.id, eff);
            const bool same = wiring(g0, n) == wiring(g1, *m);
            const bool changed = n.key != m->key;
            ++checked;
            if (changed && same && (eff.bits & moved.bits).none()) {
              std::printf("  UNDECLARED INFLUENCE %s: %s moved the key of %s\n", s.name.c_str(), P.info(ParamKey(k)).name.c_str(), NodeRegistry::name(n.type));
              ++g_failures;
            }
            const NodeDesc& dn = N.desc(n.type);
            if (same && (dn.deps.has(ParamKey(k)) || dn.orderKey == ParamKey(k)) && !changed) {
              std::printf("  DECLARED BUT INERT %s: %s did not move the key of %s\n", s.name.c_str(), P.info(ParamKey(k)).name.c_str(), NodeRegistry::name(n.type));
              ++g_failures;
            }
            declaredChecked += (dn.deps.has(ParamKey(k)) && same);
          }
        }
    }
  }
  std::printf("  key-level conformance: %ld (node, key, variant) checks, %ld on declared keys\n", checked, declaredChecked);
}

// ---- table-node output conformance ----------------------------------------------------------------------------------------------
std::vector<float> flat(const ToneAnalysis& a) {
  std::vector<float> v = a.histogram;
  v.push_back(a.p5);
  v.push_back(a.p50);
  v.push_back(a.p95);
  v.push_back(a.distance);
  v.push_back(a.margin);
  for (char c : a.key + "|" + a.range + "|" + a.nearest) v.push_back(float(c));
  return v;
}
std::vector<float> flat(const Renderer::TonePre& p) {
  std::vector<float> v = flat(p.analysis);
  v.push_back(p.has ? 1.f : 0.f);
  v.push_back(float(p.toneMove.size()));
  v.insert(v.end(), p.toneMove.begin(), p.toneMove.end());
  return v;
}
std::vector<float> flat(const ToneTableOut& t) {
  std::vector<float> v = t.tone;
  v.push_back(float(t.toneMove.size()));
  v.insert(v.end(), t.toneMove.begin(), t.toneMove.end());
  return v;
}

struct Baseline {
  std::string name;
  ParamInputs in;
  bool useLut = false;
  bool scene = true;
};

struct TableRun {
  std::map<NodeType, std::pair<Hash128, std::vector<float>>> nodes;
};

TableRun runTables(const ParamInputs& in0, const NodeRegistry& reg, const SourceImage& srcScene, const SourceImage& srcJpeg,
                   const Lut3D* lut, const CameraProfile& cam) {
  ParamInputs in = in0;
  in.ctx.lutIdentity = lut ? std::uintptr_t(lut->contentId()) : 0;
  in.ctx.cameraId = cam.id;
  const ParamSet ps = ParamSet::make(in);
  Graph g = GraphBuilder::build(ps, GraphDetail::Legacy);
  g.computeKeys(ps, reg);
  const int n = std::clamp(ps.frame().bakeSize, 17, 65);
  const TableKeys tk = effectiveTableKeys(g, ps, n);
  const SourceImage& src = ps.ctx().sceneReferred ? srcScene : srcJpeg;
  SourceImage proxy;
  proxy.sceneReferred = src.sceneReferred;
  proxy.linear = resizeToLongEdge(src.linear, 256);
  Renderer r(ps.edit(), cam, lut);
  r.setToneReach(ps.ctx().toneReach);
  if (ps.ctx().hasProxy) r.setToneProxy(&proxy);
  const TableEnv env{r, src};
  TableRun out;
  auto lattice = evalColourLattice(env, n);
  out.nodes[NodeType::ColourLattice] = {tk.lattice, *lattice};
  Renderer::TonePre pre;
  const bool move = ps.edit().toneStrength > 0.f;
  if (tk.hasPre) {
    std::vector<float> lat = *lattice;
    pre = evalToneAnalysisPre(env, &lat, n, ps.frame().fastAnalysis);
    out.nodes[NodeType::ToneAnalysisPre] = {tk.pre, flat(pre)};
    CHECK(lat == *lattice);  // the lattice is handed back unchanged
  }
  out.nodes[NodeType::ToneTable] = {tk.tone, flat(evalToneTable(env, (move && tk.hasPre) ? &pre : nullptr))};
  if (tk.hasAnalysis) out.nodes[NodeType::Analysis] = {tk.analysis, flat(evalAnalysis(env, tk.hasPre ? &pre : nullptr))};
  if (tk.hasOverflow) out.nodes[NodeType::LatticeOverflow] = {tk.overflow, evalLatticeOverflow(env, 9, 8.f)};
  return out;
}

struct ConformanceStats {
  long equalKeyChecks = 0, changedKeyChecks = 0, runs = 0, skippedKeys = 0;
  std::vector<std::string> violations;
};

ConformanceStats runConformance(const std::vector<Baseline>& baselines, const NodeRegistry& reg, const Lut3D* lut) {
  const ParamRegistry& P = ParamRegistry::instance();
  const SourceImage scene = makeScene(96, 64, true), jpeg = makeScene(96, 64, false);
  ConformanceStats st;
  for (const Baseline& b : baselines) {
    const Lut3D* bl = b.useLut ? lut : nullptr;
    const TableRun base = runTables(b.in, reg, scene, jpeg, bl, camA());
    ++st.runs;
    auto compare = [&](const TableRun& other, const std::string& what) {
      for (const auto& [type, kv] : base.nodes) {
        const auto it = other.nodes.find(type);
        if (it == other.nodes.end()) continue;
        if (kv.first == it->second.first) {
          ++st.equalKeyChecks;
          const auto& a = kv.second;
          const auto& c = it->second.second;
          if (a.size() != c.size() || std::memcmp(a.data(), c.data(), a.size() * 4) != 0)
            st.violations.push_back(b.name + ": " + what + " changed the output of " + NodeRegistry::name(type) + " but not its key");
        } else {
          ++st.changedKeyChecks;
        }
      }
    };
    for (std::size_t k = 0; k < P.size(); ++k)
      for (int variant = 0; variant < 2; ++variant) {
        ParamInputs in2 = b.in;
        if (!P.perturb(ParamKey(k), in2, variant)) {
          if (variant == 0) ++st.skippedKeys;
          continue;
        }
        // keys whose perturbation needs a loaded stock: skip when the library is absent
        const TableRun other = runTables(in2, reg, scene, jpeg, bl, camA());
        ++st.runs;
        compare(other, P.info(ParamKey(k)).name);
      }
    {  // the camera cannot be varied through the registry: use a second real profile
      const TableRun other = runTables(b.in, reg, scene, jpeg, bl, camB());
      ++st.runs;
      compare(other, "ctx.camera");
    }
    if (bl) {  // and the LUT identity: the same edit without the LUT
      const TableRun other = runTables(b.in, reg, scene, jpeg, nullptr, camA());
      ++st.runs;
      compare(other, "ctx.lutIdentity");
    }
  }
  return st;
}

std::vector<Baseline> conformanceBaselines() {
  std::vector<Baseline> v;
  auto mk = [&](const char* name, EditParams e, bool interactive, bool analysis, bool fast, bool lut = false, bool overflow = false) {
    ParamInputs in = inputs(e, interactive, analysis, fast);
    in.frame.bakeSize = 17;
    in.frame.wantOverflow = overflow;
    v.push_back({name, in, lut});
  };
  mk("plain_final", edit0(), false, true, false);
  {
    EditParams e = edit0();
    e.toneStrength = 55.f;
    e.toneTarget = "low-long";
    e.gradeShadows = {210.f, 25.f, 0.f};
    e.hsl[kBlue].saturation = 20.f;
    e.look = "warm";
    mk("tone_move_fast", e, true, true, true);
    mk("tone_move_exact", e, false, true, false, false, true);
    mk("tone_move_lut", e, true, true, true, true);
  }
  mk("interactive_fast_analysis", edit0(), true, true, true);
  if (g_lib) {
    EditParams e = edit0();
    e.filmStock = "kodak_portra_400";
    e.filmStrength = 100.f;
    mk("film_only", e, false, true, false);
    e.filmStrength = 50.f;
    mk("film_blend", e, true, true, true);
  }
  return v;
}

void testConformance() {
  const NodeRegistry& N = NodeRegistry::instance();
  const std::string cube = writeCube();
  const auto lut = loadLut(cube);
  CHECK(lut != nullptr);
  const auto baselines = conformanceBaselines();
  const auto t0 = Clock::now();
  const ConformanceStats st = runConformance(baselines, N, lut.get());
  // Known deviations (docs/engine/pipeline-architecture.md 10.2, a3 notes). The harness is exact about them: the set of
  // (key, node) pairs must be inside this list, so a new undeclared dependency fails.
  //  - (fixed in a4b) crop / straighten / vignette: the tonal analysis (Renderer::bake -> analyzeTone) renders the proxy WITH
  //    geometry and vignette, and the old pointKey()/colourKey() masked them, so a crop change returned a stale analysis /
  //    tone-class move. The Analysis / ToneAnalysisPre nodes now declare crop, straighten, rotate, flip and vignette and
  //    pointKey() keeps them (colourKey() still masks them: the lattice does not care). They are no longer allowed here.
  //  - ctx.fastAnalysis (interactive lattice analysis vs exact final) is keyed by ToneAnalysisPre / ToneTable / Analysis, so
  //    it is no longer a deviation.
  const std::set<std::pair<std::string, std::string>> known = {};
  std::set<std::pair<std::string, std::string>> seenKnown;
  int knownCount = 0;
  for (const auto& v : st.violations) {
    const auto colon = v.find(": "), chg = v.find(" changed the output of "), but = v.find(" but not its key");
    const std::string key = v.substr(colon + 2, chg - colon - 2), node = v.substr(chg + 23, but - chg - 23);
    if (known.count({key, node})) {
      seenKnown.insert({key, node});
      ++knownCount;
    } else {
      std::printf("  VIOLATION %s\n", v.c_str());
      ++g_failures;
    }
  }
  std::printf("  known legacy deviations observed: %d evaluations over %zu (key, node) pairs\n", knownCount, seenKnown.size());
  std::printf("  table-node conformance: %zu baselines, %ld node evaluations, %ld equal-key checks (outputs bit-identical), %ld key-changed, %ld keys not perturbable alone (%.1f s)\n",
              baselines.size(), st.runs, st.equalKeyChecks, st.changedKeyChecks, st.skippedKeys, msSince(t0) / 1000.0);
  CHECK(st.equalKeyChecks > 1000);

  // negative control: a wrong declaration must be caught
  const ParamRegistry& P = ParamRegistry::instance();
  ParamMask wrong = N.desc(NodeType::ToneTable).deps;
  wrong.bits.reset(P.mustFind("contrast"));
  const NodeRegistry broken = N.withDeps(NodeType::ToneTable, wrong);
  std::vector<Baseline> one{baselines[0]};
  const ConformanceStats bad = runConformance(one, broken, lut.get());
  bool caught = false;
  for (const auto& v : bad.violations) caught |= v.find("contrast") != std::string::npos && v.find("ToneTable") != std::string::npos;
  CHECK(caught);
  std::printf("  negative control: ToneTable declared without 'contrast' -> %zu violation(s), caught=%d\n", bad.violations.size(), int(caught));
  ParamMask wrong2 = N.desc(NodeType::ColourLattice).deps;
  wrong2.bits.reset(P.mustFind("saturation"));
  const NodeRegistry broken2 = N.withDeps(NodeType::ColourLattice, wrong2);
  const ConformanceStats bad2 = runConformance(one, broken2, lut.get());
  bool caught2 = false;
  for (const auto& v : bad2.violations) caught2 |= v.find("saturation") != std::string::npos;
  CHECK(caught2);
  std::printf("  negative control: ColourLattice declared without 'saturation' -> caught=%d\n", int(caught2));
}

// ---- ResultCache ---------------------------------------------------------------------------------------------------------------
void testCache() {
  ResultCache c;
  c.setLimit(NodeType::ToneTable, 2);
  auto put = [&](NodeType t, std::uint64_t key, std::uint64_t fam, int order) {
    ResultCache::Entry e;
    e.key = {key, 1};
    e.family = {fam, 2};
    e.order = order;
    e.payload = std::make_shared<int>(int(key));
    e.bytes = 4;
    c.put(t, std::move(e));
  };
  put(NodeType::ToneTable, 1, 1, 0);
  put(NodeType::ToneTable, 2, 2, 0);
  put(NodeType::ToneTable, 3, 3, 0);  // evicts 1 (FIFO)
  CHECK(!c.find(NodeType::ToneTable, {1, 1}));
  CHECK(c.find(NodeType::ToneTable, {2, 1}));
  CHECK(c.find(NodeType::ToneTable, {3, 1}));
  put(NodeType::ColourLattice, 10, 7, 41);
  put(NodeType::ColourLattice, 11, 7, 57);
  put(NodeType::ColourLattice, 12, 8, 65);
  const Hash128 fam7{7, 2};
  auto* e = c.findBest(NodeType::ColourLattice, fam7, 41);
  CHECK(e && e->order == 57);                                      // the finest at least as fine as asked
  CHECK(c.findBest(NodeType::ColourLattice, fam7, 49)->order == 57);
  CHECK(!c.findBest(NodeType::ColourLattice, fam7, 58));           // none fine enough
  CHECK(!c.findBest(NodeType::ColourLattice, {9, 2}, 17));         // other family
  c.eraseIf(NodeType::ColourLattice, [](const ResultCache::Entry& x) { return x.family.a == 7; });
  CHECK(c.entries(NodeType::ColourLattice).size() == 1);
  CHECK(c.stats(NodeType::ToneTable).evictions == 1);
  std::printf("  cache ok\n");
}

// ---- lattice reuse on the edit-bench drag sequences (gate of step a3) --------------------------------------------------------------------
struct SeqFrame {
  std::string label;
  std::string json;
  int flags;
  int longEdge;
};

// the frame() lists of cli/edit_bench.cpp (two scenarios, one session each, warm-up + repeat calls per list)
std::vector<SeqFrame> benchSequence() {
  std::vector<SeqFrame> out;
  EditParams autoP = edit0();
  autoP.toneStrength = 60.f;
  EditParams heavy = autoP;
  heavy.clarity = 40; heavy.texture = 30; heavy.nrLuma = 30; heavy.sharpen = 80; heavy.dehaze = 25; heavy.vibrance = 20;
  heavy.hsl[kOrange].saturation = 15;
  heavy.gradeShadows = {220, 30, 0};
  heavy.gradeHighlights = {40, 25, 0};
  const int repeat = 8;
  for (const EditParams& base : {autoP, heavy}) {
    auto list = [&](const char* name, auto&& vary, int flags, int le) {
      for (int k = 0; k <= repeat; ++k) {
        EditParams q = base;
        vary(q, k + 1);
        out.push_back({std::string(name) + "/" + std::to_string(k), toJson(q), flags | DAPPLE_FRAME_ANALYSIS, le});
      }
    };
    auto expo = [](EditParams& q, int k) { q.exposure += 0.002f * float(k % 100); };
    auto sat = [](EditParams& q, int k) { q.saturation += 0.1f * float(k % 100); };
    auto clar = [](EditParams& q, int k) { q.clarity = float(k % 60); };
    auto none = [](EditParams&, int) {};
    list("i1080_exposure", expo, DAPPLE_FRAME_INTERACTIVE, 1080);
    list("i1080_saturation", sat, DAPPLE_FRAME_INTERACTIVE, 1080);
    list("i1080_clarity", clar, DAPPLE_FRAME_INTERACTIVE, 1080);
    list("i1600_exposure", expo, DAPPLE_FRAME_INTERACTIVE, 1600);
    list("final1600_unchanged", none, 0, 1600);
    list("final1600_exposure", expo, 0, 1600);
    list("exact1600", none, DAPPLE_FRAME_EXACT, 1600);
  }
  return out;
}

struct SeqResult {
  std::string kinds;  // B / R / H per frame
  std::vector<std::string> hashes;
  std::vector<double> ms;
};

SeqResult runSequence(int mode, const std::vector<SeqFrame>& seq) {
  setModeOverride(mode);
  auto* s = new DappleSession;
  s->source = makeScene(1600, 1067, true, 3);
  s->camera = &camA();
  SeqResult r;
  for (const SeqFrame& f : seq) {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = f.longEdge;
    rq.flags = f.flags;
    DappleImage im{};
    char* an = nullptr;
    const auto ev0 = bakeCounters().latticeEvaluated.load(), re0 = bakeCounters().latticeReused.load();
    const auto t0 = Clock::now();
    const int rc = dapple_session_render_frame(s, f.json.c_str(), nullptr, &rq, &im, &an);
    r.ms.push_back(msSince(t0));
    CHECK(rc == DAPPLE_OK);
    r.kinds += bakeCounters().latticeEvaluated.load() > ev0 ? 'B' : (bakeCounters().latticeReused.load() > re0 ? 'R' : 'H');
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < std::size_t(im.width) * im.height * 4; ++i) h = (h ^ im.rgba[i]) * 1099511628211ull;
    if (an)
      for (const char* c = an; *c; ++c) h = (h ^ std::uint8_t(*c)) * 1099511628211ull;
    r.hashes.push_back(std::to_string(h));
    dapple_free_image(&im);
    dapple_free_string(an);
  }
  delete s;
  setModeOverride(-1);
  return r;
}

void testReuse() {
  const auto seq = benchSequence();
  const SeqResult off = runSequence(0, seq), tables = runSequence(1, seq);  // 0 = the old Renderer path (oracle), 1 = the engine
  auto count = [](const std::string& k, char c) { return int(std::count(k.begin(), k.end(), c)); };
  std::printf("  edit-bench drag sequences (%zu frames, two scenarios): old bakes[3]: bake %d, lattice-reuse %d, full-hit %d | engine: bake %d, reuse %d, hit %d\n",
              seq.size(), count(off.kinds, 'B'), count(off.kinds, 'R'), count(off.kinds, 'H'), count(tables.kinds, 'B'),
              count(tables.kinds, 'R'), count(tables.kinds, 'H'));
  CHECK(off.kinds.size() == tables.kinds.size());
  CHECK(off.kinds == tables.kinds);  // identical reuse decisions frame by frame
  CHECK(off.hashes == tables.hashes);  // and identical pixels + analysis
  {  // per frame-type medians (ms), 1600x1067 synthetic source; informational, the gate-P numbers come from edit-bench on a RAW
    std::map<std::string, std::vector<double>> g[2];
    const SeqResult* rs[2] = {&off, &tables};
    for (int m = 0; m < 2; ++m)
      for (std::size_t i = 0; i < seq.size(); ++i)
        if (i % 9 != 0) g[m][seq[i].label.substr(0, seq[i].label.find('/'))].push_back(rs[m]->ms[i]);  // skip each list's warm-up frame
    auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    for (const auto& [name, v] : g[0])
      std::printf("    %-22s off %7.2f  engine %7.2f ms\n", name.c_str(), med(v), med(g[1][name]));
  }
  if (off.kinds != tables.kinds) {
    std::printf("  old   %s\n  new   %s\n", off.kinds.c_str(), tables.kinds.c_str());
  }
}

// ---- planner / graph overhead --------------------------------------------------------------------------------------------------------
void testOverhead() {
  EditParams e = edit0();
  e.toneStrength = 60.f;
  e.clarity = 40; e.texture = 30; e.nrLuma = 30; e.sharpen = 80; e.dehaze = 25;
  e.hsl[kOrange].saturation = 15;
  e.gradeShadows = {220, 30, 0};
  e.toneCurve = {{0, 0}, {64, 52}, {192, 208}, {255, 255}};
  ParamInputs in = inputs(e, true, true, true);
  in.ctx.cameraId = camA().id;
  // a populated cache, so the lookups are real
  ResultCache cache;
  for (int i = 0; i < 3; ++i) {
    ResultCache::Entry le;
    le.key = {std::uint64_t(i + 1), 9};
    le.family = {std::uint64_t(i + 1), 8};
    le.order = 57;
    cache.put(NodeType::ColourLattice, le);
    cache.put(NodeType::LegacyRender, le);
  }
  volatile std::uint64_t sink = 0;
  auto frame = [&](GraphDetail detail) {
    const ParamSet ps = ParamSet::make(in);
    Graph g = GraphBuilder::build(ps, detail);
    const ValidationResult v = g.validate();
    g.computeKeys(ps);
    const TableKeys tk = effectiveTableKeys(g, ps, 57);
    const NodeInstance* lat = g.find(NodeType::ColourLattice);
    cache.findBest(NodeType::ColourLattice, lat->family, 57);
    cache.find(NodeType::LegacyRender, tk.tone);
    cache.find(NodeType::Analysis, tk.analysis);
    sink = sink + (v.ok ? 1 : 0) + tk.tone.a;
  };
  auto measure = [&](GraphDetail d) {
    double best = 1e9;
    for (int round = 0; round < 7; ++round) {
      const int N = 4000;
      const auto t0 = Clock::now();
      for (int i = 0; i < N; ++i) frame(d);
      best = std::min(best, msSince(t0) * 1000.0 / N);
    }
    return best;
  };
  const double legacyUs = measure(GraphDetail::Legacy);
  const double fullUs = measure(GraphDetail::Full);
  // the old keys
  SourceImage proxy = makeScene(64, 43, true);
  Renderer r(e, camA(), nullptr);
  r.setToneProxy(&proxy);
  double oldUs = 1e9;
  for (int round = 0; round < 7; ++round) {
    const int N = 4000;
    const auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) sink = sink + r.pointKey().size() + r.colourKey().size();
    oldUs = std::min(oldUs, msSince(t0) * 1000.0 / N);
  }
  // stage costs
  auto stage = [&](auto&& fn) {
    double best = 1e9;
    for (int round = 0; round < 5; ++round) {
      const int N = 4000;
      const auto t0 = Clock::now();
      for (int i = 0; i < N; ++i) fn();
      best = std::min(best, msSince(t0) * 1000.0 / N);
    }
    return best;
  };
  const double makeUs = stage([&] { sink = sink + ParamSet::make(in).hash(0).a; });
  const ParamSet ps0 = ParamSet::make(in);
  const double buildUs = stage([&] { sink = sink + GraphBuilder::build(ps0, GraphDetail::Legacy).nodes().size(); });
  Graph g0 = GraphBuilder::build(ps0, GraphDetail::Legacy);
  const double valUs = stage([&] { sink = sink + g0.validate().ok; });
  const double keyUs = stage([&] { g0.computeKeys(ps0); sink = sink + g0.nodes().back().key.a; });
  const double budgetUs = 200.0 * 0.14;  // 0.2 ms on the low-end profile, host runs ~1/0.14 faster (risk 8)
  std::printf("  per-frame engine overhead (host, best of 7x4000): legacy graph %.2f us, full graph %.2f us; old pointKey+colourKey %.2f us\n"
              "    legacy-graph stages: ParamSet::make %.2f, build %.2f, validate %.2f, keys %.2f us\n"
              "    budget: 200 us on the low-end profile x 0.14 = %.0f us on this host\n",
              legacyUs, fullUs, oldUs, makeUs, buildUs, valUs, keyUs, budgetUs);
  CHECK(legacyUs < budgetUs);
  CHECK(fullUs < budgetUs);
}

}  // namespace

int main(int argc, char** argv) {
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"hash", testHash},           {"registry", testRegistry},     {"validate", testValidate},
                     {"snapshots", testSnapshots}, {"keys", testKeys},             {"conformance", testConformance},
                     {"cache", testCache},         {"reuse", testReuse},           {"overhead", testOverhead}};
  if (argc >= 2 && std::string(argv[1]) == "--list") {
    for (const T& t : tests) std::printf("%s\n", t.id);
    return 0;
  }
  loadFilm();
  bool ran = false;
  for (const T& t : tests) {
    if (argc >= 2 && std::string(argv[1]) != t.id) continue;
    ran = true;
    std::printf("[%s]\n", t.id);
    const int before = g_failures;
    t.fn();
    std::printf("[%s] %s\n", t.id, g_failures == before ? "ok" : "FAILED");
  }
  if (!ran) {
    std::fprintf(stderr, "unknown test id\n");
    return 2;
  }
  return g_failures ? 1 : 0;
}
