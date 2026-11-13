#include "engine/nodes/gpu_nodes.h"

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

namespace {

bool recPoint(GpuRecorder& r, const void* res, const GpuPorts& p) { return recordPoint(r, *static_cast<const PointNode*>(res), p.out[0]); }
bool recNrLuma(GpuRecorder& r, const void* res, const GpuPorts& p) { return recordNrLuma(r, *static_cast<const NrLumaNode*>(res), p.in[0]); }
bool recNrChroma(GpuRecorder& r, const void* res, const GpuPorts& p) { return recordNrChroma(r, *static_cast<const NrChromaNode*>(res), p.in[0]); }
bool recDehaze(GpuRecorder& r, const void* res, const GpuPorts& p) { return recordDehazeApply(r, *static_cast<const DehazeNode*>(res), p.in[0]); }
bool recDetail(GpuRecorder& r, const void* res, const GpuPorts& p) { return recordDetail(r, *static_cast<const DetailNode*>(res), p.in[0]); }

const char* const kPoint[] = {"point"};
const char* const kNrL[] = {"nr_luma", "sep_d2y", "sep_d2y_s", "sep_y", "sep_y", "sep_y_s", "sep_y_v", "bx_d2y", "bx_y", "bx_y_v"};
const char* const kNrC[] = {"down", "nr_chroma", "sep_c", "sep_c_s"};
const char* const kDz[] = {"sep_c2y", "sep_c2y_s", "sep_y", "sep_y_s", "sep_y_v", "bx_y", "bx_y_v", "reduce", "dehaze_apply"};
const char* const kDetail[] = {"down", "gf_ab", "sep_c", "sep_c_s", "sep_d2y", "sep_d2y_s", "sep_y", "sep_y_s", "sep_y_v", "bx_d2y", "bx_y", "bx_y_v", "detail"};
const char* const kResize[] = {"resize", "convert"};
const char* const kAll[] = {"point",     "sep_d2y",  "sep_d2y_v", "sep_d2y_s", "sep_c2y", "sep_c2y_v", "sep_c2y_s", "sep_c",  "sep_c_s",
                            "sep_y",     "sep_y_v",  "sep_y_s",   "down",      "nr_luma", "nr_chroma", "dehaze_apply", "reduce", "gf_ab",
                            "detail",    "resize",   "convert",   "bx_d2y",    "bx_y",    "bx_y_v"};

const GpuImpl kPointImpl = {nullptr, kPoint, recPoint, "point", false};
const GpuImpl kNrLImpl = {nullptr, kNrL, recNrLuma, nullptr, false};
const GpuImpl kNrCImpl = {nullptr, kNrC, recNrChroma, nullptr, false};
const GpuImpl kDzImpl = {nullptr, kDz, recDehaze, nullptr, false};
const GpuImpl kDetailImpl = {nullptr, kDetail, recDetail, nullptr, false};
const GpuImpl kResizeImpl = {nullptr, kResize, nullptr, nullptr, false};  // recordResize needs a source window: called by the executor
const GpuImpl kEpilogueImpl = {nullptr, {}, nullptr, "epilogue", true};

}  // namespace

const GpuImpl* gpuImplFor(NodeType t) {
  switch (t) {
    // the point recipe: one pass for Geometry + Gain + ToneColour (+ the Exact variant) + Vignette
    case NodeType::Geometry:
    case NodeType::Gain:
    case NodeType::Hdr:
    case NodeType::ToneColour:
    case NodeType::ToneColourExact:  // the exact variant runs on the same lattice kernel (a denser bake), see docs/engine/pipeline-architecture.md 21
    case NodeType::Vignette: return &kPointImpl;
    case NodeType::NRLuma: return &kNrLImpl;
    case NodeType::NRChroma: return &kNrCImpl;
    case NodeType::Dehaze: return &kDzImpl;
    case NodeType::Detail: return &kDetailImpl;
    case NodeType::Resize: return &kResizeImpl;
    case NodeType::Grain:
    case NodeType::Pack8: return &kEpilogueImpl;
    default: return nullptr;  // tables are baked on the CPU and uploaded; Source is the resident image; film nodes: b4
  }
}

std::span<const char* const> allShaderNames() { return kAll; }

}  // namespace dapple::engine::gpu
