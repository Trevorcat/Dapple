#pragma once
// EngineSession: per-photo engine state (docs/engine/pipeline-architecture.md section 3, stage a2/a3).
//
// renderFrame() builds the frame's graph (GraphBuilder, Legacy detail), computes the node keys, evaluates the table
// nodes through the ResultCache (a3) and then runs the LegacyRender super-node, which calls the existing
// Renderer::renderBakedRgba (optionally through a GPU hook first). The C layer keeps everything in front of it
// (parsing, LUT, scaled source, proxy).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "dapple/film.h"
#include "dapple/renderer.h"
#include "engine/exec/gpu_iface.h"
#include "engine/exec/result_cache.h"
#include "engine/exec/source_store.h"
#include "engine/graph/graph.h"
#include "engine/plan/plan.h"

namespace dapple::engine {

// DAPPLE_ENGINE: unset (default) = full: every frame and export goes through the Planner and the CpuExecutor / GpuExecutor.
// "0" / "off" = the C layer's own path with the old Renderer (bakes[3] + renderBakedRgba): kept only as the golden oracle and
// the A/B switch until the device sign-off (docs/engine/pipeline-architecture.md 10.7). The intermediate "legacy" / "tables"
// modes are gone (their behaviour is the Full engine's table stage).
enum class Mode : int { Off = 0, Full = 1 };
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
// The per-frame parameters of a table node's CpuImpl::bake (`resolved`). n = lattice size; fast / maxV as the node needs.
struct TableBakeCtx {
  const TableEnv* env = nullptr;
  int n = 0;
  bool fast = false;
  float maxV = 0.f;
};
// The per-frame parameters of the Resize node's CpuImpl::bake: the source resampled to `longEdge`.
struct ResizeBakeCtx {
  const SourceImage* full = nullptr;
  int longEdge = 0;
};
// Runs `type`'s CpuImpl::bake; null payload when the node has no bake or it failed. Payload types: ColourLattice and
// LatticeOverflow std::vector<float>, ToneAnalysisPre Renderer::TonePre, ToneTable ToneTableOut, Analysis ToneAnalysis,
// Resize SourceImage.
std::shared_ptr<void> bakeNode(NodeType type, const void* resolved, std::span<const void* const> inputs = {});

// Convenience wrappers over bakeNode (conformance tests, bench).
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
  // The full-resolution source for the GPU executor (the CPU executors read `src`, which may be a scaled copy of it: the GPU
  // resizes the store to src's size itself). Null = this frame is CPU only.
  SourceStore* store = nullptr;
  float resizeSharpen = 0.f;  // extra output sharpening when the GPU downscales (export), 0 for frames
  // GPU only: render from the store resized to this long edge, whatever `src` is (tests / bench that bake from the full image).
  // 0 = the size of `src` (the C layer passes its CPU-scaled copy).
  int srcLongEdge = 0;
  // GPU only, zero readback (docs/engine/present.md): the final 8-bit picture is copied into this image, which belongs to the
  // presenter (hal::ExternalImage: VkImage + layouts + semaphores). On success FrameResult::rgba stays null and `external` is
  // set; when the GPU cannot take the frame the CPU result is returned in rgba as usual (the presenter uploads it).
  const hal::ExternalImage* gpuTarget = nullptr;
  // CPU time the C layer spent before the engine call (film exposure match, Renderer construction): reported in FrameResult::cpu
  double matchMs = 0, rendererMs = 0;
  // Video frames (docs/engine/pipeline-architecture.md section 30): non-null = the clamped, display-encoded float frame goes here instead of the
  // 8-bit rgba (CPU executor, Whole plan; the GPU executor writes 8-bit, so such a frame is always a CPU frame). FrameResult::rgba stays null.
  ImageF* outFloat = nullptr;
  // Video clips: the frame-global solves (dehaze, detail base) are found under the keys of frame index 0, i.e. they are held from the reference frame.
  bool holdSolves = false;
};

struct FrameResult {
  std::uint8_t* rgba = nullptr;  // malloc()ed, the caller free()s
  int w = 0, h = 0;
  bool hasAnalysis = false;
  ToneAnalysis analysis;
  std::string error;
  // what this frame did (tests, edit-bench)
  enum class Bake : std::uint8_t { FullHit, LatticeReuse, LatticeBake } bake = Bake::FullHit;
  std::string executor;  // "cpu", "vulkan" or "gles31": who produced the pixels (Full mode)
  std::string why;       // when the CPU rendered although a GPU was asked for or could have been: the reason (Unsupported text, "GPU self-test running", ...)
  bool external = false; // the pixels are in FrameJob::gpuTarget, not in rgba
  bool floatOut = false; // the pixels are in FrameJob::outFloat, not in rgba
  GpuStats gpu;          // GPU runs only
  // CPU time of this frame by stage, ms (reported by the throttled dapple_engine line): match = film exposure solve (C layer), renderer = Renderer
  // construction (C layer), graph = ParamSet + graph + keys, tables = host tables (lattice, tone, film), exec = plan + executor incl. GPU wait
  struct CpuSplit {
    double matchMs = 0, rendererMs = 0, graphMs = 0, tablesMs = 0, execMs = 0, totalMs = 0;
  } cpu;
};

