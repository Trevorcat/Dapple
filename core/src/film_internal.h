#pragma once
// Internal pieces of the spectral film engine (not part of the public API; see dapple/film.h).
// Everything here is double precision "reference math". The per-pixel run path lives in
// film_kernels_impl.inc and only ever sees the float tables produced by film_model.cpp.

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "dapple/film.h"

namespace dapple::film::detail {

constexpr int kN = 81;           // spectral samples, 380..780 nm step 5
constexpr double kWl0 = 380.0;   // nm
constexpr double kWlStep = 5.0;  // nm
constexpr double kPi = 3.14159265358979323846;

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;  // row-major
using Spec = std::array<double, kN>;

inline double wavelength(int i) { return kWl0 + kWlStep * i; }

Mat3 mul(const Mat3& a, const Mat3& b);
Vec3 mul(const Mat3& a, const Vec3& v);
Mat3 inverse(const Mat3& m);
Mat3 diag(const Vec3& d);

// --- standard colorimetry ----------------------------------------------------------------------
const Spec& cmf(int channel);  // CIE 1931 2-degree observer x,y,z bar on the 81-sample grid
// Illuminant by label ("D50","D55","D65","K75P","T","TH-KG3","TH-KG3-L","BBnnnn"), mean-normalised
// to 1. Returns false for unknown labels.
bool illuminant(const std::string& label, Spec& out);
Spec kg3Transmittance();
Vec3 xyzOfWhite(double x, double y);                      // Y = 1
Vec3 illuminantXyz(const Spec& e);                        // sum(E cmf)/sum(E ybar), Y = 1
void xyOfXyz(const Vec3& xyz, double& x, double& y);
Mat3 adaptationMatrix(const Mat3& cone, double xs, double ys, double xd, double yd);
const Mat3& cat16();
const Mat3& cat02();
const Mat3& prophotoToXyz();   // library-rounded matrix, native white D50
const Mat3& srgbToXyz();
const Mat3& xyzToSrgb();
constexpr double kD50x = 0.3457, kD50y = 0.3585;   // ProPhoto native white
constexpr double kD65x = 0.3127, kD65y = 0.3290;   // sRGB native white

// Pure-filter model of the dichroic colour head (Kodak CC units), per-wavelength transmittance of
// the C, M and Y filters in the pass band model "F/2 + 1/2".
void dichroicBase(Spec& fc, Spec& fm, Spec& fy);

double srgbOetf(double v);
double srgbEotf(double v);

// --- spectral upsampling (chromaticity -> smooth emission spectrum) ---------------------------------
struct SpectrumCoeffs {
  double a[3];
  double norm;  // 1 / sum(s * (xbar+ybar+zbar))
};
void spectrumFromCoeffs(const double a[3], Spec& s);  // unnormalised sigmoid spectrum in (0,1)
inline void tcToXy(double tc0, double tc1, double& x, double& y) {
  const double r = std::sqrt(std::max(tc0, 0.0));
  x = 1.0 - r;
  y = tc1 * r;
}
inline void xyToTc(double x, double y, double& tc0, double& tc1) {
  const double d = 1.0 - x;
  tc0 = std::min(std::max(d * d, 0.0), 1.0);
  tc1 = std::min(std::max(y / std::max(d, 1e-10), 0.0), 1.0);
}
// Fits the whole table (single thread, ~1 s for n = 192). Deterministic.
UpsampleTable generateUpsampleTable(int n);

// --- output gamut compression (CAM16-UCS), double precision ---------------------------------------
struct Cam16 {
  Cam16();  // viewing conditions of sRGB output (adapting white D65, L_A 64, Y_b 20, average surround)
  void forward(const Vec3& xyz, double& J, double& a, double& b) const;  // xyz in 0..1 (Y=1 white)
  Vec3 inverse(double Jp, double ap, double bp) const;
  double Fl, nbb, ncb, z, n, c, nc, D, Aw;
  Vec3 rgbw, drgb;
};
CmaxTable buildCmaxTable();  // 64 x 720, parallel
// Gamut-compresses a linear sRGB triple (default algorithm "cam16ucs" of the spec, section 4.5.3).
Vec3 compressOutput(const Cam16& cam, const CmaxTable& tab, const Vec3& rgb);

// --- film model ---------------------------------------------------------------------------------
// Float run tables: plain flat arrays laid out like textures so the same stages can be ported 1:1 to
// Vulkan/Metal (see docs/film/architecture.md). Built by FilmModel::bake().
template <class T, std::size_t Align = 64>
struct AlignedAlloc {
  using value_type = T;
  AlignedAlloc() = default;
  template <class U>
  AlignedAlloc(const AlignedAlloc<U, Align>&) {}
  template <class U>
  struct rebind {
    using other = AlignedAlloc<U, Align>;
  };
  T* allocate(std::size_t n) {
    const std::size_t bytes = (n * sizeof(T) + Align - 1) / Align * Align;
#if defined(_WIN32)
    void* p = _aligned_malloc(bytes, Align);
#else
    void* p = nullptr;
    if (posix_memalign(&p, Align, bytes) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
  }
  template <class U>
  bool operator==(const AlignedAlloc<U, Align>&) const { return true; }
  template <class U>
  bool operator!=(const AlignedAlloc<U, Align>&) const { return false; }
};
using AVec = std::vector<float, AlignedAlloc<float>>;

// Parameter digest of FilmSimulator::setParams (stock specifics, preview mode, debug switches), exposed for the engine.
void digestParams(FilmParams& p, const FilmStock& film);

// Generation counter bumped by film::trimMemory(): per-thread scratch buffers notice it on their next use.
inline std::atomic<unsigned>& scratchGeneration() {
  static std::atomic<unsigned> g{0};
  return g;
}

// Per-thread scratch buffer: 64 B aligned, grows on demand, and shrinks back (instead of pinning the high-water mark
// for the process lifetime) when a request is 4x smaller than a large buffer, or after trimMemory().
struct Scratch {
  AVec v;
  unsigned gen = 0;
  float* get(std::size_t n) {
    const unsigned g = scratchGeneration().load(std::memory_order_relaxed);
    const bool shrink = gen != g || (v.size() > (std::size_t(1) << 22) && v.size() > 4 * n);
    if (shrink || v.size() < n) {
      AVec fresh(n);
      v.swap(fresh);
      gen = g;
    }
    return v.data();
  }
};

struct FilmTables {
  // input stage
  float mIn[12] = {};  // columns of the 3x3 (linear ProPhoto -> XYZ in the film's balance white) as float4
  int rawN = 0;        // raw LUT nodes per axis (192)
  AVec raw;  // (rawN+2)^2 float4, reflect-padded by 1 node; index [(j+1)*(rawN+2) + (i+1)]
  // density curves
  int curveN = 256;
  float curveX0 = -3.f, curveInvDx = 255.f / 7.f;
  float gamma[4] = {1, 1, 1, 1};
  AVec dn;  // [3][curveN] float2 (value, slope to next node)
  AVec d0;  // same layout, pre-coupler curves
  float dirQ[12] = {};    // q = Q d + q0 : column k (contribution of d_k) as float4
  float dirQ0[4] = {};
  // print + scan, whole chain baked: film density -> encoded display sRGB
  int lutN = 0;
  float dmin[4] = {}, dscale[4] = {};  // grid coordinate = (d - dmin) * dscale * (lutN-1)
  AVec lut;              // lutN^3 float4, index ((iy*N + im)*N + ic): C fastest, then M, then Y
  AVec srgbDecode;       // 4097 entries: encoded -> linear (helper for scan-stage hooks)
  float glareRgb[4] = {1, 1, 1, 0};  // linear display RGB of the viewing white (Y = 1); [3] = mean glare baked into the LUT (fraction of white)
  // Split print path (only baked when the enlarger diffusion filter is active): film density -> log10 paper exposure (3-D LUT, same grid
  // as `lut`), paper density curves (1-D), paper density -> display sRGB (3-D LUT over [dminS, dmaxS]).
  bool split = false;
  AVec lut1;
  int paperN = 0;
  float paperX0 = 0, paperInvDx = 0;
  AVec paperCurve;  // [3][paperN] float2 (value, slope to next node)
  float dminS[4] = {}, dscaleS[4] = {};
  AVec lut3;
};

struct BakeTimings {
  double rawLutMs = 0, curvesMs = 0, neutralMs = 0, lutMs = 0, totalMs = 0;
};

// Raw-exposure LUT per film: R[tc0 index i][tc1 index j] = per-unit-brightness exposure of the three layers.
struct RawLut {
  int n = 0;
  std::vector<float> node;  // n*n*4 (R,G,B,0); index (j*n + i)*4
};
struct RawLutParams {
  bool window = true, surface = false, gamut = true;
  double knee[3] = {0.0, 1.0, 6.0};
  double uv[3] = {0, 410, 8}, ir[3] = {0, 675, 15};
};
// wx, wy: chromaticity of the film's reference illuminant; mIn: ProPhoto -> XYZ(film white) matrix.
std::shared_ptr<RawLut> bakeRawLut(const FilmStock& film, const UpsampleTable& up, const RawLutParams& rp,
                                   const Spec& iRef, double wx, double wy, const Mat3& mIn);

// Double precision model. Owns everything derived from (stocks, non-spatial params).
class FilmModel {
 public:
  FilmModel(const FilmStock& film, const FilmStock* paper, const FilmParams& digested, const UpsampleTable& up,
            const CmaxTable& cmax, std::shared_ptr<const RawLut> raw);
  bool ok() const { return err_.empty(); }
  const std::string& error() const { return err_; }
  const FilmParams& effective() const { return p_; }  // with neutral filters resolved

