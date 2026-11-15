#pragma once
// ResultCache: content-addressed cache of node results (docs/engine/pipeline-architecture.md 5.7), one per EngineSession.
//
// Entries live in slots: a node type's host payloads (slotOf(type)), its device copies (slotOf(type, true): the GPU executor's tables,
// solves, buffers, kept in the session's cache), and named slots for results that are not one node's (kBakeSlot: an assembled colour
// bake). Every entry carries what it costs to keep (host and device bytes) and what it cost to make (ms).
//
// Budget: one per session, host and device bytes (Limits). The host limit follows the memory configuration (dapple_memory_configure /
// the physical memory: defaultLimits()); the device limit is set by the GPU executor from its device's pool budget, which the session's
// device entries share with the pool's free images (the executor trims the pool to what the cache leaves). Eviction is LRU weighted by
// cost (GreedyDual-Size: an entry's priority is the inflation clock at its last use plus its build cost per MB; the lowest goes first and
// sets the clock), never the entry being inserted.
//
// Locking: every member function takes the cache's own mutex; results are returned by value (payloads as shared_ptr), so nothing points
// into the cache after a call returns. Payloads evicted by a call are released after the mutex is dropped (a device payload's deleter
// queues its handles for deferred destruction).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine/graph/types.h"

namespace dapple::engine {

using CacheSlot = std::uint16_t;
constexpr CacheSlot kDeviceSlot = 0x8000;
constexpr CacheSlot slotOf(NodeType t, bool device = false) { return CacheSlot(CacheSlot(t) | (device ? kDeviceSlot : 0)); }
constexpr CacheSlot kBakeSlot = 0x4001;             // assembled colour bake (lattice + tone table) of a frame
constexpr CacheSlot kDeviceBufferSlot = 0xC002;     // GPU: read-only SSBOs by content key (HDR field, reshaping field, local descriptors)
constexpr CacheSlot kDevicePlaneSlot = 0xC003;      // GPU: the local adjustments' plane store
constexpr CacheSlot kDeviceToneSlot = 0xC005;       // GPU: the tone table of a bake (the lattices are slotOf(ColourLattice, true))
constexpr bool isDeviceSlot(CacheSlot s) { return (s & kDeviceSlot) != 0; }

class ResultCache {
 public:
  struct Entry {
    CacheSlot slot = 0;
    Hash128 key, family;
    int order = 0;
    std::shared_ptr<void> payload;
    std::size_t hostBytes = 0, deviceBytes = 0;
    double costMs = 0;
    Hash128 ref;  // an entry this one is built from (assembled bake -> its lattice)
  };
  struct Hit {
    std::shared_ptr<void> payload;
    Hash128 key;
    int order = 0;
    explicit operator bool() const { return payload != nullptr; }
    template <class T>
    std::shared_ptr<T> as() const { return std::static_pointer_cast<T>(payload); }
  };
  struct Stats {
    std::uint64_t hits = 0, misses = 0, puts = 0, evictions = 0;
  };
  struct Limits {
    std::size_t hostBytes = 0, deviceBytes = 0;  // 0 = unlimited
    std::size_t entries = 0;                     // most entries kept (0 = unlimited): the small per-frame entries (an assembled bake, a tone table,
                                                 // an analysis) are kilobytes each, so the byte budgets alone let them pile up by the thousand in a long
                                                 // session, and every lookup that walks the entries (findBest, eviction) grew with them
  };
  // Host budget from the memory configuration: 1/32 of the device's memory (dapple_memory_configure totalBytes, else the physical memory),
  // 96 MB .. 384 MB, halved on a low-RAM device. Device budget: unlimited until a GPU executor sets it. Entries: kDefaultEntries.
  static constexpr std::size_t kDefaultEntries = 512;
  static Limits defaultLimits();

  explicit ResultCache(Limits l = defaultLimits()) : limits_(l) {}
  ResultCache(const ResultCache&) = delete;
  ResultCache& operator=(const ResultCache&) = delete;

  void setLimits(Limits l);
  void setDeviceLimit(std::size_t bytes);
  Limits limits() const;

  // Lookup that counts a hit / miss and marks the entry used.
  Hit find(CacheSlot s, const Hash128& key);
  // The entry of `family` with the largest order >= minOrder (counts, marks used).
  Hit findBest(CacheSlot s, const Hash128& family, int minOrder);
  // Lookups without touching statistics or recency (the planner decides on cache state before the executor uses it).
  bool contains(CacheSlot s, const Hash128& key) const;
  bool containsFamily(CacheSlot s, const Hash128& family) const;

  // Inserts (replacing an entry of the same slot and key), then evicts down to the limits.
  void put(Entry e);
  void erase(CacheSlot s, const Hash128& key);
  void eraseIf(const std::function<bool(const Entry&)>& pred);
  void clear();

  std::vector<Entry> entries(CacheSlot s) const;   // copies, oldest use first
  std::vector<Hash128> refs(CacheSlot s) const;    // the `ref` of every entry of slot s (no copies of payloads)
  Stats stats(CacheSlot s) const;
  std::size_t hostBytes() const;
  std::size_t deviceBytes() const;
  std::size_t size() const;

 private:
  struct Key {
    CacheSlot slot;
    Hash128 key;
    bool operator==(const Key& o) const { return slot == o.slot && key == o.key; }
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const { return Hash128Hash()(k.key) ^ (std::size_t(k.slot) * 0x9E3779B97F4A7C15ull); }
  };
  struct Item {
    Entry e;
    double prio = 0;
    std::uint64_t used = 0;
  };
  void touch(Item& it);
  // Evicts down to the limits (never `keep`); evicted payloads are appended to `out` (released by the caller after unlocking).
  void evictLocked(const Key* keep, std::vector<std::shared_ptr<void>>& out);

  mutable std::mutex m_;
  Limits limits_;
  std::unordered_map<Key, Item, KeyHash> items_;
  std::unordered_map<CacheSlot, Stats> stats_;
  std::size_t host_ = 0, device_ = 0;
  double clock_ = 0;
  std::uint64_t tick_ = 0;
};

}  // namespace dapple::engine