struct EngineStats {
  std::uint64_t frames = 0, fullHits = 0, latticeReuse = 0, latticeBake = 0, toneTableBuilds = 0, preBuilds = 0,
                analysisBuilds = 0;
  double graphMs = 0;  // accumulated ParamSet + build + validate + keys time
  // Full mode
  std::uint64_t planned = 0, sweeps = 0, tiles = 0, solveHits = 0, solveBuilds = 0, streamed = 0, cancelled = 0, fallbacks = 0;
  double planMs = 0, execMs = 0;
  // GPU executor (5.9): frames it produced, plans it could not run (CPU took them), transient failures, device losses
  std::uint64_t gpuFrames = 0, gpuUnsupported = 0, gpuTransients = 0, gpuLost = 0;
  // of gpuUnsupported: tiled exports of a film with the enlarger diffusion filter (a whole-frame blur, no window form, docs/engine/pipeline-architecture.md 28.3)
  std::uint64_t gpuEnlargerTiled = 0;
};

// Process-wide GPU counters (dapple_gpu_status_json).
struct GpuCounters {
  std::uint64_t frames = 0, fallbacks = 0, transients = 0, lost = 0, demotions = 0;
};
GpuCounters gpuCounters();

// Per-frame executor accounting (EngineSession::renderFrame, process-wide): which executor rendered each frame and, for CPU frames, why the GPU did
// not. JSON: {"gpuFrames":n,"cpuFrames":n,"deviceLost":n,"api":"vulkan|gles31|cpu","unsupported":{"<reason>":n,..}}. Also logged (logcat tag
// dapple_engine on Android, stderr when DAPPLE_ENGINE_LOG=1 elsewhere): once per 30 frames and on every change of executor or reason.
std::string executorStatsJson();

// True when frames go through the engine: DAPPLE_ENGINE is on, or the GPU runtime flag is on (the GPU executor consumes the
// Planner's Plan, so a GPU session always runs the Full engine).
bool engineInUse();

class EngineSession {
 public:
  EngineSession();
  bool renderFrame(FrameJob& job, FrameResult& out);
  const EngineStats& stats() const { return stats_; }
  ResultCache& cache() { return cache_; }
  // Latest-wins cancellation (5.8): bumps the ticket; a render in flight returns early (error "cancelled"). Lock-free, any thread.
  void cancel() { latest_.fetch_add(1, std::memory_order_relaxed); }
  std::uint64_t ticket() const { return latest_.load(std::memory_order_relaxed); }

  // Export / reference render (Full graph, exact variant: per-pixel tone + colour, always full quality) into RGB8.
  // mode = TiledExport splits the frame into tiles of `tile` px (0 = from the memory budget). false: failed (error in `err`).
  struct ExactOptions {
    ExecMode mode = ExecMode::Whole;
    int tile = 0;
    std::size_t memBudget = 512u << 20;
    bool spatial = true;
    bool hasProxy = false;
    // false (export): the frame-global solves are cached in a private cache that lives for this call only. A solve is keyed by
    // the node keys of the frame; an export has no LUT identity or other pointer-valued context that could make two different
    // renders share a key, and it never hits anyway. true: the session cache (tests of the sweep pruning).
    bool sessionCache = false;
    // Test hook: runs after the cancel ticket was taken and before execution (a deterministic cancel point).
    std::function<void()> beforeExec;
    // Non-null: the clamped, display-encoded float frame goes here instead of the 8-bit `out` (Whole plans; analysis, swatches).
    ImageF* outFloat = nullptr;
  };
  bool renderExact(const EditParams& params, const Renderer& renderer, const SourceImage& src, const ExactOptions& opt,
                   Image8& out, std::string* err = nullptr, ExecStats* stats = nullptr);

  // The graph of the last frame (diagnostics, tests).
  const Graph& lastGraph() const { return graph_; }