  // ---- reference ("direct") evaluation, used as the test oracle ----
  Vec3 exposure(const Vec3& rgbProPhoto) const;                       // linear film exposure, EV applied
  Vec3 exposureScaled(const Vec3& rgbProPhoto, double scale, const Mat3* m = nullptr) const;
  Vec3 logExposure(const Vec3& e) const;                              // log10(max(e,0)+1e-10)
  Vec3 develop(const Vec3& x) const;                                  // density with DIR (uniform field)
  Vec3 developPlain(const Vec3& x) const;                             // density without DIR
  Vec3 printScan(const Vec3& d, Vec3* linRgb = nullptr) const;        // density -> encoded display sRGB
  Vec3 printExposure(const Vec3& d) const;  // linear paper exposure (before the enlarger diffusion filter and log10)
  Vec3 scanOut(const Vec3& paperDensity) const;  // paper density -> encoded display sRGB
  Vec3 render(const Vec3& rgbProPhoto) const;                         // whole chain, double precision
  Vec3 midGreyDensity() const { return dMidDir_; }
  double printGain() const { return g_; }
  Vec3 paperDensityOf(const Vec3& logE) const { return paperDensity(logE); }

  FilmTables bake(BakeTimings* t = nullptr) const;
  // The three parts of bake() (engine film table nodes bake and cache them separately). bakeInput: mIn + padded raw LUT;
  // bakeCurves: density / pre-coupler curves + DIR matrix (identical for a model built without paper); bakePrintScan:
  // 33^3 print + scan LUT, glare white, sRGB decode (and the split-path tables).
  void bakeInput(FilmTables& t) const;
  void bakeCurves(FilmTables& t) const;
  void bakePrintScan(FilmTables& t) const;
  // Changes the enlarger filtration shifts (warmth / tint) without re-running the neutral solve.
  void rebalance(double mShift, double yShift);

