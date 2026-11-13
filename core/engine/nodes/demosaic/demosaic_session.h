#pragma once
// Region-of-interest demosaic and the result cache of the Demosaic node (docs/engine/pipeline-architecture.md section 32).
//
//   MosaicCache: LRU, byte-budgeted, content addressed: key = file content hash + decode options (mosaic) and + algorithm version
//   (demosaiced result). Same semantics as engine::ResultCache (hit = no decompress / no demosaic; the entry is a shared_ptr so a
//   running consumer keeps it alive through an eviction). The graph-level registration of the node type is the hunk in the docs.
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>

#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic.h"

namespace dapple::demosaic {

constexpr std::uint32_t kAlgoVersion = 1;  // bump when the arithmetic of the node changes: invalidates cached demosaiced results

// A rectangle of the picture in ORIENTED, CROPPED coordinates (what the editor shows) -> the rectangle of the mosaic that holds it.
// False when OpcodeList3 (a warp) makes the mapping non-trivial: demosaic the whole frame then.
bool mosaicRectOf(const MosaicImage& m, int x0, int y0, int x1, int y1, Rect* out);
// Size of the oriented, cropped picture.
void orientedSize(const MosaicImage& m, int* w, int* h);

// Demosaic a rectangle of the oriented picture at full resolution (linear ProPhoto, clipped like the RGB path).
ImageF demosaicOrientedRegion(const MosaicImage& m, int x0, int y0, int x1, int y1, int threads = 0, const std::atomic<std::uint64_t>* cancelCur = nullptr, std::uint64_t cancelMine = 0);
// The whole picture: demosaic + OpcodeList3 + crop + orientation (the export path; GPU callers use the GPU variant of the same node).
ImageF demosaicFull(const MosaicImage& m, int threads = 0, std::string* corrections = nullptr);
// Half-size picture from the mosaic by 2 x 2 / 3 x 3 binning (no interpolation), the editor's proxy: same maths as LibRaw's half_size.
ImageF binnedHalf(const MosaicImage& m, int step = 1);  // step > 1: every step-th block in x and y only (statistics)
// The same, cropped (DNG default crop) and oriented like the full picture: the picture the exposure anchor is computed from (no OpcodeList3).
ImageF binnedOriented(const MosaicImage& m, int step = 1);
// X-Trans only (empty otherwise): the half-resolution reconstruction (Desc::half, docs 32.12), cropped and oriented like the full picture, from the same cached
// mosaic: 1/2 linear size (3876 px wide for 40 MP), as sharp as a half-size decode. GPU or CPU node by the measured verdict (a separate verdict kind).
ImageF halfOriented(const MosaicImage& m, int threads = 0, bool* usedGpu = nullptr);

// Streaming export (docs/engine/pipeline-architecture.md section 42): the node demosaicFull() would run for this mosaic (the GPU when a backend is installed,
// usable and preferred for the whole frame), and a region demosaic pinned to that choice, so that every band of one picture comes from the same node (no
// seam between a GPU band and a CPU band). A failed GPU run of a band falls back to the CPU node and sets *fellBack.
bool fullUsesGpu(const MosaicImage& m);
ImageF demosaicOrientedRegionOn(const MosaicImage& m, int x0, int y0, int x1, int y1, bool gpu, bool* fellBack = nullptr, int threads = 0);

struct CacheStats {
  std::uint64_t mosaicHits = 0, mosaicMisses = 0, fullHits = 0, fullMisses = 0, evictions = 0;
  std::size_t bytes = 0;
};

class MosaicCache {
 public:
  explicit MosaicCache(std::size_t budgetBytes = std::size_t(600) << 20) : budget_(budgetBytes) {}
  // Decompressed mosaic of a file, from the cache or decoded (decodeRawMosaic). `hit` reports which.
  Result<std::shared_ptr<const MosaicImage>> mosaic(const std::filesystem::path& path, const DecodeOptions& opt = {}, bool* hit = nullptr);
  // The demosaiced full picture of a cached mosaic (same key family): cached as long as the budget allows.
  std::shared_ptr<const ImageF> full(const std::shared_ptr<const MosaicImage>& m, int threads = 0, bool* hit = nullptr);
  CacheStats stats() const;
  void clear();
  // Drops every entry but the mosaic of key (k0, k1) (a streaming export frees what other photos left; entries in use stay alive through their
  // shared_ptr). Returns the bytes the cache no longer holds.
  std::size_t trimExcept(std::uint64_t k0, std::uint64_t k1);

 private:
  struct Entry {
    std::uint64_t k0, k1;
    int kind;  // 0 mosaic, 1 full
    std::size_t bytes;
    std::shared_ptr<const MosaicImage> mos;
    std::shared_ptr<const ImageF> img;
  };
  Entry* find(std::uint64_t k0, std::uint64_t k1, int kind);
  void insert(Entry e);
  mutable std::mutex mu_;
  std::list<Entry> lru_;  // front = most recent
  std::size_t budget_, bytes_ = 0;
  CacheStats st_;
};

}  // namespace dapple::demosaic