  // ---- GPU executor selection (5.9) ----------------------------------------------------------------------------------------------
  // Tests / start-up self-test: run exactly this executor instead of the provider's, and force it (a plan it cannot run is an
  // error of the frame, not a CPU fallback). nullptr = back to the provider.
  void useGpuExecutor(IGpuExecutor* x, bool force);
  // Test seam: the next `count` GPU runs of this session fail with this classification before touching the device.
  void injectGpuFailure(GpuFailure f, int count);
  // Full-picture export of the baked pipeline on the GPU (tiles of `tile` px, frame-global solves from a sweep over all tiles).
  // Same pixels the app exports on the GPU path: lattice 65, Lanczos downscale to outLongEdge with a little output sharpening.
  struct GpuExport {
    const EditParams& params;
    Renderer& renderer;
    const SourceImage& image;   // full resolution (the tables are baked from it)
    SourceStore& store;         // the same pixels, for the executor
    const CameraProfile& camera;
    const Lut3D* lut = nullptr;
    int outLongEdge = 0;        // 0 = natural size
    int tile = 0;               // 0 = 2048 (1024 on unified memory)
    float resizeSharpen = 0.2f;
    int latticeN = 65;          // lattice size of the bake
    bool forceStream = false;   // tests: never keep the source resident
    GpuTarget target;           // target.sink receives the tiles
    std::function<void(int w, int h)> onSize;  // the output size, called before the first tile (allocate the destination here)
  };
  // 0 = done, 1 = the GPU cannot / did not do it (nothing written to the sink beyond what an aborted run left; the caller renders on the CPU),
  // -1 = cancelled / failed hard (err).
  int exportGpu(GpuExport& job, GpuStats* stats, std::string* err);

 private:
  bool gpuUsable(const FrameJob& job) const;
  // One plan on the GPU with the selection of 5.9. 0 = out filled, 1 = CPU takes the frame, -1 = cancelled.
  int runOnGpu(const GpuRun& run, GpuOut& out);
  int renderGpu(FrameJob& job, const ParamSet& ps, const BakedLut& bake, FrameResult& out);
  // renderExact on the GPU executor (the exact variant: ToneColourExact through a dense lattice bake): 0 = `out` holds the frame, 1 = the CPU takes it,
  // -1 = failed / cancelled (err)
  int renderExactGpu(const EditParams& params, const Renderer& renderer, const SourceImage& src, const ExactOptions& opt, Image8& out,
                     std::string* err);
  const std::vector<float>* overflowFor(const ParamSet& ps, int n) const;
  hal::Api gpuLastApi_ = hal::Api::Null;
  IGpuExecutor* gpuExecutor();
  std::unique_ptr<IGpuExecutor> gpuOwned_;
  std::uint64_t gpuGen_ = 0;
  IGpuExecutor* gpuOverride_ = nullptr;
  bool gpuForce_ = false;
  std::string gpuWhy_;  // last reason for not using the GPU (diagnostics)
  std::string frameWhy_;  // reason the GPU did not render the frame being rendered (reset by renderFrame; FrameResult::why)

  // The HostTable stage: evaluates the table nodes of `graph_` through their CpuImpl::bake (cached in the ResultCache under the
  // effective node keys) and assembles the frame's BakedLut.
  std::shared_ptr<const BakedLut> runHostTables(FrameJob& job, const ParamSet& ps, int n, FrameResult& out);

  // 0 = out.rgba is the frame, 1 = not supported here (render with the legacy renderer), -1 = failed / cancelled (out.error)
  int renderFull(FrameJob& job, const ParamSet& ps, const BakedLut& bake, FrameResult& out);

  std::uint64_t id_;
  std::atomic<std::uint64_t> latest_{1};
  ResultCache cache_;
  EngineStats stats_;
  Graph graph_;

 public:
  // The Resize node: the session source resampled to `longEdge` (CpuImpl::bake of Resize, cached; at most 4 copies / ~160 MB,
  // least recently used first out). Null when the source is not larger than `longEdge` (the caller reads `full` itself).
  // The returned image stays valid while the caller holds it, whatever the cache does.
  // cacheLarge = false (exports, previews): a copy of more than 64 MB is returned but not kept.
  std::shared_ptr<const SourceImage> resizedSource(const SourceImage& full, int longEdge, bool cacheLarge = true);

 private:
  std::mutex resizeMtx_;
};

// One-shot reference-quality renders for callers that have no DappleSession (CLI, developer, tone analysis, swatches): a private
// EngineSession, the exact variant of the Full graph through the Planner and the CpuExecutor (frames above 16 MP in tiles). The old
// Renderer is only the parameter resolver here. `spatial` = false skips the neighbourhood stages. False: failed (error in `err`).
// With DAPPLE_ENGINE=off these still work (they are the engine's own entry points, not the oracle).
bool renderImage8(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, Image8& out,
                  std::string* err = nullptr, float toneReach = 1.f);
bool renderImageF(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, bool spatial,
                  ImageF& out, std::string* err = nullptr);

// Process-wide summary for dapple_engine_info_json().
std::string infoJson();

}  // namespace dapple::engine
