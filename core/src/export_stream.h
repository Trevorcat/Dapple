#pragma once
// Streaming (band) export: docs/engine/pipeline-architecture.md section 42. Internal header (core/src).
//
// A full-resolution export of a large RAW never holds the picture whole: the session keeps the cached 16-bit mosaic (LazyFull), the picture-wide
// analyses come from exact reductions built band by band in one pass (analyse()), and the render runs in bands whose source pixels are demosaiced
// on demand from the mosaic (BandSource), straight into the JPEG strip encoder. Peak memory is the mosaic + a few bands + the reductions, whatever
// the picture size.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"
#include "dapple/mosaic.h"
#include "engine/exec/engine_session.h"
#include "engine/exec/source_store.h"
#include "local_eval.h"

namespace dapple::xstream {

// DAPPLE_EXPORT_STREAM=on|off|auto (default auto): on = every FULL open of a mosaic RAW is lazy and its exports stream; off = never; auto = when the
// predicted peak of the whole-frame path exceeds the budget.
enum class Mode { Off, On, Auto };
Mode envMode();

// ---- the export memory policy (pipeline-architecture.md 42.9) ----
// budget = min(share, ceiling, room):
//   share    what the OS says is free: iOS os_proc_available_memory x 0.6 (this process's own jetsam headroom), Android / desktop MemAvailable
//            (available physical memory) x 0.5, minus 96 MB for the app's own growth; when the query fails a quarter of the device minus half
//            of what the process holds, clamped to [256 MB, 1 GB]
//   ceiling  phones only, independent of what is free right now: what one export may add on top of the app = min(total / 4, 1.5 GB)
//            (low-RAM devices: min(total / 8, 384 MB)). MemAvailable on a 12 GB phone says 5 GB, but lmkd and the GPU driver do not let one app
//            take 4 GB reliably (Note10+: a 3.9 GB whole-frame film export, 4 failed vkAllocateMemory)
//   room     Android only: the whole process stays under 35 % of the device (low-RAM 25 %): that cap minus the current footprint
// The band row cache (42.8) gets at most min(budget - machinery, budget / 2) and, on phones, at most 256 MB (low-RAM 96 MB).
// DAPPLE_EXPORT_BUDGET_MB overrides the whole rule (tests); DAPPLE_EXPORT_DEVICE_MB=<total> applies the phone rule (Android variant) on any
// platform with that device size (tests, desktop emulation).
struct BudgetInputs {
  std::size_t available = 0;  // OS figure, 0 = unknown
  std::size_t total = 0;      // device memory
  std::size_t footprint = 0;  // what the process holds now
  bool lowRam = false;
  int platform = 0;  // 0 desktop, 1 Android, 2 iOS
};
struct Budget {
  std::size_t bytes = 0;  // what an export may still add
  std::size_t share = 0, ceiling = 0, room = 0;  // the terms (ceiling / room = SIZE_MAX where they do not apply)
  std::size_t rowCacheCap = 0;                   // SIZE_MAX = only the budget limits the row cache
  std::string how;                               // log text: "available memory 5455 MB x 0.5 - 96 = 2631, ceiling 1536, room 2465 -> 1536 MB"
};
Budget computeBudget(const BudgetInputs& in);
BudgetInputs currentInputs();  // this device now
Budget currentBudget();
// Bytes an export may still allocate on this device (currentBudget().bytes); `how` = the rule's terms for the log. DAPPLE_EXPORT_BUDGET_MB overrides.
std::size_t budgetBytes(std::string* how = nullptr);
// Extra bytes per pixel of a whole-frame export when the GPU executor renders it on unified memory (phones: the GPU's images come out of the same
// RAM): the resident RGBA16F source (8 B/px, shared with what the CPU path drops: ~2 B/px net) and, with a film stock, the frame-sized film images
// (~24 B/px; M1 Metal vs CPU: 26 MP +643 MB, 40 MP +936 MB); plus the executor pool (<= 320 MB at tile 1024).
std::size_t predictGpuExtra(int W, int H, bool film, std::size_t poolBytes);
// Releases what an export can do without before the budget is taken: the cached mosaics / pictures of other photos, the frame-sized film
// workspaces of earlier renders. Bytes released (what the process may then reuse; the OS footprint drops when the allocator returns them).
std::size_t trimForExport(const std::uint64_t keepMosaic[2]);
// What the whole-frame path adds on top of the session for a W x H RAW picture: the float source (12 B/px), the 8-bit output (3 B/px), the
// executor tiles and the encoder (~4 B/px); a film edit adds its frame planes (~55 B/px).
std::size_t predictWholePeak(int W, int H, bool film);
// The whole decode of a RAW the band path cannot read (LibRaw processes it: raw + its 16-bit image + the memory image + the float picture,
// ~30 B/px measured on a 12 MP linear DNG).
std::size_t predictWholeDecodePeak(int W, int H);
// At a FULL open: open lazily (keep the mosaic, no float picture)? `why` says on what grounds (log line).
bool openLazy(int W, int H, std::string* why = nullptr, std::size_t extraBytes = 0, bool film = false);  // extraBytes: what the GPU adds (predictGpuExtra); film: the edit is known (dapple_export_intent) and has a stock

// The oriented, cropped full-resolution picture of a cached mosaic, read in windows: the same Demosaic node the full decode would run (pinned for
// the whole picture), then the shadow-chroma floor when the picture-wide decision says so. One window is kept (valid until the next call).
class BandSource {
 public:
  explicit BandSource(std::shared_ptr<const MosaicImage> mosaic);
  int width() const { return W_; }
  int height() const { return H_; }
  bool gpu() const { return gpu_; }
  bool fellBack() const { return fellBack_; }
  void setShadowFloor(bool on) {
    if (on != shadow_) bandOk_ = false;  // the band's rows were made with the other setting
    shadow_ = on;
  }
  bool shadowFloor() const { return shadow_; }
  // [x0, x1) x [y0, y1) (must lie inside the picture). Null when cancelled (cancel != null and *cancel != mine).
  const ImageF* window(int x0, int y0, int x1, int y1);
  void setCancel(const std::atomic<std::uint64_t>* cur, std::uint64_t mine) { cancelCur_ = cur, cancelMine_ = mine; }
  std::uint64_t windows() const { return windows_; }
  double demosaicMs() const { return ms_; }
  double demosaicMpx() const { return double(px_) / 1e6; }  // demosaiced area (window pixels, halos included), for the stage log
  std::size_t peakWindowBytes() const { return peakBytes_; }
  void release() { ImageF().data.swap(win_.data), win_ = ImageF(), bandOk_ = false; }
  // Rows [y0, y1), columns [x0, x1) of the picture for a walk over tiles in rows (the GPU executor's streamed tiles): a pointer into a cached row block
  // when the rect lies in one, else into a full-width band (at least kBandRows rows) that rolls down the picture: the rows it shares with the previous
  // band move to its top, cached rows are copied, only the rest is demosaiced (region demosaic == the same pixels of the whole demosaic), so a picture
  // is demosaiced about once instead of once per tile with its halo. Cached blocks above the band are dropped as the walk passes them. A walk down a
  // column of the picture (a quarter turn) gets its own window per rect. Valid until the next call; pitch = width * 3 for a band or a block.
  static constexpr int kBandRows = 768;
  const float* rows(int x0, int y0, int x1, int y1, std::size_t& pitchFloats);

