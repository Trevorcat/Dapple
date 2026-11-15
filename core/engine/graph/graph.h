#pragma once
// Node descriptors, graph, validation, builder (docs/engine/pipeline-architecture.md sections 4.2-4.7). Stage a1.
//
// The graph is a pure description: building it executes nothing. Node content keys are
//   key = H(type, version, params.hash(desc.deps), ctx order key, keys of the keyed inputs)
// so changing a slider changes exactly the keys of the nodes that declare (or inherit) it.
#include <string>
#include <vector>

#include "engine/graph/node.h"
#include "engine/graph/params.h"
#include "engine/graph/types.h"

namespace dapple::engine {

// Where the builder wires an optional node (in NodeDesc::order within its slot, when NodeDesc::wanted says so):
//   Source   after the Resize (Geometry)       Scene    after the Gain, on the normal branch (Hdr, LocalLinear)
//   Display  after the merge of the branches, before the 8-bit pack (Vignette, the window nodes, Grain)
//   Skeleton not optional: the builder's frame skeleton (source, tables, branches, blend, pack) places it
enum class Wire : std::uint8_t { Skeleton, Source, Scene, Display };
// Execution group a node is fused into by the planner:
//   Head  the point chain of a sweep (CPU: 256-pixel block fragments; GPU: one recipe pass)
//   Film  the film stage (whole-frame, before the sweeps)
//   None  its own group (window nodes, tables, source, sink)
enum class Fusion : std::uint8_t { None, Head, Film };

struct BuildCtx {
  const ParamSet& ps;
  const FrameView& view;
};

struct NodeDesc {
  NodeType type = NodeType::Source;
  const char* name = "";
  std::uint16_t version = 1;
  Domain output = Domain::Image;
  Footprint footprint = Footprint::Point;
  int minInputs = 0, maxInputs = 0;
  Domain inputDomain[4] = {Domain::Image, Domain::Image, Domain::Image, Domain::Image};
  ParamMask deps;                  // ParamKeys + context keys this node reads itself (inputs add their own)
  ParamKey orderKey = kNoKey;      // a context key that selects among interchangeable results (lattice size): part of the node key but not
                                   // of its family; the planner looks such a node up by family (any cached order serves)
  // ---- port tags (read by Graph::validate, the planner and the GPU executor) ----
  Space inSpace[4] = {Space::Any, Space::Any, Space::Any, Space::Any};  // image inputs: the space they must carry (Any: passed through)
  Space outSpace = Space::Any;     // Any: the space of input 0
  Precision precision = Precision::F16;
  bool rescales = false;           // Resample that changes the grid scale (Resize): source windows of the plan are in the grid after it
  // ---- execution ----
  Fusion fusion = Fusion::None;
  Wire wire = Wire::Skeleton;
  int order = 0;                                     // position within the wire slot
  bool (*wanted)(const BuildCtx&) = nullptr;         // optional node: wired when true (null: when its block is not the identity)
  const NodeBehaviour* behaviour = nullptr;          // identity / halo / inputRect over the block
  const void* (*block)(const FrameView&) = nullptr;  // the node's resolved block in a frame (null: none)
  Accum (*solve)(const void* block) = nullptr;       // the frame-global solve the node needs for this block (null / None: none)
  int (*workPlanes)(const void* block) = nullptr;    // float planes per pixel of tile scratch it needs (memory estimate)
  bool readsAux = false;                             // reads the per-pixel side planes a head node writes (local adjustments)
  const CpuImpl* cpu = nullptr;
  bool (*bind)(const FrameEnv&, Resolved&) = nullptr;  // binds the CPU slot for a frame (null: no slot)
};

class NodeRegistry {
 public:
  static const NodeRegistry& instance();
  const NodeDesc& desc(NodeType t) const { return descs_[std::size_t(t)]; }
  static const char* name(NodeType t) { return instance().desc(t).name; }
  // The block of node type `t` in a frame (null when the node has none or the view lacks it).
  const void* block(NodeType t, const FrameView& v) const {
    const NodeDesc& d = desc(t);
    return d.block ? d.block(v) : nullptr;
  }
  // The node is the identity for this block (not wired / elided).
  bool identity(NodeType t, const FrameView& v) const {
    const NodeDesc& d = desc(t);
    const void* b = block(t, v);
    return b && d.behaviour && d.behaviour->isIdentity && d.behaviour->isIdentity(b);
  }
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
  NodeId add(NodeType t, const std::vector<Edge>& inputs);
  void markOutput(NodeId id, OutputRole role) { outputs_.push_back({id, role}); }
  const std::vector<NodeInstance>& nodes() const { return nodes_; }
  const NodeInstance* find(NodeType t) const;
  const std::vector<std::pair<NodeId, OutputRole>>& outputs() const { return outputs_; }

  // 4.7: DAG (producers precede consumers), input counts and domains, colour spaces of the image ports, no dead nodes, registered ParamKeys.
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

// Builds the graph of one frame request: the skeleton (source, Resize, table nodes, the normal and the film branch, their blend, the 8-bit
// pack) and every optional node (NodeDesc::wire) its family wants for the frame's resolved blocks. Pure function of the ParamSet and the
// blocks; nothing is executed. A frame builds it once: the session evaluates the table nodes of this graph, then plans and executes it.
class GraphBuilder {
 public:
  static Graph build(const ParamSet& ps, const FrameView& view);
};

}  // namespace dapple::engine
