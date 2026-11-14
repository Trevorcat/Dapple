#pragma once
// Internal: the structure-aware continuous field (reshape mode C; docs/engine/pipeline-architecture.md section 49).
//
// Photo analysis (a structure tensor per solve-grid node, cached per photo) and the sparse quadratic solve of the displacement field on a coarse grid.
// The energy (forward field F, long-edge units; everything linear in F):
//   data       wd |F - T|^2                       T = the operators' intended displacement inside the person plane, 0 beyond the support / at the border
//   structure  wS c |n . (dF/dt)|^2               along a strong straight edge the displacement component across it must not change
//   keep       wK c |n . F|^2                     strong structures prefer to stay put (light)
//   smooth     ws |grad F|^2                      low in textureless background (it absorbs the distortion), higher on texture
//   identity   wi |F|^2                           grows with the distance from the silhouette: the field dies out
// Solved by block-Jacobi preconditioned conjugate gradients with a fixed iteration budget, warm-started by the caller.
#include <cstdint>
#include <memory>
#include <vector>

namespace dapple::warp {

struct StructureGrid {
  int w = 0, h = 0;            // nodes of the solve grid
  std::vector<float> nx, ny;   // unit normal (gradient direction) of the dominant edge at every node
  std::vector<float> c;        // edge strength x coherence, 0..1 (0 at the border ring)
  std::vector<float> tex;      // local texture, 0..1
  std::uint64_t key = 0;
  double ms = 0;               // analysis time
};

// Structure tensor of the luma plane (lw x lh, 0..255, the photo's full extent) sampled at gw x gh nodes. Cached (process-wide LRU of 6) by (hash, gw, gh).
std::shared_ptr<const StructureGrid> structureAnalyse(const std::uint8_t* luma, int lw, int lh, std::uint64_t hash, int gw, int gh, bool* hit = nullptr);
void structureCacheClear();

struct StructureSolveIn {
  int w = 0, h = 0;
  double hx = 1, hy = 1;                // node spacing, long-edge units
  const StructureGrid* S = nullptr;     // null: no structure term, uniform smoothness
  const std::vector<double>* wd = nullptr;    // data weight per node (>= 0)
  const std::vector<double>* T = nullptr;     // data target, 2 per node
  const std::vector<double>* dist = nullptr;  // distance from the silhouette per node, long-edge units
  const std::vector<double>* stiff = nullptr; // per node multiplier (>= 1) of the smoothness weight (the fold-free reweighting), null = 1
  double decay = 0.05;                  // length over which the field is allowed to die out
  int maxIter = 120;
};
struct StructureSolveOut {
  int iters = 0;
  double rel = 0;   // final residual relative to the initial one
  double ms = 0;
};
// x: 2 per node, warm start in, solution out.
StructureSolveOut structureSolve(const StructureSolveIn& in, std::vector<double>& x);

// chamfer distance (long-edge units) from the marked nodes of a w x h node grid
std::vector<double> structureDistance(const std::vector<char>& marked, int w, int h, double hx, double hy);

}  // namespace dapple::warp