  // Row cache (section 42.8): demosaiced full-width rows kept up to `budget` bytes, so that the render reuses what the analysis pass made
  // instead of demosaicing it again. The analysis pass runs bottom to top and offers its rows (keep()); when the cache is full the lowest
  // rows give way (the render starts at the top). A window whose rows are all cached is copied out of the cache.
  void setCacheBudget(std::size_t bytes) { cacheBudget_ = bytes; }
  void keep(const ImageF& rowsOfWindow, int y0, int ya, int yb);  // rows [ya, yb) of a full-width window starting at row y0
  void applyShadowFloorToCache();                                  // the floor was decided after the rows were kept
  void dropCache();
  std::size_t cacheBytes() const { return cacheBytes_; }
  std::uint64_t cacheHits() const { return cacheHits_; }
  int cachedRows() const;
  // Row accounting (stage log, tests): how often each picture row was demosaiced since resetRowCounts() (a rolling band keeps the rows it shares with the next
  // tile row, so a streamed render demosaics a row once; rows the analysis pass cached are not demosaiced at all)
  void resetRowCounts() { rowCount_.assign(std::size_t(std::max(H_, 0)), 0); }
  int rowsDemosaiced() const { return int(std::count_if(rowCount_.begin(), rowCount_.end(), [](std::uint8_t c) { return c > 0; })); }
  int rowsDemosaicedAgain() const { return int(std::count_if(rowCount_.begin(), rowCount_.end(), [](std::uint8_t c) { return c > 1; })); }
  int rowsNotDemosaiced() const { return int(rowCount_.size()) - rowsDemosaiced(); }
  double bandCopyMs() const { return copyMs_; }  // time of the rolling band outside the demosaic (kept-row moves, cached-row copies, buffer growth)

