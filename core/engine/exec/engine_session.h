#pragma once
// EngineSession: per-photo engine state (docs/engine/pipeline-architecture.md section 3, stage a2/a3).
//
// renderFrame() builds the frame's graph (GraphBuilder, Legacy detail), computes the node keys, evaluates the table
// nodes through the ResultCache (a3) and then runs the LegacyRender super-node, which calls the existing
// Renderer::renderBakedRgba (optionally through a GPU hook first). The C layer keeps everything in front of it
// (parsing, LUT, scaled source, proxy).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dapple/film.h"
#include "dapple/renderer.h"
#include "engine/exec/result_cache.h"
#include "engine/graph/graph.h"

namespace dapple::engine {

// DAPPLE_ENGINE: unset/0 = off (the C layer's own path), "legacy" = routed through EngineSession with the old bakes[3]
// inside it (step a2), anything else = on, table nodes + ResultCache (step a3).
enum class Mode : int { Off = 0, LegacyBakes = 1, Tables = 2 };
Mode mode();
void setModeOverride(int m);  // tests: -1 = back to the environment

// Outputs of the table nodes (cache payloads).
struct ToneTableOut {
  std::vector<float> tone, toneMove;
};

struct TableEnv {
  const Renderer& renderer;
  const SourceImage& src;
};
std::shared_ptr<std::vector<float>> evalColourLattice(const TableEnv& env, int n);
Renderer::TonePre evalToneAnalysisPre(const TableEnv& env, std::vector<float>* lattice, int n, bool fast);
ToneTableOut evalToneTable(const TableEnv& env, const Renderer::TonePre* pre);
std::vector<float> evalLatticeOverflow(const TableEnv& env, int n, float maxV);
ToneAnalysis evalAnalysis(const TableEnv& env, const Renderer::TonePre* pre);

// The keys under which the executor stores the table nodes of a frame. They are the graph's node keys plus the
// lattice that was actually used (`nLattice`: the size the lookup returned): the tonal analysis and the tone-class move
// belong to the bake they were made in (the semantics of the bakes[3] slots this replaces).
struct TableKeys {
  Hash128 lattice, pre, tone, analysis, overflow;
  bool hasPre = false, hasAnalysis = false, hasOverflow = false;
};
TableKeys effectiveTableKeys(const Graph& g, const ParamSet& ps, int nLattice);

struct FrameJob {
  const EditParams& params;
  Renderer& renderer;
  const SourceImage& src;
  const SourceImage* proxy = nullptr;
  const CameraProfile& camera;
  const Lut3D* lut = nullptr;
  FrameRequest request;       // bakeSize = n as the C layer resolved it
  FastOptions fastOptions;
  // Tried first for the pixels (GPU backend); returns null to fall through to the CPU renderer.
  std::function<std::uint8_t*(const BakedLut&, int& w, int& h)> gpuHook;
};

struct FrameResult {
  std::uint8_t* rgba = nullptr;  // malloc()ed, the caller free()s
  int w = 0, h = 0;
  bool hasAnalysis = false;
  ToneAnalysis analysis;
  std::string error;
  // what this frame did (tests, edit-bench)
  enum class Bake : std::uint8_t { FullHit, LatticeReuse, LatticeBake } bake = Bake::FullHit;
};

struct EngineStats {
  std::uint64_t frames = 0, fullHits = 0, latticeReuse = 0, latticeBake = 0, toneTableBuilds = 0, preBuilds = 0,
                analysisBuilds = 0;
  double graphMs = 0;  // accumulated ParamSet + build + validate + keys time
};

class EngineSession {
 public:
  EngineSession();
  bool renderFrame(FrameJob& job, FrameResult& out);
  const EngineStats& stats() const { return stats_; }
  ResultCache& cache() { return cache_; }
  // The graph of the last frame (diagnostics, tests).
  const Graph& lastGraph() const { return graph_; }

 private:
  std::shared_ptr<const BakedLut> bakeLegacy(FrameJob& job, int n, FrameResult& out);
  std::shared_ptr<const BakedLut> bakeTables(FrameJob& job, const ParamSet& ps, int n, FrameResult& out,
                                             Renderer::TonePre* preOut, bool* haveAnalysis);

  std::uint64_t id_;
  ResultCache cache_;
  EngineStats stats_;
  Graph graph_;
  std::vector<std::shared_ptr<const BakedLut>> bakes_;  // Mode::LegacyBakes only
};

// Process-wide summary for dapple_engine_info_json().
std::string infoJson();

}  // namespace dapple::engine
