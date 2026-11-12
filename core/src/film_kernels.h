#pragma once
// Run-layer kernels of the film pipeline: plain-data views over the baked tables and a set of function
// pointers per instruction-set variant (selected at runtime). No allocation, no exceptions, no virtual
// calls, no libm in the per-pixel path.

namespace dapple::film::detail {

struct KView {
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