 private:
  std::shared_ptr<const MosaicImage> m_;
  int W_ = 0, H_ = 0;
  bool gpu_ = false, fellBack_ = false, shadow_ = false;
  ImageF win_;
  const std::atomic<std::uint64_t>* cancelCur_ = nullptr;
  std::uint64_t cancelMine_ = 0, windows_ = 0;
  double ms_ = 0;
  std::uint64_t px_ = 0;
  std::size_t peakBytes_ = 0;
  int rowsY0_ = 0;       // first picture row of win_ when it is a full-width band (bandOk_)
  bool bandOk_ = false;  // win_ holds full-width rows [rowsY0_, rowsY0_ + win_.height), demosaiced (and floored) completely
  int lastX0_ = -1, lastY0_ = -1, lastY1_ = -1;  // previous narrow rect of rows(): tells a walk along rows from a walk down a column
  bool band(int b0, int b1);                     // win_ = full-width rows [b0, b1) (kept / cached / demosaiced rows); false when cancelled or failed
  bool demosaicRows(int ya, int yb, float* dst);  // full-width rows [ya, yb) demosaiced (and floored) into dst (pitch W_ * 3)
  const float* blockRows(int y0, int y1) const;   // rows [y0, y1) when one cached block holds them all, else null
  struct Block {
    int y0 = 0, y1 = 0;
    std::vector<float> px;  // (y1 - y0) full-width rows
  };
  std::vector<Block> cache_;  // disjoint, any order
  std::size_t cacheBudget_ = 0, cacheBytes_ = 0;
  std::uint64_t cacheHits_ = 0;
  bool fromCache(int x0, int y0, int x1, int y1);
  std::vector<std::uint8_t> rowCount_;
  double copyMs_ = 0;
  void countRows(int ya, int yb) {
    if (rowCount_.size() != std::size_t(std::max(H_, 0))) rowCount_.assign(std::size_t(std::max(H_, 0)), 0);
    for (int y = ya; y < yb; ++y)
      if (rowCount_[std::size_t(y)] < 255) ++rowCount_[std::size_t(y)];
  }
};

// SourceStore over a BandSource (the GPU executor's streamed tiles ask it for one window per tile).
class BandSourceStore final : public engine::SourceStore {
 public:
  explicit BandSourceStore(BandSource& src) : src_(src), id_(next()) {}
  int width() const override { return src_.width(); }
  int height() const override { return src_.height(); }
  bool sceneReferred() const override { return true; }
  std::uint64_t identity() const override { return id_; }
  const float* window(int x0, int y0, int w, int h, std::size_t& pitchFloats) override;

 private:
  static std::uint64_t next();
  BandSource& src_;
  std::uint64_t id_;
};

// What the picture-wide analyses of an export need, each bit-identical to what it derives from the whole picture.
struct Wants {
  bool tone = true;    // resizeToLongEdge(full, 256): tonal analysis, the session's proxies (film match, modern white point)
  bool hdr = false;    // resizeToLongEdge(full, 512): single-image HDR field
  bool modern = false; // the modern rendering's end-point samples (engine::point::modernGridPixel)
  std::vector<int> longEdges;  // further reductions resizeToLongEdge(full, n) (Ultra HDR quarter renders, a session opened at a long edge)
  std::vector<std::pair<int, int>> guideGrids;  // raster refinement guide grids (local::guideGridSizes); non-empty also captures the 64 x 64 samples
  bool guide = false;
};
struct Analyses {
  bool shadow = false;  // the shadow-chroma floor applies (the BandSource was switched on)
  std::shared_ptr<SourceImage> p256, p512, grid;
  std::vector<std::pair<int, std::shared_ptr<SourceImage>>> reduced;  // (long edge, picture)
  std::shared_ptr<local::GuideCapture> guide;
  double ms = 0;
  // where the pass spent its time (stage log): window reads (demosaic + the band's copies), the row cache's copies, the shadow-floor samples, the reductions' rows, guide / grid
  double msWindow = 0, msKeep = 0, msLuma = 0, msFeed = 0, msGrid = 0;
  int passes = 0;
  std::shared_ptr<SourceImage> at(int longEdge) const {
    for (const auto& r : reduced)
      if (r.first == longEdge) return r.second;
    return nullptr;
  }
};
// One pass over the picture in full-width bands (two when the shadow floor applies: the decision needs the whole picture first). `meta` gives the
// metadata / scene flag of the reductions. progress(fraction) false = cancel. False on cancel / failure (`err`). With a row cache budget on `src`
// and no raster guide (whose area sums must run top to bottom) the pass runs bottom to top and leaves its top rows in the cache.
bool analyse(BandSource& src, const SourceImage& meta, const Wants& want, Analyses& out, const std::function<bool(float)>& progress, std::string* err);

// A SourceImage with the picture's size and metadata and no pixels (ImageF data empty): what a streamed render is handed.
SourceImage sizeOnly(const SourceImage& meta, int W, int H);

// The raster guide capture of `a` for the binds of this thread while alive (local::setGuideCapture).
struct GuideScope {
  explicit GuideScope(const local::GuideCapture* g) { local::setGuideCapture(g); }
  ~GuideScope() { local::setGuideCapture(nullptr); }
  GuideScope(const GuideScope&) = delete;
  GuideScope& operator=(const GuideScope&) = delete;
};

// The CPU streamed render: EngineSession::renderExact with the band hooks (TiledExport, `tile` rows per band; the source windows from `bs`;
// onSize(w, h) before the first band; rows(y0, n, rgb) per finished band). `point` reads its analyses from the Analyses
// (point::setAnalysisSources) and the guide capture is installed (GuideScope); `virt` = sizeOnly(...). False: failed / cancelled (`err`;
// "engine: plan not supported ..." = this edit cannot stream: the film branch).
bool renderBands(engine::EngineSession& es, const EditParams& params, const engine::point::Params& point, const SourceImage& virt, BandSource& bs, int tile, bool hasProxy,
                 const std::function<void(int, int)>& onSize, const std::function<bool(int, int, const std::uint8_t*)>& rows, std::string* err);

}  // namespace dapple::xstream
