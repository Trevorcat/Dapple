#pragma once
// RAW mosaic output of the decoder (docs/engine/pipeline-architecture.md section 31): the sensor samples before any demosaic, with
// everything the Demosaic node needs to turn them into linear ProPhoto RGB. The RGB path of ImageDecoder::decode stays as the oracle
// and the fallback; this is the "decompress only" half of it.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "dapple/decoder.h"
#include "dapple/metadata.h"
#include "dapple/result.h"

namespace dapple {

struct MosaicImage {
  int width = 0, height = 0;           // the file's active area, sensor orientation (the decode crops to cropX.. after the demosaic)
  std::vector<std::uint16_t> data;     // width * height raw samples (after DNG OpcodeList1 / 2 where they act on the mosaic)
  CfaKind cfa = CfaKind::None;         // Bayer (2x2), XTrans (6x6) or None: no mosaic, `reason` says why, use ImageDecoder::decode
  int period = 2;                      // 2 or 6
  std::uint8_t cfaIdx[36] = {};        // colour of cell (y % period, x % period) of `data`: 0 R, 1 G, 2 B, 3 second green (Bayer)
  int blackRows = 1, blackCols = 1;    // black level table, repeat pattern over `data`
  std::vector<float> black;            // total black level (common + per colour + repeat pattern) of each cell
  float white = 65535.f;               // sensor clipping level (LibRaw `maximum`, as the RGB path uses it)
  float blackMin = 0.f;                // the common black LibRaw subtracts from `white`: span = white - blackMin
  float wb[4] = {1.f, 1.f, 1.f, 1.f};  // as-shot multipliers per cfaIdx (R, G, B, G2), the smallest is 1 (what LibRaw scales by)
  float camToWork[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // white-balanced camera RGB -> linear ProPhoto (LibRaw's rgb_cam folded with the output profile)
  int flip = 0;                        // LibRaw / EXIF orientation code of the file (0, 3, 5, 6); the mosaic is NOT rotated
  bool hasCrop = false;                // DNG default crop in active-area pixels (applied after the demosaic and OpcodeList3)
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
  std::vector<std::uint8_t> opcodeList3;  // raw DNG OpcodeList3 payload (WarpRectilinear ...), applies to the demosaiced image; empty = none
  float exposureGain = 1.f;            // 1 for camera files
  ShotMetadata meta;                   // as ImageDecoder::decode fills it, except the exposure anchor (needs RGB: attach from the proxy)
  float dngBaselineEv = std::numeric_limits<float>::quiet_NaN();  // DNG BaselineExposure (+ offset), NaN = none (input of the exposure anchor)
  bool fixedBadPixels = false;         // the DNG opcodes already fixed bad pixels on the mosaic: the optional hot-sample filter stays off
  std::string dngCorrections;          // opcodes applied to the mosaic
  std::string reason;                  // cfa == None / unsupported: why
  std::uint64_t key[2] = {0, 0};       // content hash of the file + the decode options (head, tail, size): the cache key
};

// 128-bit hash of the file content (size + 64 KB head + 64 KB tail + the option bytes): the ResultCache key of the mosaic and of the
// demosaiced result. Independent of path and mtime (a copied file keeps its cached results).
void fileContentKey(const std::filesystem::path& path, std::uint64_t out[2]);

// Decompress and return the mosaic (no demosaic, no colour conversion). Returns cfa == None (and `reason`) for files that need no
// demosaic or that the mosaic path does not cover (sRAW / mRAW, monochrome, linear DNG, JPEG XL DNG, four-colour sensors, Fuji
// rotated EXR sensors, formats LibRaw delivers only as 4-channel data): the caller then uses ImageDecoder::decode.
Result<MosaicImage> decodeRawMosaic(const std::filesystem::path& path, const DecodeOptions& options = {});

// OpcodeList3 (WarpRectilinear, ...) of the mosaic image, then the default crop, then the file orientation, on a full-frame RGB image
// (ProPhoto, the output of the Demosaic node for the whole active area). Same order as the RGB path.
ImageF finishMosaicRgb(const MosaicImage& m, ImageF rgb, std::string* corrections = nullptr);

// The same SourceImage that ImageDecoder::decode(path, {Full}) produces (linear ProPhoto, oriented, cropped, shadow chroma floor, exposure anchor), but through
// the mosaic: LibRaw only decompresses, the Demosaic node (GPU when the C API installed a backend and the runtime switch is on, else the CPU node) does the
// rest. The mosaic comes from a process-wide MosaicCache (re-opening a photo does not decompress again). On refusal (no mosaic path for this file, switch
// DAPPLE_DEMOSAIC=off) the result is an error and `reason` says why: the caller takes the RGB path.
Result<SourceImage> decodeFullFromMosaic(const std::filesystem::path& path, const DecodeOptions& options, std::string* reason = nullptr);

// A rectangle (x0, y0, x1, y1) of the oriented, cropped full-resolution picture at 1:1, from the cached mosaic. The exposure anchor is the one of the whole
// picture (cached per file) so a detail region renders at the same exposure as the proxy. Refuses files with an OpcodeList3 warp (use the full decode).
Result<SourceImage> decodeRawRegion(const std::filesystem::path& path, int x0, int y0, int x1, int y1, const DecodeOptions& options, std::string* reason = nullptr,
                                    const std::atomic<std::uint64_t>* cancelCur = nullptr, std::uint64_t cancelMine = 0);

// Drops the process-wide mosaic cache (memory pressure, tests).
void clearMosaicCache();
// Entries the cache served without decoding / decoded (tests).
void mosaicCacheCounters(unsigned long long* hits, unsigned long long* misses);

}  // namespace dapple
