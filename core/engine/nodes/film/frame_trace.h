#pragma once
// Frame trace (off by default, enabled by dapple_frame_trace_enable / the on-device autotest): one record per rendered frame of the session API, saying what the
// frame showed (stock, which print table was bound and where it came from, its generation) and what the machine was doing (table / match bakes running or
// done on the frame thread). Cost when off: one relaxed atomic load per hook. Hooks run on the frame thread only (worker threads never write the record).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dapple::ftrace {

enum PrintKind : int { kNone = 0, kExact = 1, kCoarse = 2, kStaleSameStock = 3, kFallbackOtherStock = 4, kInterpolated = 5, kLagging = 6 };
enum MatchKind : int { kMatchNone = 0, kMatchHit = 1, kMatchInterp = 2, kMatchStalePrev = 3, kMatchComputed = 4 };

struct Rec {
  double tMs = 0, renderMs = 0, tablesMs = 0, matchMs = 0;
  std::int64_t seq = 0;
  int longEdge = 0, w = 0, h = 0, interactive = 0, quality = 0, lutN = 0;
  char stock[32] = {0};
  int printKind = 0, matchKind = 0, stockFallback = 0, wantedKind = 0;
  std::uint64_t printGen = 0, wantedGen = 0;
  int bgBake = 0, bgMatch = 0, frameBake = 0, replaced = 0;
  float printEv = 0, filmEv = 0, warmth = 0, tint = 0, black = 0;
};

struct State {
  std::atomic<bool> on{false};
  std::atomic<int> bgBake{0}, bgMatch{0}, frameBakeCtr{0};
  std::atomic<std::uint64_t> genCounter{0};
  std::atomic<std::int64_t> viewSeq{0};
  std::mutex m;
  Rec cur;
  std::vector<Rec> recs;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};
inline State& S() {
  static State* s = new State;
  return *s;
}
// true on the pool-less worker threads that never write the record (table / match workers run with the background pool selected)
inline bool on() { return S().on.load(std::memory_order_relaxed); }
inline double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - S().t0).count(); }
inline std::uint64_t nextGen() { return S().genCounter.fetch_add(1, std::memory_order_relaxed) + 1; }

// RAII counter of background work (table bake, match, calibration); cheap enough to keep always on.
struct Busy {
  std::atomic<int>& c;
  explicit Busy(std::atomic<int>& a) : c(a) { c.fetch_add(1, std::memory_order_relaxed); }
  ~Busy() { c.fetch_sub(1, std::memory_order_relaxed); }
};
// Accumulates the time of a scope into a field of the current record (frame thread, trace on).
struct Timer {
  double* dst;
  std::chrono::steady_clock::time_point t;
  explicit Timer(double* d) : dst(d), t(std::chrono::steady_clock::now()) {}
  ~Timer() { if (dst) *dst += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
};

inline void beginFrame(const char* /*unused*/ = nullptr) {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  s.cur = Rec{};
  s.cur.tMs = nowMs();
  s.cur.seq = s.viewSeq.load();
  s.cur.bgBake = s.bgBake.load();
  s.cur.bgMatch = s.bgMatch.load();
}
// Field setters (no-ops when off). The record is shared; the frame thread is the only writer while a frame renders.
template <class F>
inline void with(F&& f) {
  if (!on()) return;
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  f(s.cur);
}
inline void endFrame(double renderMs, int longEdge, int w, int h, int interactive, bool ok) {
  if (!on()) return;
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  Rec r = s.cur;
  r.renderMs = renderMs;
  r.longEdge = longEdge;
  r.w = w;
  r.h = h;
  r.interactive = interactive;
  r.bgBake = std::max(r.bgBake, s.bgBake.load());
  r.bgMatch = std::max(r.bgMatch, s.bgMatch.load());
  if (!ok) r.printKind = -1;
  r.replaced = (!s.recs.empty() && r.seq > s.recs.back().seq + 1) ? int(r.seq - s.recs.back().seq - 1) : 0;
  s.recs.push_back(r);
}
inline void clear() {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  s.recs.clear();
  s.t0 = std::chrono::steady_clock::now();
}
inline bool dump(const char* path) {
  State& s = S();
  std::lock_guard<std::mutex> l(s.m);
  std::FILE* f = std::fopen(path, "w");
  if (!f) return false;
  std::fprintf(f, "tMs\tseq\trenderMs\ttablesMs\tmatchMs\tlongEdge\tw\th\tinteractive\tstock\tframeBake\tprintKind\twantedKind\tprintGen\twantedGen\tlutN\tstockFallback\tmatchKind\tbgBake\tbgMatch\treplaced\tprintEv\tfilmEv\twarmth\ttint\tblack\n");
  for (const Rec& r : s.recs)
    std::fprintf(f, "%.2f\t%lld\t%.2f\t%.2f\t%.2f\t%d\t%d\t%d\t%d\t%s\t%d\t%d\t%d\t%llu\t%llu\t%d\t%d\t%d\t%d\t%d\t%d\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n", r.tMs, (long long)r.seq,
                 r.renderMs, r.tablesMs, r.matchMs, r.longEdge, r.w, r.h, r.interactive, r.stock, r.frameBake, r.printKind, r.wantedKind, (unsigned long long)r.printGen,
                 (unsigned long long)r.wantedGen, r.lutN, r.stockFallback, r.matchKind, r.bgBake, r.bgMatch, r.replaced, r.printEv, r.filmEv, r.warmth, r.tint, r.black);
  std::fclose(f);
  return true;
}
}  // namespace dapple::ftrace
