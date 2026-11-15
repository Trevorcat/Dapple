// The GPU implementation table: generated from the registration list (engine/nodes/node_list.inc); each family defines its GpuImpl in its
// own gpu.cpp.
#include "engine/nodes/gpu_nodes.h"

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {
const char* const kAll[] = {"point",     "sep_d2y",  "sep_d2y_v", "sep_d2y_s", "sep_c2y", "sep_c2y_v", "sep_c2y_s", "sep_c",  "sep_c_s",
                            "sep_y",     "sep_y_v",  "sep_y_s",   "down",      "nr_luma", "nr_chroma", "dehaze_apply", "reduce", "gf_ab",
                            "detail",    "resize",   "convert",   "bx_d2y",    "bx_y",    "bx_y_v", "point_local", "local_detail", "local_film_in",
                            "local_heal", "local_rt_down", "local_retouch"};
}  // namespace

const GpuImpl* noGpu() { return nullptr; }

const GpuImpl* gpuImplFor(NodeType t) {
  static const GpuImpl* const table[kNodeTypeCount] = {
#define DAPPLE_NODE(type, desc, gpu) gpu(),
#include "engine/nodes/node_list.inc"
#undef DAPPLE_NODE
  };
  return std::size_t(t) < std::size_t(kNodeTypeCount) ? table[std::size_t(t)] : nullptr;
}

std::span<const char* const> allShaderNames() { return kAll; }

}  // namespace dapple::engine::gpu
