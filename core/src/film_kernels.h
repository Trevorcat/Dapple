#pragma once
// Run-layer kernels of the film pipeline: plain-data views over the baked tables and a set of function
// pointers per instruction-set variant (selected at runtime). No allocation, no exceptions, no virtual
// calls, no libm in the per-pixel path.

#include <cmath>

namespace dapple::film::detail {

// The film's reading of a display-referred source (JPEG / PNG / HEIC): an approximate inverse of a typical camera rendering, applied to the
// ungained source pixel before the film (docs/engine/pipeline-architecture.md, film section). A tone curve on luminance (a sample table over
// sqrt(Y), Y = ProPhoto luma of the display-linear pixel, value = scene-referred luminance with 0.18 -> 0.18) and a chroma factor
// k(Y) = k0 .. k1 (smoothstep e0 .. e1) on rgb / Y - 1. Calibrated in core/src/film_display_ref.cpp. GLSL twin: film_point.comp drInverse().
struct DisplayInverse {
  float tone[68];  // tone[i] = scene luminance at sqrt(Y) = i / 64, i = 0..64 (the rest is padding for the GPU uniform)
  float k0, k1, e0, e1;
};
const DisplayInverse& displayInverse();
// The table computed from the calibration knots (what film_display_table.inc was generated from); the unit test compares the two.
DisplayInverse buildDisplayInverseReference();
// DAPPLE_FILM_SRC_INVERSE=0 turns the inverse off for display-referred sources (A/B switch for calibration; unset / anything else = on).
bool displayInverseEnabled();
inline void displayInverseApply(const DisplayInverse& d, const float in[3], float out[3]) {
  const float Y = 0.2880402f * in[0] + 0.7118741f * in[1] + 0.0000857f * in[2];
  if (!(Y > 1e-9f)) {
    out[0] = in[0], out[1] = in[1], out[2] = in[2];
    return;
  }
  float gY;
  if (Y >= 1.f) {
    gY = d.tone[64] * Y;
  } else {
    const float pos = std::sqrt(Y) * 64.f;
    int i = int(pos);
    i = i < 63 ? i : 63;
    const float f = pos - float(i);
    gY = d.tone[i] + f * (d.tone[i + 1] - d.tone[i]);
  }
  float t = (Y - d.e0) / (d.e1 - d.e0);
  t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
  const float k = d.k0 + (d.k1 - d.k0) * (t * t * (3.f - 2.f * t));
  const float a = gY * (1.f - k), b = gY / Y * k;
  out[0] = a + b * in[0];
  out[1] = a + b * in[1];
  out[2] = a + b * in[2];
}

struct KView {
  const DisplayInverse* dr = nullptr;  // display-referred source: the inverse rendering in front of the input matrix (null: scene-referred, nothing)
  const float* mIn;  // 3 float4 columns
  int rawN, rawStride;
  const float* raw;  // padded float4 grid
  int curveN;
  float curveX0, curveInvDx;
  float gamma[4];
  const float* dn;   // [3][curveN] float2
  const float* d0;
  float dirQ[12], dirQ0[4];
  int lutN;
  float dmin[4], dscale[4];
  const float* lut;  // lutN^3 float4
  // split print path (enlarger diffusion): film density -> log10 paper exposure, paper curves, paper density -> display
  const float* lut1;
  const float* paperCurve;  // [3][paperN] float2
  int paperN;
  float paperX0, paperInvDx;
  float dminS[4], dscaleS[4];
  const float* lut3;
};

struct KernelSet {
  const char* name;
  // Whole non-spatial chain: interleaved linear ProPhoto RGB -> interleaved display sRGB.
  void (*fused)(const KView&, const float* in, float* out, int n, float scale);
  // Staged variants (hook path / per-stage timing). All buffers interleaved float3.
  void (*exposure)(const KView&, const float* rgb, float* e, int n, float scale);   // linear raw exposure
  void (*logE)(const KView&, const float* e, float* x, int n);                       // log10 exposure
  void (*develop1)(const KView&, const float* x, float* q, int n);                   // inhibitor field q
  void (*develop2)(const KView&, const float* x, const float* q, float* d, int n);   // density after DIR
  void (*printScan)(const KView&, const float* d, float* out, int n);
  void (*single)(const KView&, const float* rgb, float* out, float scale);           // one pixel (fused)
  // Split print path: film density -> log10 paper exposure (before the enlarger diffusion filter) ...
  void (*printRaw)(const KView&, const float* d, float* logE, int n);
  // ... and (blurred) log10 paper exposure -> display sRGB.
  void (*paperScan)(const KView&, const float* logE, float* out, int n);
  // Whole chain, stage loops over L1-resident blocks (default run path).
  void (*block)(const KView&, const float* in, float* out, int n, float scale);
};

extern const KernelSet kBase;
#if defined(DAPPLE_FILM_HAVE_AVX2)
extern const KernelSet kAvx2;
#endif
const KernelSet& selectKernels();

}  // namespace dapple::film::detail
