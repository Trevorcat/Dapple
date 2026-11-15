#pragma once
// Internal to EngineSession, split along its seams:
//   engine_session.cpp  the frame policy: one graph per frame (built from the frame's draft blocks), the host tables, the executor choice,
//                       the CPU frame and the exact (export / reference) render, accounting
//   host_tables.cpp     the HostTable stage: the table nodes of the frame's graph through their bakes, cached in the session's ResultCache
//   gpu_dispatch.cpp    the GPU executor selection and failure semantics (5.9) by typed reason, the GPU frame / exact / export renders
#include <atomic>
#include <chrono>
#include <string>

#include "engine/exec/engine_session.h"

namespace dapple::engine::session {

struct Totals {
  std::atomic<std::uint64_t> gpuFrames{0}, gpuFallbacks{0}, gpuTransients{0}, gpuLost{0}, gpuDemotions{0};
  std::atomic<std::uint64_t> frames{0}, fullHits{0}, latticeReuse{0}, latticeBake{0};
  std::atomic<std::uint64_t> planned{0}, planUs{0}, execUs{0}, sweeps{0}, tiles{0}, solveHits{0}, cancelled{0}, fallbacks{0};
};
Totals& totals();

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

Hash128 combine(const Hash128& a, const Hash128& b);
Hash128 withOrder(const Hash128& family, int order);
void gpuLog(const char* what, const std::string& why);
void addStats(EngineStats& st, const ExecStats& es);
// The plan request of a w x h frame with the given blocks.
PlanRequest request(int w, int h, ExecMode mode, int tile, std::size_t budget, const FrameView& view);
// Computes the node keys of a frame graph (a video clip holds its solves at the reference frame's keys).
void keyGraph(Graph& g, const ParamSet& ps, bool holdSolves);
void notePlanned(EngineStats& st, double ms);

}  // namespace dapple::engine::session
