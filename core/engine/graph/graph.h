#pragma once
// Node descriptors, graph, validation, builder (docs/engine/pipeline-architecture.md sections 4.2-4.7). Stage a1.
//
// The graph is a pure description: building it executes nothing. Node content keys are
//   key = H(type, version, params.hash(desc.deps), ctx order key, keys of the keyed inputs)
// so changing a slider changes exactly the keys of the nodes that declare (or inherit) it.
#include <string>
#include <vector>

#include "dapple/frame_plan.h"
#include "engine/graph/params.h"
#include "engine/graph/types.h"

namespace dapple::engine {

enum class CacheHint : std::uint8_t { Never, KeepIfCheap, Keep };

struct NodeDesc {
  NodeType type = NodeType::Source;
  const char* name = "";
  std::uint16_t version = 1;
  Domain output = Domain::Image;
  Footprint footprint = Footprint::Point;
  int minInputs = 0, maxInputs = 0;
  Domain inputDomain[4] = {Domain::Image, Domain::Image, Domain::Image, Domain::Image};
  ParamMask deps;                  // ParamKeys + context keys this node reads itself (inputs add their own)
  ParamKey orderKey = kNoKey;      // a context key that selects among interchangeable results (lattice size): part of
                                   // the node key but not of its family (see ResultCache)
  bool usesGeometry = false;       // output depends on frame geometry (spatial outputs)
  bool cpuImpl = true;             // (the GPU implementations are asked of gpuImplFor(NodeType), nodes/gpu_nodes.h)
  CacheHint cache = CacheHint::Never;
  const char* haloNote = "";       // human readable: the neighbourhood this node reads (declared, executed whole-frame in a6)
};

class NodeRegistry {
 public:
  static const NodeRegistry& instance();
  const NodeDesc& desc(NodeType t) const { return descs_[std::size_t(t)]; }
  static const char* name(NodeType t) { return instance().desc(t).name; }
  // A copy with one node's declared dependencies replaced (conformance tests: proves the harness catches a wrong mask).
  NodeRegistry withDeps(NodeType t, const ParamMask& deps) const {
    NodeRegistry r(*this);
    r.descs_[std::size_t(t)].deps = deps;
    return r;
  }

 private:
  NodeRegistry();
  std::vector<NodeDesc> descs_;
};

struct Edge {
  NodeId from = 0;
  bool keyed = true;  // false: scheduling edge only; the producer's key does not enter the consumer's key
};

struct NodeInstance {
  NodeType type = NodeType::Source;
  NodeId id = 0;
  std::vector<Edge> in;
  Hash128 key;
  Hash128 family;     // key without the order key (lattice size)
};

struct ValidationResult {
  bool ok = true;
  std::string error;
};

class Graph {
 public:
  NodeId add(NodeType t, std::initializer_list<Edge> inputs = {});
  void markOutput(NodeId id, OutputRole role) { outputs_.push_back({id, role}); }
  const std::vector<NodeInstance>& nodes() const { return nodes_; }
  const NodeInstance* find(NodeType t) const;
  const std::vector<std::pair<NodeId, OutputRole>>& outputs() const { return outputs_; }

  // 4.7: DAG (producers precede consumers), input counts and domains, no dead nodes, registered ParamKeys.
  ValidationResult validate(const NodeRegistry& reg = NodeRegistry::instance()) const;
  // Node types + wiring, NOT parameter values (plan cache key).
  Hash128 structureHash() const;
  // Fills NodeInstance::key / family from a ParamSet.
  void computeKeys(const ParamSet& ps, const NodeRegistry& reg = NodeRegistry::instance());
  // Video clips (docs/engine/pipeline-architecture.md section 30.4): computeKeys, then the keys of the frame-global solve nodes (Dehaze, Detail) are replaced by those of the
  // same frame with ctx.frameIndex = 0, so the solve built from the reference frame is found again for every later frame (the film grain upstream of the solve
  // carries the frame index into the keys; a solve per frame would make the airlight and the detail base flicker).
  void computeKeysHoldingSolves(const ParamSet& ps, const NodeRegistry& reg = NodeRegistry::instance());
  // Stable text form used by the snapshot tests: one line per node with its inputs.
  std::string dump(bool withKeys = false) const;

 private:
  std::vector<NodeInstance> nodes_;
  std::vector<std::pair<NodeId, OutputRole>> outputs_;
};

// How much of the pipeline the builder spells out.
enum class GraphDetail : std::uint8_t {
  Full,    // every current stage as its own node (snapshot tests, validation, later steps)
  FullFilm,  // Full, with the film simulation decomposed into its nodes (8.1) instead of the FilmSim super-node
  Legacy,  // the stages stage (a) executes: tables as nodes, everything after them inside LegacyRender
};

// Builds the graph of one frame request. Pure function of the ParamSet (and, for Full detail, of `plan`, the
// FramePlan the renderer resolves for the frame: spatial amounts after quality / region, geometry, vignette).
class GraphBuilder {
 public:
  static Graph build(const ParamSet& ps, GraphDetail detail = GraphDetail::Legacy, const FramePlan* plan = nullptr);
};

}  // namespace dapple::engine
