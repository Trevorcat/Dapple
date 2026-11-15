#pragma once
// Groups of ParamKeys that node families declare their masks from (graph/registry.cpp), and the helpers a family's `desc()` uses.
// The declared masks are what the declaration-conformance harness (tests/engine) proves: perturbing a key that a node (and its keyed
// inputs) does not declare must leave its output bit-identical.
#include <initializer_list>
#include <string_view>

#include "engine/graph/graph.h"

namespace dapple::engine {

struct ParamGroups {
  ParamMask frame;      // framing + region + source size + reshaping: what moves pixels
  ParamMask tone, wb, colour, look, filmCtx;
  ParamMask analysis;   // the tonal analysis (histogram of the cropped, vignetted proxy render)
  ParamMask filmPitch;  // pixel pitch of the film frame (spatial film effects are defined in micrometres)
  ParamMask spatial;    // what every neighbourhood node of the display chain reads besides its own sliders
  ParamMask all;
  static const ParamGroups& get();
};

inline ParamMask keys(std::initializer_list<std::string_view> names) { return ParamRegistry::instance().mask(names); }
ParamMask keysWithPrefix(std::string_view prefix);
inline ParamKey key(std::string_view name) { return ParamRegistry::instance().mustFind(name); }

// A descriptor with its ports and mask; the family fills in the rest.
inline NodeDesc nodeDesc(const char* name, Domain out, Footprint fp, int minIn, int maxIn, ParamMask deps) {
  NodeDesc d;
  d.name = name;
  d.output = out;
  d.footprint = fp;
  d.minInputs = minIn;
  d.maxInputs = maxIn;
  d.deps = std::move(deps);
  return d;
}

}  // namespace dapple::engine
