#pragma once
// What a node is besides its descriptor (docs/engine/pipeline-architecture.md 4.2, section 50): behaviour over its resolved block,
// CPU implementation, GPU implementation. STABLE IR header.
//
// A node = NodeDesc (graph.h: ports, parameter mask, colour spaces, footprint class, fusion group, behaviour, block accessor, CPU
// implementation + binding, wiring) + an optional GpuImpl (GPU library). Both are registered by one line of engine/nodes/node_list.inc.
// A node whose implementation for an executor is null makes any plan containing it unsupported on that executor.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "engine/graph/params.h"
#include "engine/graph/types.h"

namespace dapple::engine {

struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };      // half-open, pixels of the node's own grid
struct Halo { int x = 0, y = 0; };                         // symmetric neighbourhood read by a node
struct PixelPos { int x = 0, y = 0; };                     // absolute (frame) coordinates of the first pixel of a block

enum class Status : std::uint8_t { Ok, Unsupported, OutOfMemory, Cancelled, Internal };

// The 1/4 grid a frame-global solve is made from (accumulated by the sweep before it, from the image at the solving node's input).
//   ColourGrid  area-averaged RGB (dehaze: dark channel + airlight)     LumaGrid  area-averaged (Y, Y^2) (guided-filter coefficients)
enum class Accum : std::uint8_t { None, ColourGrid, LumaGrid };

// Window into a planner-owned buffer (float, interleaved `channels`).
struct ImageView {
  float* data = nullptr;
  int width = 0, height = 0, channels = 3;
  std::size_t strideFloats = 0;      // row pitch
  Rect origin;                       // where this window sits in the node's grid
  // Non-zero = planar layout, channel c lives at data + c * planeStrideFloats (strideFloats is then the row pitch of one plane).
  std::size_t planeStrideFloats = 0;
};

struct CpuTile;                    // exec/cpu_executor.h: an ImageF window of the frame + the node's frame-global solve
struct FrameEnv;                   // exec/cpu_executor.h: what a node binds its CPU slot to for one frame
struct Resolved;                   // exec/cpu_executor.h: a node's CPU slot (<= kMaxResolvedBytes)

struct CpuCtx {
  Rect frame;                        // the whole frame in the node's grid: boundary policy applies against THIS, not the tile
  const std::atomic<std::uint64_t>* cancel = nullptr;
  std::uint64_t ticket = 0;
  std::span<const void* const> tables;  // payloads of the node's Table inputs, in edge order
};

struct CpuImpl {
  // Whole-image entry (film nodes; runFilmFrame composes them).
  Status (*run)(const CpuCtx&, const void* resolved, std::span<const ImageView> in, std::span<const ImageView> out) = nullptr;
  // Head nodes: block fragment for fused streaming (n <= 256 interleaved float RGB pixels, in place, absolute position given).
  void (*block)(const void* resolved, float* px, int n, const PixelPos& pos) = nullptr;
  // Window / sink nodes: process one tile (an ImageF window of the frame) in place.
  Status (*tile)(CpuTile&, const void* resolved) = nullptr;
  // Table nodes: produce the table payload (opaque to the planner; stored in the ResultCache under the node key).
  Status (*bake)(const void* resolved, std::span<const void* const> inputs, void** payloadOut) = nullptr;
  void (*freePayload)(void*) = nullptr;
  // Nodes with a frame-global solve (NodeDesc::solve): the solve from the accumulated 1/4 grid (gw x gh, channels by Accum). The payload
  // is cached under the node key and handed to `tile` as CpuTile::solved; *bytes = its size.
  std::shared_ptr<void> (*solve)(const std::vector<float>& grid, int gw, int gh, const void* resolved, std::size_t* bytes) = nullptr;
  // The lead node of a head chain may fuse it: given the chain's node types (starting with itself), returns the fused fragment and how many
  // nodes it replaces (0: no fusion). The fused fragment binds the lead node's slot.
  const CpuImpl* (*fuse)(const FrameEnv&, std::span<const NodeType> chain, std::size_t* consumed) = nullptr;
};

namespace gpu {                      // the GPU executor's own types (engine/nodes/gpu_common.h)
struct GpuRecorder;
struct NodeCall;
struct GroupCall;
}  // namespace gpu
// The GPU implementation of a node. Nodes of one fusion group share a recipe: the group is recorded once, by the impl of highest `rank`
// among its nodes (`group`); a window node records itself (`node`). Epilogue nodes (Grain, Pack8) have no pass: they are the epilogue slot
// of the last pass. Film nodes are recorded by the film stage as a whole (`film`).
struct GpuImpl {
  std::span<const char* const> shaders;
  int (*group)(gpu::GpuRecorder&, const gpu::GroupCall&) = nullptr;  // fused group: returns the new working image, -1 on error
  int (*node)(gpu::GpuRecorder&, const gpu::NodeCall&) = nullptr;    // single node: returns the new working image, -1 on error
  // frame-global solve from the accumulated 1/4 grid (gw x gh): returns the solved image, -1 on error; `solvedRgba` its format (else R32F)
  int (*solve)(gpu::GpuRecorder&, const void* block, int grid, int gw, int gh) = nullptr;
  bool solvedRgba = false;
  int rank = 0;
  bool epilogue = false;
  bool film = false;
  bool lattice = false;   // reads the colour lattice / tone table (the device tables are uploaded for the frame)
};

struct NodeDesc;                   // graph.h

// What a node's resolved block is read from (resolved per frame by the session / point::CpuFrame).
struct FrameBlocks;
namespace point { struct Blocks; }
struct FrameView {
  const point::Blocks* point = nullptr;
  const FrameBlocks* blocks = nullptr;
};

// Behaviour of a node over its resolved block (NodeDesc::block). Pure functions.
struct NodeBehaviour {
  std::size_t resolvedBytes = 0;
  bool (*isIdentity)(const void* resolved) = nullptr;       // true: the node is not wired / elided from the plan
  Halo (*halo)(const void* resolved) = nullptr;             // Window: pixels read around an output pixel (both executors)
  Rect (*inputRect)(const void* resolved, const Rect& outRect) = nullptr;   // Resample: input pixels an output rect reads
};

}  // namespace dapple::engine
