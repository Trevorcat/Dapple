// Node descriptors for every current stage of the CPU renderer (docs/engine/pipeline-architecture.md 4.5, 8.1).
// The declared ParamMasks are what the declaration-conformance harness (tests/engine) proves: perturbing a key that a
// node (and its keyed inputs) does not declare must leave its output bit-identical.
#include "engine/graph/graph.h"

namespace dapple::engine {

namespace {

ParamMask byPrefix(const ParamRegistry& r, std::string_view prefix) {
  ParamMask m;
  for (std::size_t k = 0; k < r.size(); ++k)
    if (r.info(ParamKey(k)).name.compare(0, prefix.size(), prefix) == 0) m.add(ParamKey(k));
  return m;
}

}  // namespace

const NodeRegistry& NodeRegistry::instance() {
  static const NodeRegistry r;
  return r;
}

NodeRegistry::NodeRegistry() {
  const ParamRegistry& P = ParamRegistry::instance();
  descs_.resize(kNodeTypeCount);

  // ---- groups of keys ------------------------------------------------------------------------------------------------
  const ParamMask FRAME = [&] {
    ParamMask m = P.mask({"crop", "straighten", "rotate", "flip", "ctx.region", "ctx.srcSize", "ctx.outLongEdge"});
    return m;
  }();
  const ParamMask TONE = P.mask({"contrast", "highlights", "shadows", "whites", "blacks"});
  const ParamMask WB = P.mask({"temp", "tint"});
  ParamMask COLOUR = P.mask({"vibrance", "saturation", "look", "lookStrength", "palette", "curve_rgb", "curve_r",
                             "curve_g", "curve_b", "lutAmount"});
  COLOUR.add(byPrefix(P, "hsl_")).add(byPrefix(P, "gr_")).add(byPrefix(P, "cv_"));
  const ParamMask LOOK = P.mask({"look", "lookStrength", "palette"});
  const ParamMask FILMCTX = P.mask({"ctx.filmActive", "ctx.filmOnly"});

  ParamMask ANALYSIS = P.mask({"exposure", "toneTarget", "toneStrength", "ctx.camera", "ctx.source", "ctx.proxy",
                               "ctx.toneReach", "ctx.lutIdentity", "ctx.fastAnalysis"});
  // The tonal analysis is the histogram of the cropped / straightened / vignetted proxy render: it depends on the frame
  // geometry and the vignette (the legacy keys masked them, so a crop change returned a stale analysis; fixed in a4b).
  ANALYSIS.add(WB).add(TONE).add(COLOUR).add(FILMCTX).add(P.mask({"crop", "straighten", "rotate", "flip", "vignette"}));

  ParamMask ALL;
  for (std::size_t k = 0; k < P.size(); ++k) ALL.add(ParamKey(k));

  auto def = [&](NodeType t, const char* name, Domain out, Footprint fp, int minIn, int maxIn, ParamMask deps) -> NodeDesc& {
    NodeDesc& d = descs_[std::size_t(t)];
    d.type = t;
    d.name = name;
    d.output = out;
    d.footprint = fp;
    d.minInputs = minIn;
    d.maxInputs = maxIn;
    d.deps = std::move(deps);
    return d;
  };

  // ---- image path ------------------------------------------------------------------------------------------------------
  def(NodeType::Source, "Source", Domain::Image, Footprint::Source, 0, 0, P.mask({"ctx.source"}));
  {
    NodeDesc& d = def(NodeType::Resize, "Resize", Domain::Image, Footprint::Resample, 1, 1,
                      P.mask({"ctx.outLongEdge", "ctx.region", "ctx.srcSize"}));
    d.cache = CacheHint::Keep;
    d.haloNote = "area-average support at the scale ratio";
  }
  {
    NodeDesc& d = def(NodeType::Geometry, "Geometry", Domain::Image, Footprint::Resample, 1, 1, FRAME);
    d.usesGeometry = true;
    d.haloNote = "Catmull-Rom 4x4 around the mapped point";
  }
  def(NodeType::Gain, "Gain", Domain::Image, Footprint::Point, 1, 1,
      P.mask({"temp", "tint", "exposure", "ctx.camera", "ctx.source"}));

  // ---- tables (always CPU, cached) ---------------------------------------------------------------------------------------
  {
    ParamMask m = COLOUR;
    m.add(P.mask({"ctx.camera", "ctx.source", "ctx.lutIdentity", "ctx.filmOnly"}));
    NodeDesc& d = def(NodeType::ColourLattice, "ColourLattice", Domain::Table, Footprint::Global, 0, 0, m);
    d.orderKey = P.mustFind("ctx.bakeSize");
    d.cache = CacheHint::Keep;
  }
  {
    ParamMask m = COLOUR;
    m.add(P.mask({"ctx.camera", "ctx.source", "ctx.lutIdentity", "ctx.filmOnly"}));
    NodeDesc& d = def(NodeType::LatticeOverflow, "LatticeOverflow", Domain::Table, Footprint::Global, 0, 1, m);
    d.inputDomain[0] = Domain::Table;
    d.cache = CacheHint::Keep;
  }
  {
    NodeDesc& d = def(NodeType::ToneAnalysisPre, "ToneAnalysisPre", Domain::Table, Footprint::Global, 0, 1, ANALYSIS);
    d.inputDomain[0] = Domain::Table;  // the lattice, for the fast path (scheduling edge)
    d.cache = CacheHint::KeepIfCheap;
  }
  {
    ParamMask m = TONE;
    m.add(P.mask({"toneStrength", "ctx.camera", "ctx.source", "ctx.fastAnalysis"}));
    NodeDesc& d = def(NodeType::ToneTable, "ToneTable", Domain::Table, Footprint::Global, 0, 1, m);
    d.inputDomain[0] = Domain::Table;  // ToneAnalysisPre when toneStrength > 0
    d.cache = CacheHint::Keep;
  }
  {
    NodeDesc& d = def(NodeType::Analysis, "Analysis", Domain::Table, Footprint::Global, 0, 1, ANALYSIS);
    d.inputDomain[0] = Domain::Table;
    d.cache = CacheHint::KeepIfCheap;
  }

  // ---- point stages ------------------------------------------------------------------------------------------------------
  {
    NodeDesc& d = def(NodeType::ToneColour, "ToneColour", Domain::Image, Footprint::Point, 3, 4, P.mask({"ctx.source"}));
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = d.inputDomain[2] = d.inputDomain[3] = Domain::Table;
  }
  {
    ParamMask m = ANALYSIS;
    def(NodeType::ToneColourExact, "ToneColourExact", Domain::Image, Footprint::Point, 1, 1, m);
  }
  {
    ParamMask m = FRAME;
    m.add(P.mask({"vignette"}));
    NodeDesc& d = def(NodeType::Vignette, "Vignette", Domain::Image, Footprint::Point, 1, 1, m);
    d.usesGeometry = true;
  }

  // ---- neighbourhood stages (executed whole-frame, halos declared for a9) --------------------------------------------------
  auto spatial = [&](NodeType t, const char* name, Footprint fp, std::initializer_list<std::string_view> own,
                     const char* halo) {
    ParamMask m = FRAME;
    m.add(LOOK).add(P.mask(own));
    m.add(P.mask({"ctx.quality", "ctx.source"}));
    NodeDesc& d = def(t, name, Domain::Image, fp, 1, 1, m);
    d.usesGeometry = true;
    d.haloNote = halo;
  };
  spatial(NodeType::NRLuma, "NRLuma", Footprint::Window, {"nrLuma", "ctx.noise"}, "box radii scale with the frame (running sums)");
  spatial(NodeType::NRChroma, "NRChroma", Footprint::Window, {"nrColor", "ctx.noise"}, "box on 1/2-grid chroma");
  spatial(NodeType::Dehaze, "Dehaze", Footprint::Composite, {"dehaze"}, "dark channel on a 1/4 grid + global airlight");
  spatial(NodeType::Detail, "Detail", Footprint::Composite, {"clarity", "texture", "sharpen", "sharpRadius"},
          "guided base + detail passes");
  spatial(NodeType::Grain, "Grain", Footprint::Point, {"grain", "ctx.activeParam", "ctx.filmOnly"}, "hash on frame coordinates");
  def(NodeType::Pack8, "Pack8", Domain::Image, Footprint::Sink, 1, 1, ParamMask{});

  // ---- film (super-node until b4) ---------------------------------------------------------------------------------------
  {
    ParamMask m = FRAME;
    m.add(WB).add(P.mask({"exposure", "filmStock", "filmPaper", "film_exp", "film_warmth", "film_tint", "film_grain",
                          "film_halation", "ctx.filmKeyEv", "ctx.filmPrintEv", "film_black", "film_wb", "ctx.filmScene", "ctx.camera", "ctx.source", "ctx.filmLib", "ctx.quality", "ctx.filmActive"}));
    NodeDesc& d = def(NodeType::FilmSim, "FilmSim", Domain::Image, Footprint::Super, 1, 1, m);
    d.usesGeometry = true;
    d.haloNote = "spectral film simulator incl. halation / DIR / grain / scan";
  }
  // ---- film decomposition (8.1, 8.3): each node declares the smallest set it reads itself ------------------------------------
  // Pixel pitch of the film frame (spatial effects are defined in micrometres) follows the frame size keys.
  const ParamMask FILMPITCH = P.mask({"ctx.srcSize", "ctx.outLongEdge", "ctx.region", "ctx.filmLib", "filmStock"});
  {
    NodeDesc& d = def(NodeType::FilmTableRaw, "FilmTableRaw", Domain::Table, Footprint::Global, 0, 0,
                      P.mask({"filmStock", "film_wb", "temp", "tint", "ctx.filmScene", "ctx.filmLib"}));
    d.cache = CacheHint::Keep;
    d.haloNote = "raw-exposure LUT 192^2 (reflect-padded)";
  }
  {
    // the film side only: the curves do not depend on the print paper
    NodeDesc& d = def(NodeType::FilmTableCurves, "FilmTableCurves", Domain::Table, Footprint::Global, 0, 0,
                      P.mask({"filmStock", "ctx.filmLib"}));
    d.cache = CacheHint::Keep;
    d.haloNote = "density + pre-coupler curves, DIR matrix";
  }
  {
    // dragging print warmth / tint re-bakes only this table
    NodeDesc& d = def(NodeType::FilmTablePrintScan, "FilmTablePrintScan", Domain::Table, Footprint::Global, 0, 0,
                      P.mask({"filmStock", "filmPaper", "film_warmth", "film_tint", "ctx.filmPrintEv", "film_black", "ctx.filmLib"}));
    d.cache = CacheHint::Keep;
    d.haloNote = "33^3 film density -> display sRGB LUT, glare white, sRGB decode";
  }
  {
    // gain (white balance x exposure x film exposure) is folded into the input matrix
    ParamMask m = WB;
    m.add(P.mask({"exposure", "film_exp", "ctx.filmKeyEv", "ctx.camera", "ctx.source", "ctx.filmLib"}));
    NodeDesc& d = def(NodeType::FilmExposure, "FilmExposure", Domain::Image, Footprint::Point, 2, 2, m);
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = Domain::Table;  // FilmTableRaw
    d.haloNote = "point: matrix + chromaticity + bicubic raw LUT; output is 3 planar F32 planes";
  }
  {
    ParamMask m = FILMPITCH;
    m.add(P.mask({"film_halation"}));
    NodeDesc& d = def(NodeType::FilmBlurExposure, "FilmBlurExposure", Domain::Image, Footprint::Composite, 1, 1, m);
    d.usesGeometry = true;
    d.cache = CacheHint::KeepIfCheap;
    d.haloNote = "camera diffusion / lens blur + scatter + halation: Gaussian mixtures, half-resolution pyramid above 3 px";
  }
  {
    ParamMask m = FILMPITCH;
    m.add(P.mask({"ctx.quality"}));  // preview mode drops the DIR diffusion
    NodeDesc& d = def(NodeType::FilmDirField, "FilmDirField", Domain::Image, Footprint::Composite, 2, 2, m);
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = Domain::Table;  // FilmTableCurves
    d.usesGeometry = true;
    d.cache = CacheHint::KeepIfCheap;
    d.haloNote = "inhibitor field q from E2, diffused (sigma >= 3.2 px: half-resolution)";
  }
  {
    NodeDesc& d = def(NodeType::FilmDevelop, "FilmDevelop", Domain::Image, Footprint::Point, 3, 3, ParamMask{});
    d.inputDomain[0] = Domain::Image;  // E2
    d.inputDomain[1] = Domain::Image;  // q field (FilmDirField), or elided
    d.inputDomain[2] = Domain::Table;  // FilmTableCurves
    d.haloNote = "point: log10, curves, DIR correction -> density";
  }
  {
    ParamMask m = FILMPITCH;
    m.add(P.mask({"film_grain", "ctx.quality", "ctx.keepGrain", "ctx.frameIndex", "ctx.fullLongEdge"}));
    NodeDesc& d = def(NodeType::FilmGrain, "FilmGrain", Domain::Image, Footprint::Window, 1, 1, m);
    d.usesGeometry = true;
    d.haloNote = "stateless hash noise + dye-cloud / grain blur (a few px)";
  }
  {
    // identity unless the film has an enlarger diffusion filter (FilmResolved::fx, not an edit parameter)
    NodeDesc& d = def(NodeType::FilmPrintExposure, "FilmPrintExposure", Domain::Image, Footprint::Point, 2, 2, FILMPITCH);
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = Domain::Table;  // FilmTablePrintScan (split tables)
    d.usesGeometry = true;
    d.haloNote = "point: tetrahedral 3-D LUT density -> log paper exposure, then 10^x (enlarger diffusion path only)";
  }
  {
    NodeDesc& d = def(NodeType::FilmEnlargerBlur, "FilmEnlargerBlur", Domain::Image, Footprint::Composite, 1, 1, FILMPITCH);
    d.usesGeometry = true;
    d.cache = CacheHint::KeepIfCheap;
    d.haloNote = "enlarger diffusion filter: Gaussian mixtures of the linear paper exposure (enlarger diffusion path only)";
  }
  {
    NodeDesc& d = def(NodeType::FilmPrintScan, "FilmPrintScan", Domain::Image, Footprint::Point, 2, 2, ParamMask{});
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = Domain::Table;  // FilmTablePrintScan
    d.haloNote = "point: tetrahedral 3-D LUT (enlarger diffusion path: log10 + paper curve + scan LUT of the blurred paper exposure)";
  }
  {
    ParamMask m = FILMPITCH;
    m.add(P.mask({"filmPaper", "ctx.quality", "ctx.frameIndex"}));
    NodeDesc& d = def(NodeType::FilmGlare, "FilmGlare", Domain::Image, Footprint::Window, 2, 2, m);
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = Domain::Table;  // FilmTablePrintScan (sRGB decode, glare white)
    d.usesGeometry = true;
    d.haloNote = "decode to linear + blurred log-normal hash field (reads no neighbours of the image)";
  }
  {
    ParamMask m = FILMPITCH;
    m.add(P.mask({"filmPaper", "ctx.quality"}));
    NodeDesc& d = def(NodeType::FilmScan, "FilmScan", Domain::Image, Footprint::Window, 1, 1, m);
    d.usesGeometry = true;
    d.haloNote = "scanner blur + unsharp mask in linear light, sRGB encode";
  }
  {
    ParamMask m = P.mask({"curve_rgb", "curve_r", "curve_g", "curve_b", "lutAmount", "ctx.lutIdentity"});
    m.add(byPrefix(P, "cv_"));
    def(NodeType::DisplayStages, "DisplayStages", Domain::Image, Footprint::Point, 1, 1, m);
  }
  {
    NodeDesc& d = def(NodeType::Blend, "Blend", Domain::Image, Footprint::Point, 2, 2, P.mask({"film_strength"}));
    (void)d;
  }

  // ---- the stage (a) super-node ------------------------------------------------------------------------------------------
  {
    NodeDesc& d = def(NodeType::LegacyRender, "LegacyRender", Domain::Image, Footprint::Super, 1, 4, ALL);
    d.inputDomain[0] = Domain::Image;
    d.inputDomain[1] = d.inputDomain[2] = d.inputDomain[3] = Domain::Table;
    d.usesGeometry = true;
  }
}

}  // namespace dapple::engine
