#pragma once
// ResultCache: content-addressed cache of node outputs (docs/engine/pipeline-architecture.md 5.7), per EngineSession.
//
// Stage (a3) keeps the retention semantics of the `bakes[3]` slots it replaces (FIFO, a small fixed number of entries
// per node type) so that reuse decisions are the same as before; the cost/bytes-aware eviction of 5.7 comes with the
// later steps. An entry has an `order` (the lattice size): a lookup by family returns the largest entry whose order is
// at least the requested one, which is how a finer lattice serves a request for a coarser one.
#include <algorithm>
#include <memory>
#include <vector>

#include "engine/graph/types.h"

namespace dapple::engine {

class ResultCache {
 public:
  struct Entry {
    Hash128 key, family;
    int order = 0;
    std::shared_ptr<void> payload;
    std::size_t bytes = 0;
    double costMs = 0;
    Hash128 ref;  // an entry this one is built from (assembled bake inputs -> their lattice)
  };
  struct Stats {
    std::uint64_t hits = 0, misses = 0, puts = 0, evictions = 0;
  };

  ResultCache() {
    for (auto& l : limit_) l = 8;
    limit_[std::size_t(NodeType::ColourLattice)] = 3;
    limit_[std::size_t(NodeType::LegacyRender)] = 3;  // assembled bake inputs: the old bakes[3]
    limit_[std::size_t(NodeType::LatticeOverflow)] = 2;
  }

  void setLimit(NodeType t, std::size_t n) { limit_[std::size_t(t)] = n; }

  Entry* find(NodeType t, const Hash128& key) {
    for (Entry& e : slot(t))
      if (e.key == key) {
        ++stats_[std::size_t(t)].hits;
        return &e;
      }
    ++stats_[std::size_t(t)].misses;
    return nullptr;
  }

  // The entry of `family` with the largest order >= minOrder (earliest inserted among equals).
  Entry* findBest(NodeType t, const Hash128& family, int minOrder) {
    Entry* best = nullptr;
    for (Entry& e : slot(t))
      if (e.family == family && e.order >= minOrder && (!best || e.order > best->order)) best = &e;
    ++(best ? stats_[std::size_t(t)].hits : stats_[std::size_t(t)].misses);
    return best;
  }

  Entry& put(NodeType t, Entry e) {
    auto& v = slot(t);
    v.push_back(std::move(e));
    ++stats_[std::size_t(t)].puts;
    while (v.size() > limit_[std::size_t(t)]) {
      v.erase(v.begin());
      ++stats_[std::size_t(t)].evictions;
    }
    return v.back();
  }

  template <class Pred>
  void eraseIf(NodeType t, Pred&& p) {
    auto& v = slot(t);
    const auto before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), p), v.end());
    stats_[std::size_t(t)].evictions += before - v.size();
  }

  const std::vector<Entry>& entries(NodeType t) const { return entries_[std::size_t(t)]; }
  const Stats& stats(NodeType t) const { return stats_[std::size_t(t)]; }
  std::size_t bytes() const {
    std::size_t b = 0;
    for (const auto& v : entries_)
      for (const Entry& e : v) b += e.bytes;
    return b;
  }
  void clear() {
    for (auto& v : entries_) v.clear();
  }

 private:
  std::vector<Entry>& slot(NodeType t) { return entries_[std::size_t(t)]; }
  std::vector<Entry> entries_[kNodeTypeCount];
  std::size_t limit_[kNodeTypeCount];
  Stats stats_[kNodeTypeCount];
};

}  // namespace dapple::engine
