// The point family's nodes (docs/engine/pipeline-architecture.md section 50): descriptors, CPU slot bindings and the 8-bit sink.
//   image   Source, Geometry, Gain, Hdr, ToneColour, ToneColourExact, DisplayStages, Blend, Pack8
//   tables  ColourLattice, LatticeOverflow, ToneAnalysisPre, ToneTable, Analysis (bakes in bake.cpp)
#include <cstring>
#include <new>

#include "engine/exec/cpu_executor.h"
#include "engine/graph/param_groups.h"
#include "engine/nodes/point/cpu.h"
#include "engine/nodes/point/kernels.h"
#include "engine/nodes/point/point.h"
#include "parallel.h"

namespace dapple::engine::point {

namespace {

const void* blocksOf(const FrameView& v) { return v.point; }

// the slot of a node that reads the frame's binding (block fragments over point::CpuFrame)
bool bindFrame(const FrameEnv& env, Resolved& out) {
  if (!env.fk) return false;
  new (out.bytes) FrameSlot{env.fk};
  return true;
}
// DisplayStages: the film plane, replacing the pixel unless a Blend follows in the chain
bool bindDisplay(const FrameEnv& env, Resolved& out) {
  if (!env.fk) return false;
  bool blend = false;
  for (NodeType t : env.chain) blend |= t == NodeType::Blend;
  new (out.bytes) DisplayCpu{env.fk, env.film, !blend, env.filmRow0};
  return true;
}

// Pack8: 8-bit encode of the interior of the tile (the chain already clamped to [0, 1])
Status tilePack8(CpuTile& t, const void*) {
  const ImageF& img = *t.img;
  const Rect& in = t.interior;
  const int ch = t.outChannels;
  ThreadPool::instance().run(in.y1 - in.y0, 16, [&](int a, int b) {
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
  return Status::Ok;
}
const CpuImpl kPack8{nullptr, nullptr, tilePack8};

NodeDesc table(const char* name, int maxIn, ParamMask deps, const CpuImpl& cpu) {
  NodeDesc d = nodeDesc(name, Domain::Table, Footprint::Global, 0, maxIn, std::move(deps));
  d.inputDomain[0] = Domain::Table;
  d.cpu = &cpu;
  return d;
}

}  // namespace

NodeDesc sourceDesc() {
  NodeDesc d = nodeDesc("Source", Domain::Image, Footprint::Source, 0, 0, keys({"ctx.source"}));
  d.outSpace = Space::Source;
  return d;
}

NodeDesc geometryDesc() {
  NodeDesc d = nodeDesc("Geometry", Domain::Image, Footprint::Resample, 1, 1, ParamGroups::get().frame);
  d.inSpace[0] = Space::Source;
  d.outSpace = Space::Source;
  d.fusion = Fusion::Head;
  d.wire = Wire::Source;
  d.behaviour = &geometryBehaviour();
  d.block = blocksOf;
  d.cpu = &geometryCpu();
  d.bind = bindFrame;
  return d;
}

NodeDesc gainDesc() {
  NodeDesc d = nodeDesc("Gain", Domain::Image, Footprint::Point, 1, 1, keys({"temp", "tint", "exposure", "ctx.camera", "ctx.source"}));
  d.inSpace[0] = Space::Source;
  d.outSpace = Space::Scene;
  d.fusion = Fusion::Head;
  d.cpu = &gainCpu();
  d.bind = bindFrame;
  return d;
}

// The field is a function of the source, the white balance and the exposure (inherited through the keyed edge from Gain) and of the ISO
// (ctx.source); the strength is the node's own key. Wired when the edit asks for it on a scene-referred source without a film; whether the
// scene needs compressing is known once the tone table is (hdrBind): an inactive block elides the node at plan time.
NodeDesc hdrDesc() {
  NodeDesc d = nodeDesc("Hdr", Domain::Image, Footprint::Point, 1, 1, keys({"hdr", "modern", "ctx.source"}));
  d.inSpace[0] = Space::Scene;
  d.fusion = Fusion::Head;
  d.wire = Wire::Scene;
  d.order = 10;
  d.wanted = hdrWanted;
  d.behaviour = &hdrBehaviour();
  d.block = blocksOf;
  d.cpu = &hdrCpu();
  d.bind = bindFrame;
  return d;
}

NodeDesc toneColourDesc() {
  NodeDesc d = nodeDesc("ToneColour", Domain::Image, Footprint::Point, 3, 4, keys({"ctx.source"}));
  d.inputDomain[1] = d.inputDomain[2] = d.inputDomain[3] = Domain::Table;
  d.inSpace[0] = Space::Scene;
  d.outSpace = Space::Display;
  d.fusion = Fusion::Head;
  d.cpu = &toneColourCpu();
  d.bind = bindFrame;
  return d;
}

NodeDesc toneColourExactDesc() {
  NodeDesc d = nodeDesc("ToneColourExact", Domain::Image, Footprint::Point, 1, 1, ParamGroups::get().analysis);
  d.inSpace[0] = Space::Scene;
  d.outSpace = Space::Display;
  d.fusion = Fusion::Head;
  d.cpu = &toneColourCpu();
  d.bind = bindFrame;
  return d;
}

NodeDesc displayDesc() {
  ParamMask m = keys({"curve_rgb", "curve_r", "curve_g", "curve_b", "lutAmount", "ctx.lutIdentity"});
  m.add(keysWithPrefix("cv_"));
  NodeDesc d = nodeDesc("DisplayStages", Domain::Image, Footprint::Point, 1, 1, m);
  d.inSpace[0] = Space::Display;
  d.fusion = Fusion::Head;
  d.cpu = &displayCpu();
  d.bind = bindDisplay;
  return d;
}

NodeDesc blendDesc() {
  NodeDesc d = nodeDesc("Blend", Domain::Image, Footprint::Point, 2, 2, keys({"film_strength"}));
  d.inSpace[0] = d.inSpace[1] = Space::Display;
  d.fusion = Fusion::Head;
  d.cpu = &blendCpu();
  d.bind = bindFrame;
  return d;
}

NodeDesc pack8Desc() {
  NodeDesc d = nodeDesc("Pack8", Domain::Image, Footprint::Sink, 1, 1, ParamMask{});
  d.inSpace[0] = Space::Display;
  d.cpu = &kPack8;
  return d;
}

NodeDesc latticeDesc() {
  ParamMask m = ParamGroups::get().colour;
  m.add(keys({"ctx.camera", "ctx.source", "ctx.lutIdentity", "ctx.filmOnly"}));
  NodeDesc d = table("ColourLattice", 0, m, latticeCpu());
  d.orderKey = key("ctx.bakeSize");
  return d;
}

NodeDesc overflowDesc() {
  ParamMask m = ParamGroups::get().colour;
  m.add(keys({"ctx.camera", "ctx.source", "ctx.lutIdentity", "ctx.filmOnly"}));
  return table("LatticeOverflow", 1, m, overflowCpu());
}

NodeDesc tonePreDesc() { return table("ToneAnalysisPre", 1, ParamGroups::get().analysis, tonePreCpu()); }  // input: the lattice (fast path)

NodeDesc toneTableDesc() {
  ParamMask m = ParamGroups::get().tone;
  // WB: the modern end points are measured after the white balance
  m.add(keys({"toneStrength", "modern", "ctx.proxy", "ctx.camera", "ctx.source", "ctx.fastAnalysis"})).add(ParamGroups::get().wb);
  return table("ToneTable", 1, m, toneTableCpu());  // input: ToneAnalysisPre when toneStrength > 0
}

NodeDesc analysisDesc() { return table("Analysis", 1, ParamGroups::get().analysis, analysisCpu()); }

}  // namespace dapple::engine::point
