#pragma once
// Local adjustments node family (LocalLinear, stage L; LocalDetail, stage W; docs/engine/pipeline-architecture.md sections 40 and 50).
// bind(): the adjustment list of the edit resolved against the source (masks, plane caches; local::bind) — owned by whoever holds the
// frame's source (point::CpuFrame on the CPU, the session's GPU frame). resolve(): the per-frame block both executors read (the bound list,
// the LocalDetail radii and amount planes, the frame pixel -> photo metric mapping of the portrait retouch). Kernels: core/src/local_eval
// (CPU), local/gpu.cpp + shaders/local*.comp (GPU).
#include <memory>

#include "dapple/edit_params.h"
#include "engine/graph/node.h"
#include "engine/nodes/frame_facts.h"

namespace dapple::local { struct Frame; struct AuxTarget; struct PixelSource; }  // core/src/local_eval.h
namespace dapple::warp { struct Field; }  // core/src/warp_field.h
namespace dapple { struct SourceImage; }  // dapple/decoder.h

namespace dapple::engine::localadj {

struct Block {
  const local::Frame* frame = nullptr;  // the bound list (null: no local adjustment); owner: the frame's binder
  const warp::Field* warp = nullptr;    // portrait reshaping field: the heal spots follow it
  float scale = 1.f;                    // radii scale (FrameFacts::spatialScale)
  bool detail = false;                  // stage W runs (LocalDetail node)
  int halo = 0;                         // LocalDetail neighbourhood in frame pixels (local::detailRadii), multiple of 4
  int auxPlanes = 0;                    // float planes of stage-W amounts the point chain writes (local::auxLayout)
  bool retouch = false;                 // portrait retouch / heal spots: `photo` is set
  double photo[6] = {1, 0, 0, 0, 1, 0};  // frame pixel -> photo metric (local::Affine, local::frameAffine)
  bool active() const { return frame != nullptr; }
};

std::shared_ptr<const local::Frame> bind(const EditParams& p, const SourceImage& src, bool interactive);
Block resolve(const local::Frame* f, const warp::Field* wf, const FrameFacts& facts);

// The CPU binding of LocalLinear: the block's list, the frame's geometry and the executor's amount planes of the work window.
struct LinearCpu {
  const local::Frame* frame;
  const local::PixelSource* src;
  const local::AuxTarget* aux;
};
const NodeBehaviour& linearBehaviour();
const NodeBehaviour& detailBehaviour();
const CpuImpl& linearCpu();  // resolved slot: LinearCpu
const CpuImpl& detailCpu();  // resolved slot: Block

}  // namespace dapple::engine::localadj
