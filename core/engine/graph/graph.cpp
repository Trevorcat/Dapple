#include "engine/graph/graph.h"

#include <algorithm>
#include <cstdio>

namespace dapple::engine {

NodeId Graph::add(NodeType t, std::initializer_list<Edge> inputs) {
  NodeInstance n;
  n.type = t;
  n.id = NodeId(nodes_.size());
  n.in.assign(inputs.begin(), inputs.end());
  nodes_.push_back(std::move(n));
  return nodes_.back().id;
}

const NodeInstance* Graph::find(NodeType t) const {
  for (const NodeInstance& n : nodes_)
    if (n.type == t) return &n;
  return nullptr;
}

ValidationResult Graph::validate(const NodeRegistry& reg) const {
  auto fail = [](std::string m) { return ValidationResult{false, std::move(m)}; };
  if (outputs_.empty()) return fail("no output marked");
  std::vector<int> consumers(nodes_.size(), 0);
  for (const NodeInstance& n : nodes_) {
    const NodeDesc& d = reg.desc(n.type);
    const std::string where = std::string(d.name) + "#" + std::to_string(n.id);
    if (int(n.in.size()) < d.minInputs || int(n.in.size()) > d.maxInputs)
      return fail(where + ": " + std::to_string(n.in.size()) + " inputs, expected " + std::to_string(d.minInputs) + ".." +
                  std::to_string(d.maxInputs));
    for (std::size_t i = 0; i < n.in.size(); ++i) {
      const Edge& e = n.in[i];
      if (e.from >= n.id) return fail(where + ": input " + std::to_string(e.from) + " does not precede it (cycle or order)");
      const NodeDesc& pd = reg.desc(nodes_[e.from].type);
      const Domain want = d.inputDomain[std::min<std::size_t>(i, 3)];
      if (pd.output != want)
        return fail(where + ": input " + std::to_string(i) + " is " + pd.name + " (wrong domain)");
      ++consumers[e.from];
    }
    for (std::size_t k = 0; k < kMaxKeys; ++k)
      if (d.deps.bits.test(k) && k >= ParamRegistry::instance().size()) return fail(where + ": unknown ParamKey in deps");
  }
  for (const auto& o : outputs_) {
    if (o.first >= nodes_.size()) return fail("output refers to a missing node");
    ++consumers[o.first];
  }
  for (const NodeInstance& n : nodes_)
    if (!consumers[n.id]) return fail(std::string("dead node ") + reg.desc(n.type).name + "#" + std::to_string(n.id));
  return {};
}

Hash128 Graph::structureHash() const {
  Hasher h;
  h.u64(nodes_.size());
  for (const NodeInstance& n : nodes_) {
    h.u64(std::uint64_t(n.type));
    h.u64(n.in.size());
    for (const Edge& e : n.in) h.u64(std::uint64_t(e.from) << 1 | (e.keyed ? 1u : 0u));
  }
  for (const auto& o : outputs_) h.u64(0x4F00000000ull | (std::uint64_t(o.first) << 8) | std::uint64_t(o.second));
  return h.finish();
}

void Graph::computeKeys(const ParamSet& ps, const NodeRegistry& reg) {
  for (NodeInstance& n : nodes_) {
    const NodeDesc& d = reg.desc(n.type);
    Hasher h;
    h.u64(std::uint64_t(n.type));
    h.u64(d.version);
    h.hash(ps.hash(d.deps));
    for (const Edge& e : n.in)
      if (e.keyed) h.hash(nodes_[e.from].key);
    n.family = h.finish();
    if (d.orderKey != kNoKey) {
      Hasher o;
      o.hash(n.family);
      o.hash(ps.hash(d.orderKey));
      n.key = o.finish();
    } else {
      n.key = n.family;
    }
  }
}

std::string Graph::dump(bool withKeys) const {
  std::string s;
  const NodeRegistry& reg = NodeRegistry::instance();
  for (const NodeInstance& n : nodes_) {
    s += "n" + std::to_string(n.id) + " " + reg.desc(n.type).name;
    if (!n.in.empty()) {
      s += " <-";
      for (const Edge& e : n.in) s += " n" + std::to_string(e.from) + (e.keyed ? "" : "~");
    }
    for (const auto& o : outputs_)
      if (o.first == n.id) s += o.second == OutputRole::Frame ? " [frame]" : (o.second == OutputRole::Analysis ? " [analysis]" : " [table]");
    if (withKeys) s += "  " + n.key.hex().substr(0, 12);
    s += "\n";
  }
  return s;
}

}  // namespace dapple::engine
