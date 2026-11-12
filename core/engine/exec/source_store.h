#pragma once
// SourceStore: where an executor gets the full-resolution source pixels from (docs/engine/pipeline-architecture.md 5.6, 5.9
// "resident data is reconstructible from SourceStore"). The GPU executor keeps a device copy (RGBA16F) of what it reads here
// and re-creates it after a device loss; a streaming export asks for one window per tile and never holds the whole picture.
//
// MemorySourceStore is the in-memory implementation (the decoded SourceImage of the session). A file backed / tiled store
// (out-of-core RAW, 100 MP) implements the same three calls.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "dapple/decoder.h"

namespace dapple::engine {

class SourceStore {
 public:
  virtual ~SourceStore() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual bool sceneReferred() const = 0;
  // Changes whenever the pixels (or their geometry) change; executors key their resident copies on it.
  virtual std::uint64_t identity() const = 0;
  // Linear ProPhoto float RGB of the window [x0, x0 + w) x [y0, y0 + h), `pitchFloats` floats between rows. The pointer stays
  // valid until the next call on this store. Null = the window cannot be read (the executor reports a transient failure).
  virtual const float* window(int x0, int y0, int w, int h, std::size_t& pitchFloats) = 0;
  // The CPU image when the store is memory backed (bakes of the table nodes read it); null otherwise.
  virtual const SourceImage* image() const { return nullptr; }
};

class MemorySourceStore final : public SourceStore {
 public:
  explicit MemorySourceStore(const SourceImage& img) : img_(img), id_(next()) {}
  int width() const override { return img_.linear.width; }
  int height() const override { return img_.linear.height; }
  bool sceneReferred() const override { return img_.sceneReferred; }
  std::uint64_t identity() const override { return id_; }
  const float* window(int x0, int y0, int w, int h, std::size_t& pitchFloats) override {
    if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0 || x0 + w > img_.linear.width || y0 + h > img_.linear.height) return nullptr;
    pitchFloats = std::size_t(img_.linear.width) * 3;
    return img_.linear.data.data() + (std::size_t(y0) * img_.linear.width + std::size_t(x0)) * 3;
  }
  const SourceImage* image() const override { return &img_; }
  // The pixels were replaced in place (HDR merge result stored in the same SourceImage): resident copies are stale.
  void touch() { id_ = next(); }

 private:
  static std::uint64_t next() {
    static std::atomic<std::uint64_t> n{1};
    return n.fetch_add(1);
  }
  const SourceImage& img_;
  std::uint64_t id_;
};

}  // namespace dapple::engine
