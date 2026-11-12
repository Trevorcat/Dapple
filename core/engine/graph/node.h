#pragma once
// Per-executor implementation tables of a node (docs/engine/pipeline-architecture.md 4.2). STABLE IR header.
//
// A node = NodeDesc (graph.h, what it reads / how it connects) + optional CpuImpl + optional GpuImpl registered next to it.
// A node whose impl pointer is null makes any plan containing it unsupported on that executor.
#include <cstddef>
#include <span>

#include <atomic>
#include <cstdint>

#include "engine/graph/params.h"
#include "engine/graph/types.h"

namespace dapple::engine {

struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };      // half-open, pixels of the node's own grid
struct Halo { int x = 0, y = 0; };                         // symmetric neighbourhood read by a node
struct PixelPos { int x = 0, y = 0; };                     // absolute (frame) coordinates of the first pixel of a block

enum class Status : std::uint8_t { Ok, Unsupported, OutOfMemory, Cancelled, Internal };

// Window into a planner-owned buffer (float, interleaved `channels`).
struct ImageView {
  float* data = nullptr;
  int width = 0, height = 0, channels = 3;
  std::size_t strideFloats = 0;      // row pitch
  Rect origin;                       // where this window sits in the node's grid
  // Additive (film nodes): non-zero = planar layout, channel c lives at data + c * planeStrideFloats (strideFloats is then
  // the row pitch of one plane). 0 = interleaved. CPU `Planar` logical images (4.1).
  std::size_t planeStrideFloats = 0;
};

struct CpuTile;                    // exec/cpu_executor.h: an ImageF window of the frame + per-frame solves (additive, CPU engineer)

struct CpuCtx {
  Rect frame;                        // the whole frame in the node's grid: boundary policy applies against THIS, not the tile
  const std::atomic<std::uint64_t>* cancel = nullptr;
  std::uint64_t ticket = 0;
  // Additive: payloads of the node's Table inputs, in edge order (the ResultCache payload pointers). Image nodes that read
  // tables (film) find them here.
  std::span<const void* const> tables;
};

struct CpuImpl {
  // Whole-tile entry. `resolved` is the node's POD parameter block (NodeDesc::resolve, <= 256 B, std140 compatible).
  Status (*run)(const CpuCtx&, const void* resolved, std::span<const ImageView> in, std::span<const ImageView> out) = nullptr;
  // Point nodes only: block fragment for fused streaming (n <= 256 float4-AoS pixels, in place, absolute position given).
  void (*block)(const void* resolved, float* px, int n, const PixelPos& pos) = nullptr;
  // Window / global-part nodes (additive): process one tile (an ImageF window of the frame) in place. The tile carries its
  // origin in the frame and the frame-global solves; `resolved` is bound per frame by nodes/main/cpu_nodes.cpp.
  Status (*tile)(CpuTile&, const void* resolved) = nullptr;
  // Table nodes: produce the table payload (opaque to the planner; stored in the ResultCache under the node key).
  Status (*bake)(const void* resolved, std::span<const void* const> inputs, void** payloadOut) = nullptr;
  // Additive: destroys a payload produced by `bake` (the executor wraps it in a shared_ptr<void> with this deleter).
  void (*freePayload)(void*) = nullptr;
};

struct GpuRecorder;                  // defined by the GPU executor (b2)
namespace gpu {                      // additive (GPU engineer, step b2): the executor's own types, see engine/nodes/gpu_common.h
struct GpuRecorder;
struct GpuPorts;
}  // namespace gpu
struct GpuImpl {
  Status (*record)(GpuRecorder&, const void* resolved, std::span<const ImageView> in, std::span<const ImageView> out) = nullptr;
  std::span<const char* const> shaders;
  // Additive: the typed entry the GPU executor calls (input/output ports are graph handles of the executor's GpuGraph, not
  // CPU ImageViews). `resolved` is the node's GPU parameter block (nodes/gpu_common.h *Node structs). Appends dispatches and
  // returns false (recorder error set) on failure.
  bool (*recordPorts)(gpu::GpuRecorder&, const void* resolved, const gpu::GpuPorts&) = nullptr;
  // Name of the fused kernel this node is a member of (point recipe: Geometry + Gain + ToneColour + Vignette), or null.
  const char* recipe = nullptr;
  // Grain / Pack8: no pass of their own, they are the epilogue slot of the last pass of the chain.
  bool epilogue = false;
};

// Resolution of the ParamSet into the numbers kernels need. Pure function; output is POD.
struct FrameGeom { int w = 0, h = 0; float scale = 1.f; Rect region; };
struct NodeBehaviour {
  std::size_t resolvedBytes = 0;
  void (*resolve)(const ParamSet&, const FrameGeom&, void* out) = nullptr;
  bool (*isIdentity)(const void* resolved) = nullptr;       // true: node is elided from the plan
  Halo (*halo)(const void* resolved) = nullptr;             // Point: always {0,0}
  Rect (*inputRect)(const void* resolved, const Rect& outRect) = nullptr;   // Resample only
};

}  // namespace dapple::engine
