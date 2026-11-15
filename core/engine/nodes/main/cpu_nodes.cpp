// The CPU implementation table of the main pipeline (docs/engine/pipeline-architecture.md 4.5) and the per-frame binding of resolved
// blocks. Every node family brings its CpuImpl (core/engine/nodes/<family>, docs section 50): the point family (nodes/point: the head's
// block fragments over point::CpuFrame, the table bakes), the film glue (FilmSim), Vignette, NR, Dehaze, Detail, Grain, the local
// adjustments, Resize. This file lists them, binds their resolved slot (bindResolved: the family's block from FrameEnv::blocks, or the
// frame's point::CpuFrame) and implements Pack8. Nothing else is implemented here.
//
// Block fragment contract (CpuImpl::block): `px` = n pixels of interleaved float RGB (3 floats, not float4), n <= 256,
// processed in place; `pos` = frame coordinates of the first pixel. The head of a chain (Resize/Geometry/DisplayStages in
// replace mode) ignores the incoming values and produces them.
#include <cstring>
#include <vector>

#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "engine/nodes/resize/resize.h"
#include "parallel.h"

namespace dapple::engine {

namespace {

// ---- tile (window / whole-frame) nodes -------------------------------------------------------------------------------------
// Pack8: clamp-free 8-bit encode of the interior of the tile (the chain already clamped to [0, 1])
Status tilePack8(CpuTile& t, const void* r) {
  (void)r;
  const ImageF& img = *t.img;
  const Rect& in = t.interior;
  const int ch = t.outChannels;
  {
    const int rows = in.y1 - in.y0;
    ThreadPool::instance().run(rows, 16, [&](int a, int b) {
      for (int yy = a; yy < b; ++yy) {
        const int y = in.y0 + yy;
        const float* s = img.row(y) + 3 * std::size_t(in.x0);
        std::uint8_t* o = t.out8 + (std::size_t(t.oy + y - t.outRow0) * t.frameW + std::size_t(t.ox + in.x0)) * ch;
        const int n = in.x1 - in.x0;
        for (int x = 0; x < n; ++x) {
          o[ch * x] = std::uint8_t(s[3 * x] * 255.f + 0.5f);
          o[ch * x + 1] = std::uint8_t(s[3 * x + 1] * 255.f + 0.5f);
          o[ch * x + 2] = std::uint8_t(s[3 * x + 2] * 255.f + 0.5f);
          if (ch == 4) o[4 * x + 3] = 255;
        }
      }
    });
  }
  return Status::Ok;
}

// Resize: the bake is the family's (nodes/resize: the whole source at a long edge); the chain's head copies rows of the frame's source
const CpuImpl kResize{nullptr, point::copyBlock, nullptr, resize::bake, resize::freePayload};
const CpuImpl kPack8{nullptr, nullptr, tilePack8, nullptr};

}  // namespace

const CpuImpl* cpuFusedHead() { return &point::fusedHeadCpu(); }

const CpuImpl* cpuImpl(NodeType t) {
  switch (t) {
    case NodeType::Source: return &point::sourceCpu();
    case NodeType::Resize: return &kResize;
    case NodeType::Geometry: return &point::geometryCpu();
    case NodeType::Gain: return &point::gainCpu();
    case NodeType::Hdr: return &point::hdrCpu();
    case NodeType::ToneColour:
    case NodeType::ToneColourExact: return &point::toneColourCpu();
    case NodeType::Vignette: return &vignette::cpu();
    case NodeType::LocalLinear: return &localadj::linearCpu();
    case NodeType::LocalDetail: return &localadj::detailCpu();
    case NodeType::DisplayStages: return &point::displayCpu();
    case NodeType::Blend: return &point::blendCpu();
    case NodeType::NRLuma: return &nr::lumaCpu();
    case NodeType::NRChroma: return &nr::chromaCpu();
    case NodeType::Dehaze: return &dehaze::cpu();
    case NodeType::Detail: return &detail::cpu();
    case NodeType::Grain: return &grain::cpu();
    case NodeType::FilmSim: return &point::filmSimCpu();
    case NodeType::Pack8: return &kPack8;
    case NodeType::ColourLattice: return &point::latticeCpu();
    case NodeType::ToneAnalysisPre: return &point::tonePreCpu();
    case NodeType::ToneTable: return &point::toneTableCpu();
    case NodeType::Analysis: return &point::analysisCpu();
    case NodeType::LatticeOverflow: return &point::overflowCpu();
    default: return nullptr;  // AssembledBake has no plan
  }
}

bool bindResolved(NodeType t, const FrameEnv& env, Resolved& out) {
  switch (t) {
    case NodeType::DisplayStages:
      new (out.bytes) point::DisplayCpu{env.fk, env.film, !env.blendFollows, env.filmRow0};
      return true;
    case NodeType::NRLuma:
      if (!env.blocks) return false;
      new (out.bytes) nr::LumaBlock(env.blocks->nrLuma);
      return true;
    case NodeType::NRChroma:
      if (!env.blocks) return false;
      new (out.bytes) nr::ChromaBlock(env.blocks->nrChroma);
      return true;
    case NodeType::Grain:
      if (!env.blocks) return false;
      new (out.bytes) grain::Block(env.blocks->grain);
      return true;
    case NodeType::Dehaze:
      if (!env.blocks) return false;
      new (out.bytes) dehaze::Block(env.blocks->dehaze);
      return true;
    case NodeType::Detail:
      if (!env.blocks) return false;
      new (out.bytes) detail::Block(env.blocks->detail);
      return true;
    case NodeType::LocalDetail:
      if (!env.blocks) return false;
      new (out.bytes) localadj::Block(env.blocks->local);  // resolved after the film outcome is known (radii scale)
      return true;
    case NodeType::Pack8: return true;
    case NodeType::Vignette:
      if (!env.blocks) return false;
      new (out.bytes) vignette::Block(env.blocks->vignette);
      return true;
    case NodeType::LocalLinear:
      if (!env.blocks) return false;
      new (out.bytes) localadj::LinearCpu{env.blocks->local.frame, env.fk, env.aux};
      return true;
    default:
      if (!cpuImpl(t) || (!cpuImpl(t)->block && !cpuImpl(t)->tile)) return false;  // table nodes bake, they are not bound per frame
      new (out.bytes) point::FrameSlot{env.fk};
      return true;
  }
}

}  // namespace dapple::engine