 private:
  void init();
  Vec3 paperRaw(const Vec3& d, const Spec& ef) const;
  Vec3 scanXyz(const Vec3& dp) const;
  Vec3 paperDensity(const Vec3& logE) const;
  Vec3 printScanLinear(const Vec3& d, bool glare) const;
  Vec3 scanLinear(const Vec3& dp, bool glare) const;
  void solveNeutral();
  void buildCurves();
  void buildEnlarger(double cNeutral, double mCC, double yCC);

  const FilmStock& film_;
  const FilmStock* paper_;
  FilmParams p_;
  std::string err_;
  const CmaxTable* cmax_;
  Cam16 cam_;

  // filming
  Mat3 mIn_{};
  double wx_ = 0, wy_ = 0;
  Spec iRef_{};
  std::shared_ptr<const RawLut> raw_;
  std::vector<double> dn_[3], d0_[3];
  double lex0_ = -3, dlex_ = 7.0 / 255.0;
  int nCurve_ = 256;
  Mat3 dirM_{};
  Vec3 dMax_{};
  Mat3 dirQ_{};
  Vec3 dirQ0_{};
  Vec3 gam_{1, 1, 1};
  bool dirActive_ = false;
  // print / scan media
  bool scanFilm_ = false;
  std::array<bool, kN> filmValid_{}, paperValid_{};
  Spec filmDye_[3]{}, filmBase_{}, paperDye_[3]{}, paperBase_{}, paperS_[3]{};
  Spec eEnl_{}, eView_{}, ef_{};
  Vec3 xyzIll_{};
  double normView_ = 1;
  Mat3 xyzToRgb_{};  // CAT02(view white -> D65) then XYZ->sRGB
  double g_ = 1, pe_ = 1;
  // scanner black / white correction (print path, spec 02 s3.10)
  bool bwOn_ = false;
  double bwM_ = 1, bwQ_ = 0, bwExposure_ = 1;
  std::vector<double> paperTab_[3];
  void buildBlackWhite();
  Vec3 rawPre_{};
  std::vector<double> paperAxis_, paperCurve_[3];
  Vec3 dMidDir_{};
  Vec3 dMinFilm_{}, dMaxFilm_{};
  Vec3 dMinScan_{}, dMaxScan_{};
};

}  // namespace dapple::film::detail
