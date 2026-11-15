// The node registry: every node type's descriptor, from the registration list (engine/nodes/node_list.inc). Each family declares its
// nodes in its own directory (`desc()` functions); nothing here knows a node.
#include "engine/graph/param_groups.h"
#include "engine/nodes/film/film_nodes.h"
#include "engine/nodes/frame_blocks.h"
#include "engine/nodes/point/point.h"
#include "engine/nodes/resize/resize.h"

namespace dapple::engine {

ParamMask keysWithPrefix(std::string_view prefix) {
  const ParamRegistry& r = ParamRegistry::instance();
  ParamMask m;
  for (std::size_t k = 0; k < r.size(); ++k)
    if (r.info(ParamKey(k)).name.compare(0, prefix.size(), prefix) == 0) m.add(ParamKey(k));
  return m;
}

const ParamGroups& ParamGroups::get() {
  static const ParamGroups g = [] {
    ParamGroups p;
    p.frame = keys({"crop", "straighten", "rotate", "flip", "ctx.region", "ctx.srcSize", "ctx.outLongEdge", "reshape"});
    p.tone = keys({"contrast", "highlights", "shadows", "whites", "blacks"});
    p.wb = keys({"temp", "tint"});
    p.colour = keys({"modern", "vibrance", "saturation", "look", "lookStrength", "palette", "curve_rgb", "curve_r", "curve_g", "curve_b", "lutAmount",
                     "ctx.outSpace"});
    p.colour.add(keysWithPrefix("hsl_")).add(keysWithPrefix("gr_")).add(keysWithPrefix("cv_"));
    p.look = keys({"look", "lookStrength", "palette"});
    p.filmCtx = keys({"ctx.filmActive", "ctx.filmOnly"});
    // The tonal analysis is the histogram of the cropped / straightened / vignetted proxy render: it depends on the frame geometry and the
    // vignette.
    p.analysis = keys({"exposure", "toneTarget", "toneStrength", "ctx.camera", "ctx.source", "ctx.proxy", "ctx.toneReach", "ctx.lutIdentity",
                       "ctx.fastAnalysis"});
    p.analysis.add(p.wb).add(p.tone).add(p.colour).add(p.filmCtx).add(keys({"crop", "straighten", "rotate", "flip", "vignette"}));
    p.filmPitch = keys({"ctx.srcSize", "ctx.outLongEdge", "ctx.region", "ctx.filmLib", "filmStock"});
    p.spatial = p.frame;
    p.spatial.add(p.look).add(keys({"ctx.quality", "ctx.source"}));
    const ParamRegistry& r = ParamRegistry::instance();
    for (std::size_t k = 0; k < r.size(); ++k) p.all.add(ParamKey(k));
    return p;
  }();
  return g;
}

const NodeRegistry& NodeRegistry::instance() {
  static const NodeRegistry r;
  return r;
}

NodeRegistry::NodeRegistry() {
  descs_.resize(kNodeTypeCount);
#define DAPPLE_NODE(t, desc, gpu)                \
  descs_[std::size_t(NodeType::t)] = desc();     \
  descs_[std::size_t(NodeType::t)].type = NodeType::t;
#include "engine/nodes/node_list.inc"
#undef DAPPLE_NODE
}

}  // namespace dapple::engine
