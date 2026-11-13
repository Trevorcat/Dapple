#pragma once
// Streaming (band) export: docs/engine/pipeline-architecture.md section 42. Internal header (core/src).
//
// A full-resolution export of a large RAW never holds the picture whole: the session keeps the cached 16-bit mosaic (LazyFull), the picture-wide
// analyses come from exact reductions built band by band in one pass (analyse()), and the render runs in bands whose source pixels are demosaiced
// on demand from the mosaic (BandSource), straight into the JPEG strip encoder. Peak memory is the mosaic + a few bands + the reductions, whatever
// the picture size.
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

// Bytes an export may still allocate on this device: the process's own headroom where the OS reports it (iOS os_proc_available_memory), else a
// conservative share of the memory available to the system (Android / Linux MemAvailable * 0.5, desktop * 0.5), minus a reserve for the app.
// DAPPLE_EXPORT_BUDGET_MB overrides (tests).
std::size_t budgetBytes();
// What the whole-frame path adds on top of the session for a W x H RAW picture: the float source (12 B/px), the 8-bit output (3 B/px), the
// executor tiles and the encoder (~4 B/px); a film edit adds its frame planes (~55 B/px).
std::size_t predictWholePeak(int W, int H, bool film);
// At a FULL open: open lazily (keep the mosaic, no float picture)? `why` says on what grounds (log line).
bool openLazy(int W, int H, std::string* why = nullptr);

// The oriented, cropped full-resolution picture of a cached mosaic, read in windows: the same Demosaic node the full decode would run (pinned for
// the whole picture), then the shadow-chroma floor when the picture-wide decision says so. One window is kept (valid until the next call).
class BandSource {
 public:
  explicit BandSource(std::shared_ptr<const MosaicImage> mosaic);
  int width() const { return W_; }
  int height() const { return H_; }
  bool gpu() const { return gpu_; }
  bool fellBack() const { return fellBack_; }
  void setShadowFloor(bool on) { shadow_ = on; }
  bool shadowFloor() const { return shadow_; }
  // [x0, x1) x [y0, y1) (must lie inside the picture). Null when cancelled (cancel != null and *cancel != mine).
  const ImageF* window(int x0, int y0, int x1, int y1);
  void setCancel(const std::atomic<std::uint64_t>* cur, std::uint64_t mine) { cancelCur_ = cur, cancelMine_ = mine; }
  std::uint64_t windows() const { return windows_; }
  double demosaicMs() const { return ms_; }
  std::size_t peakWindowBytes() const { return peakBytes_; }
  void release() { ImageF().data.swap(win_.data), win_ = ImageF(); }

 private:
  std::shared_ptr<const MosaicImage> m_;
  int W_ = 0, H_ = 0;
  bool gpu_ = false, fellBack_ = false, shadow_ = false;
  ImageF win_;
  const std::atomic<std::uint64_t>* cancelCur_ = nullptr;
  std::uint64_t cancelMine_ = 0, windows_ = 0;
  double ms_ = 0;
  std::size_t peakBytes_ = 0;
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
  bool modern = false; // the modern rendering's end-point samples (Renderer::modernGridPixel)
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
  int passes = 0;
  std::shared_ptr<SourceImage> at(int longEdge) const {
    for (const auto& r : reduced)
      if (r.first == longEdge) return r.second;
    return nullptr;
  }
};
// One pass over the picture in full-width bands (two when the shadow floor applies: the decision needs the whole picture first). `meta` gives the
// metadata / scene flag of the reductions. progress(fraction) false = cancel. False on cancel / failure (`err`).
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
// onSize(w, h) before the first band; rows(y0, n, rgb) per finished band). `renderer` reads its analyses from the Analyses
// (Renderer::setAnalysisSources) and the guide capture is installed (GuideScope); `virt` = sizeOnly(...). False: failed / cancelled (`err`;
// "engine: plan not supported ..." = this edit cannot stream: the film branch).
bool renderBands(engine::EngineSession& es, const EditParams& params, const Renderer& renderer, const SourceImage& virt, BandSource& bs, int tile, bool hasProxy,
                 const std::function<void(int, int)>& onSize, const std::function<bool(int, int, const std::uint8_t*)>& rows, std::string* err);

}  // namespace dapple::xstream
