#pragma once
// GpuImpl registration of the main nodes (step b2): which node runs which shaders, and the typed record entry the GPU
// executor (or, later, the Planner's stage list) calls. The CPU side of the same nodes is registered by the CPU engineer.
#include <span>

#include "engine/graph/node.h"
#include "engine/graph/types.h"

namespace dapple::engine::gpu {

// Null when the node has no GPU implementation (any plan containing it is unsupported on the GPU, docs 5.9).
const GpuImpl* gpuImplFor(NodeType t);

// Every shader name any registered node can use (self-test / precompile list). Binding signatures are checked by the HAL.
std::span<const char* const> allShaderNames();

}  // namespace dapple::engine::gpu
