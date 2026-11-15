#include "engine/exec/result_cache.h"

#include <algorithm>

#include "mem_probe.h"

namespace dapple::engine {

ResultCache::Limits ResultCache::defaultLimits() {
  const std::size_t total = mem::deviceTotal();
  std::size_t host = std::clamp<std::size_t>(total / 32, std::size_t(96) << 20, std::size_t(384) << 20);
  if (mem::deviceClass().lowRam.load(std::memory_order_relaxed)) host /= 2;
  return Limits{host, 0, kDefaultEntries};
}

void ResultCache::setLimits(Limits l) {
  std::vector<std::shared_ptr<void>> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    limits_ = l;
    evictLocked(nullptr, dead);
  }
}

void ResultCache::setDeviceLimit(std::size_t bytes) {
  std::vector<std::shared_ptr<void>> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    limits_.deviceBytes = bytes;
    evictLocked(nullptr, dead);
  }
}

ResultCache::Limits ResultCache::limits() const {
  std::lock_guard<std::mutex> lk(m_);
  return limits_;
}

void ResultCache::touch(Item& it) {
  const double mb = double(it.e.hostBytes + it.e.deviceBytes) / 1048576.0;
  it.prio = clock_ + it.e.costMs / std::max(1.0, mb);
  it.used = ++tick_;
}

ResultCache::Hit ResultCache::find(CacheSlot s, const Hash128& key) {
  std::lock_guard<std::mutex> lk(m_);
  Stats& st = stats_[s];
  auto it = items_.find(Key{s, key});
  if (it == items_.end()) {
    ++st.misses;
    return {};
  }
  ++st.hits;
  touch(it->second);
  return Hit{it->second.e.payload, it->second.e.key, it->second.e.order};
}

ResultCache::Hit ResultCache::findBest(CacheSlot s, const Hash128& family, int minOrder) {
  std::lock_guard<std::mutex> lk(m_);
  Item* best = nullptr;
  for (auto& [k, it] : items_)
    if (k.slot == s && it.e.family == family && it.e.order >= minOrder &&
        (!best || it.e.order > best->e.order || (it.e.order == best->e.order && it.used < best->used)))
      best = &it;
  Stats& st = stats_[s];
  if (!best) {
    ++st.misses;
    return {};
  }
  ++st.hits;
  touch(*best);
  return Hit{best->e.payload, best->e.key, best->e.order};
}

bool ResultCache::contains(CacheSlot s, const Hash128& key) const {
  std::lock_guard<std::mutex> lk(m_);
  return items_.count(Key{s, key}) != 0;
}

bool ResultCache::containsFamily(CacheSlot s, const Hash128& family) const {
  std::lock_guard<std::mutex> lk(m_);
  for (const auto& [k, it] : items_)
    if (k.slot == s && it.e.family == family) return true;
  return false;
}

void ResultCache::put(Entry e) {
  std::vector<std::shared_ptr<void>> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    const Key k{e.slot, e.key};
    Stats& st = stats_[e.slot];
    ++st.puts;
    auto it = items_.find(k);
    if (it != items_.end()) {
      host_ -= it->second.e.hostBytes;
      device_ -= it->second.e.deviceBytes;
      dead.push_back(std::move(it->second.e.payload));
      it->second.e = std::move(e);
    } else {
      it = items_.emplace(k, Item{std::move(e), 0, 0}).first;
    }
    host_ += it->second.e.hostBytes;
    device_ += it->second.e.deviceBytes;
    touch(it->second);
    evictLocked(&k, dead);
  }
}

void ResultCache::evictLocked(const Key* keep, std::vector<std::shared_ptr<void>>& out) {
  // kind 0: host bytes, 1: device bytes, 2: entry count (any entry is a victim)
  auto over = [&](int kind) {
    return kind == 1 ? (limits_.deviceBytes && device_ > limits_.deviceBytes)
                     : (kind == 0 ? (limits_.hostBytes && host_ > limits_.hostBytes) : (limits_.entries && items_.size() > limits_.entries));
  };
  for (int kind : {0, 1, 2}) {
    while (over(kind)) {
      auto victim = items_.end();
      for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (keep && it->first == *keep) continue;
        if (kind < 2 && (kind == 1 ? it->second.e.deviceBytes : it->second.e.hostBytes) == 0) continue;
        if (victim == items_.end() || it->second.prio < victim->second.prio ||
            (it->second.prio == victim->second.prio && it->second.used < victim->second.used))
          victim = it;
      }
      if (victim == items_.end()) break;
      clock_ = std::max(clock_, victim->second.prio);
      host_ -= victim->second.e.hostBytes;
      device_ -= victim->second.e.deviceBytes;
      ++stats_[victim->first.slot].evictions;
      out.push_back(std::move(victim->second.e.payload));
      items_.erase(victim);
    }
  }
}

void ResultCache::erase(CacheSlot s, const Hash128& key) {
  eraseIf([&](const Entry& e) { return e.slot == s && e.key == key; });
}

void ResultCache::eraseIf(const std::function<bool(const Entry&)>& pred) {
  std::vector<std::shared_ptr<void>> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = items_.begin(); it != items_.end();) {
      if (pred(it->second.e)) {
        host_ -= it->second.e.hostBytes;
        device_ -= it->second.e.deviceBytes;
        ++stats_[it->first.slot].evictions;
        dead.push_back(std::move(it->second.e.payload));
        it = items_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void ResultCache::clear() {
  std::unordered_map<Key, Item, KeyHash> dead;
  {
    std::lock_guard<std::mutex> lk(m_);
    dead.swap(items_);
    host_ = device_ = 0;
  }
}

std::vector<ResultCache::Entry> ResultCache::entries(CacheSlot s) const {
  std::vector<std::pair<std::uint64_t, Entry>> v;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& [k, it] : items_)
      if (k.slot == s) v.emplace_back(it.used, it.e);
  }
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Entry> out;
  out.reserve(v.size());
  for (auto& p : v) out.push_back(std::move(p.second));
  return out;
}

std::vector<Hash128> ResultCache::refs(CacheSlot s) const {
  std::vector<Hash128> out;
  std::lock_guard<std::mutex> lk(m_);
  for (const auto& [k, it] : items_)
    if (k.slot == s) out.push_back(it.e.ref);
  return out;
}

ResultCache::Stats ResultCache::stats(CacheSlot s) const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = stats_.find(s);
  return it == stats_.end() ? Stats{} : it->second;
}

std::size_t ResultCache::hostBytes() const {
  std::lock_guard<std::mutex> lk(m_);
  return host_;
}
std::size_t ResultCache::deviceBytes() const {
  std::lock_guard<std::mutex> lk(m_);
  return device_;
}
std::size_t ResultCache::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return items_.size();
}

}  // namespace dapple::engine
