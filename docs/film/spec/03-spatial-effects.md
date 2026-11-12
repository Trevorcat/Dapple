# 03 - Spatial and stochastic effects (functional specification)

Clean-room functional specification of every neighbourhood (blur-like) and stochastic
operation in the reference film-simulation pipeline: highlight boost, diffusion filter,
lens blur, in-emulsion scatter, halation, DIR-coupler inhibitor diffusion, film grain,
print glare, scanner blur and unsharp mask, plus the filter primitives they use and the
mapping between image pixels and physical film size.

This document contains no reference source code. Equations, tables of constants and
behavioural descriptions only. Numeric constants and defaults are facts transcribed from
the reference and are listed exactly. All images are floating point (double in the
reference), shape H x W x 3, channel order R,G,B (for density: C,M,Y layers, same index order).

Conventions: `x` = pixel column, `y` = pixel row, `G_s` = isotropic 2D Gaussian blur with
standard deviation `s` pixels (separable), `E_l` = isotropic 2D exponential blur with
decay constant `l` pixels, `*` = convolution, `px` = pixel, `um` = micrometre (film plane).

---

## 1. Where the effects sit in the pipeline

Pipeline topology (default, print path): `preprocess -> film.expose -> film.develop ->
print.expose -> print.develop -> scan`. With `io.scan_film = true` the print stages are
skipped and the developed negative/positive film density is scanned directly.

| Order | Stage | Spatial / stochastic operation |
|---|---|---|
| 0 | preprocess | auto-exposure gain (global); pixel size derived; optional crop; optional cubic upscale |
| 1 | film.expose, after RGB -> film-raw spectral integration and exposure-compensation gain `2^EV` | (a) highlight boost -> (b) camera diffusion filter -> (c) camera lens blur -> (d) halation block: in-emulsion scatter, then back-reflection halation -> (e) black/white exposure correction scalar -> (f) `log10(max(raw,0)+1e-10)` |
| 2 | film.develop | density-from-curve; (g) DIR-coupler inhibitor diffusion and re-lookup; (h) grain (negative/positive film density) |
| 3 | print.expose | film density -> spectral transmission -> print-paper raw; times print exposure; (i) enlarger diffusion filter on the print raw (before log10) |
| 4 | print.develop | none (no grain, no blur on the print paper density) |
| 5 | scan | density -> XYZ; black/white XYZ correction; (j) glare (print only); XYZ -> linear RGB; output gamut compression; (k) scanner Gaussian blur; (l) unsharp mask; transfer-function encoding |

Notes:
* The enlarger has a `lens_blur` parameter in the parameter schema but no stage consumes it
  (it is only zeroed by the preview/spatial-off switches). Treat enlarger lens blur as
  **not implemented** in the reference; reproduce that (no blur) for numerical parity.
* Film `glare` params exist in the film render parameters but are never used: glare is
  applied only in the print scan (parameters in `print_render.glare`), never when scanning film.
* All blurs in stage 1 operate on **linear raw exposure** (not log). Grain and DIR operate on
  **density**. Glare operates on XYZ. Scanner blur/unsharp operate on **linear RGB after
  gamut compression and before the output transfer function** (so unsharp mask is applied
  in linear light, not in display-encoded values, and its result is not clamped here).

## 2. Pixel size and film format

`pixel_size_um = film_format_mm * 1000 / max(H0, W0)`

where `H0, W0` are the dimensions of the image entering preprocessing **after auto-exposure
gain but before crop and before upscale**. `film_format_mm` (default **35.0**) is the physical
length of the image's **long edge** (it is not 36 mm, and not the diagonal). Consequences:

* Cropping (`io.crop`) does not change pixel size: a crop is a window of the same sensor grid.
  Crop rectangle: centre `(cx, cy)` in normalised coordinates of width/height (default 0.5,
  0.5); size `(sx, sy)` as fractions of the **long side** in pixels (default 0.1, 0.1); centre
  in pixels = round(shape * centre); size = round(long_side * size); top-left = round(centre -
  size/2); clamp top-left to >= 0 and shift back if the window overruns the bottom/right edge.
* Upscale (`io.upscale_factor`, default 1.0): if != 1, pixel size is divided by the factor and
  the image is resampled with a bicubic (order 3) rescale, all channels, no extra anti-alias
  step specified. The effect is that all physical (um) sizes are resolved over more pixels,
  and dye-cloud-scale grain detail appears.
* Every `*_um` parameter is converted as `sigma_px = value_um / pixel_size_um`.
* Parameters given directly in **pixels** (not scaled by format/resolution): `grain.blur`,
  `scanner.lens_blur`, scanner unsharp-mask sigma, glare blur. Everything else in this
  document that is in um scales with image resolution; pixel-unit parameters do not, so
  their physical meaning changes with image size (an inherent property of the reference).
* Example: 6000 px long edge -> 5.833 um/px; 3000 px -> 11.67 um/px.
* Preview rendering first shrinks the image so its long edge is at most `settings.preview_max_size`
  (default 640) with a linear (order 1) anti-aliased resize, then runs the normal path (so
  pixel size is correspondingly larger). Auto-exposure measures a nearest-neighbour (order 0)
  thumbnail with long edge at most 256 px.

---

## 3. Filter primitives (shared)

### 3.1 Gaussian blur G_s
* Separable, per-channel, **boundary = reflect with edge sample repeated** (scipy "reflect":
  `d c b a | a b c d | d c b a`), unless stated otherwise below.
* `sigma` can be a scalar (same for all three channels) or a length-3 vector (one per channel).
* `sigma <= 0` returns a copy (identity).
* Reference implementation has two interchangeable algorithms with dispatch at `s = 3.0`:
  * `s < 3`: truncated FIR. Radius `r = floor(truncate * s + 0.5)` with `truncate = 3.0`
    (default); weights `w_i = exp(-i^2 / (2 s^2))` for `i = -r..r`, normalised to sum 1.
    For `s < ~0.167` the radius is 0 and the filter is the identity.
  * `s >= 3`: Young and van Vliet (2002) third-order recursive (IIR) Gaussian, forward then
    backward sweep, one horizontal pass then one vertical pass.
    Coefficients: `q = 0.98711 s - 0.96330` (s >= 2.5), else `q = 3.97156 - 4.14554 sqrt(1 - 0.26891 s)`;
    `b0 = 1.57825 + 2.44413 q + 1.4281 q^2 + 0.422205 q^3`,
    `b1 = 2.44413 q + 2.85619 q^2 + 1.26661 q^3`, `b2 = -(1.4281 q^2 + 1.26661 q^3)`,
    `b3 = 0.422205 q^3`, gain `B = 1 - (b1 + b2 + b3)/b0`, normalised taps `(b1,b2,b3)/b0`.
    Recurrence `w[n] = B x[n] + (b1/b0) w[n-1] + (b2/b0) w[n-2] + (b3/b0) w[n-3]`.
    Boundary: initial state = first sample replicated (forward) and the last forward output
    replicated (backward). Max error vs analytic Gaussian is about 1e-3 relative; this is an
    **acceleration option**: a C++ port may use an exact (FIR or FFT) Gaussian everywhere and
    stay within the reference's own approximation error. (If s < 0.5 the IIR is never used.)
* The truncation of the FIR at 3 sigma is part of the reference result for small sigmas
  (the unsharp mask, glare blur, grain blur use this path); a 4-5 sigma exact kernel differs by
  < 0.3 percent of weight.

### 3.2 Exponential blur E_l (Gaussian-mixture surrogate)
Target PSF (2D isotropic exponential, unit integral): `K(r) = exp(-r/l) / (2 pi l^2)`.
The reference does **not** convolve with this exactly; it approximates it by a sum of three
Gaussian blurs (default `n_gaussians = 3`):

`E_l * I  ~=  sum_k a_k  G_{c_k * l} * I`

| k | a_k | c_k (sigma / l) |
|---|---|---|
| 1 | 0.1633 | 0.5360 |
| 2 | 0.6496 | 1.5236 |
| 3 | 0.1870 | 2.7684 |

(a_k sum to 1.0 - energy preserving; per-axis variance of the mixture = 2.988 l^2 vs 3 l^2 for
the true exponential.) A 2-component variant exists (a = 0.6235, 0.3765; c = 0.9401, 2.5177)
but is not used by default. Each component uses the dispatch of 3.1. `l` may be a per-channel
vector. The surrogate must be reproduced (not the exact exponential) for numerical parity.

### 3.3 FFT / direct convolution (diffusion filter only)
The diffusion filter (section 4.2) convolves with a per-channel PSF sampled on a pixel grid
using FFT convolution (`same` size) on an image reflect-padded by the kernel radius with
**numpy-style reflect (edge sample NOT repeated, `d c b | a b c d | c b a`)**. Any exact linear
convolution implementation is acceptable (direct, FFT, or Gaussian-mixture surrogate).

---

## 4. Camera-side (film exposure) effects

All steps below operate on `raw` (linear film exposure, 3 channels, after the `2^EV` gain).

### 4.1 Highlight boost
Image-global, deterministic, per-pixel nonlinear curve that "reconstructs" clipped highlights
before blur propagation. Applied **whether or not halation is active**, always before
the diffusion filter. Default `boost_ev = 0` which disables it completely.

Parameters (stored in the halation parameter group): `boost_ev` M (stops, default **0.0**,
must be >= 0), `boost_range` A (default **0.3**, must be in [0,1]), `protect_ev` P (default
**4.0**, >= 0), reference mid-grey (constant) `m = 0.184`.

Let `X = max over all pixels and all channels of raw` (image-global; if `X = 0` output is all
zeros). Let `x0 = clamp(m * 2^P, 0, X)`. If `x0 = X` the image is unchanged.

`a = 28^(1 - A)`, `u = x0 / X`, `den = exp(a (1 - u)) - a (1 - u) - 1`, `k = (2^M - 1) / den`.

For each sample `v` (every channel independent):
* `v <= x0`: `y = v`
* `v > x0`: `d = (v - x0)/X`, `y = v + X k (exp(a d) - a d - 1)`

Properties: continuous with zero slope at `x0`; the maximum value `X` maps to `X * 2^M` (exactly
M stops of boost for the brightest sample). Because it normalises by the image-wide maximum,
it is not a pure per-pixel transform (cannot be baked into a 3D LUT; reference disables it in
LUT mode). Raise an error on invalid parameter ranges (denominator <= 0).

### 4.2 Diffusion filter (camera: before lens blur; enlarger: see 6.1)
Models Pro-Mist/Glimmerglass/CineBloom-style filters. Energy-conserving convex combination:

`E_out = (1 - p) E_in + p (K * E_in)`

with per-channel unit-sum PSF `K` (`K_R, K_G, K_B` differ only in the halo's colour weighting)
and scatter fraction `p` from the strength.

**Defaults** (`DiffusionFilterParams`): `active = false`, `filter_family = black_pro_mist`,
`strength = 0.5`, `spatial_scale = 1.0`, `halo_warmth = 0.0`, and six per-group multipliers all
1.0 (`core_intensity, core_size, halo_intensity, halo_size, bloom_intensity, bloom_size`).
The effect is skipped if `active` is false, `strength <= 0`, `spatial_scale <= 0`, or `p <= 0`.
The camera and the enlarger each have their own independent instance of these parameters
(camera: `camera.diffusion_filter`; enlarger: `enlarger.diffusion_filter`).

**Strength to scatter fraction.** Piece-wise linear interpolation in `log2(strength)`:

| strength (stops) | 0.125 | 0.25 | 0.5 | 1 | 2 |
|---|---|---|---|---|---|
| base fraction | 0.10 | 0.20 | 0.35 | 0.55 | 0.75 |

Outside [0.125, 2] the value clamps to the end values (strength is clipped below at 1e-6 before
the log). Then `p = clip(base * gain_family, 0, 0.99)` with family gains:
glimmerglass 0.65, black_pro_mist 0.75, pro_mist 1.05, cinebloom 1.00.

**PSF structure.** `K_c(r) = w_c K_core(r) + w_h K_halo,c(r) + w_b K_bloom(r)`, with
`w_c + w_h + w_b = 1`. Each group is a weighted sum of 2D exponentials
`E(r; l) = exp(-r/l)/(2 pi l^2)` (l in um, r in um; with unit integral).

Group sub-components: `n` decay constants spaced geometrically (uniform in `ln l`) between
`l0/spread` and `l0*spread` inclusive (`n = 1` or `spread <= 1` gives just `l0`).
Within-group weights are normalised to sum 1:
* core, halo: uniform weights;
* bloom: `w_k proportional to l_k^(2 - alpha)` (so the sum tends to a power-law tail `r^-alpha`).

Family table:

| family | core l0 (um), spread, n | halo l0, spread, n | bloom l0, spread, n, alpha | w_c, w_h, w_b | halo_warmth_base |
|---|---|---|---|---|---|
| glimmerglass | 10, 1.5, 2 | 50, 2.0, 3 | 260, 2.5, 4, 3.2 | 0.60, 0.30, 0.10 | 0.0 |
| black_pro_mist | 16, 1.5, 2 | 95, 2.0, 3 | 380, 2.5, 4, 3.5 | 0.40, 0.47, 0.13 | 0.65 |
| pro_mist | 14, 1.5, 2 | 150, 2.0, 3 | 650, 2.5, 4, 2.9 | 0.28, 0.42, 0.30 | 0.40 |
| cinebloom | 20, 1.5, 2 | 200, 2.0, 3 | 1000, 2.5, 4, 2.5 | 0.22, 0.30, 0.48 | 0.85 |

Per-group multipliers: `w_x <- w_x * max(intensity_x, 0)`, then `w_c, w_h, w_b` renormalised to sum
1 (if the total is <= 0, keep the family values); `l0_x <- l0_x * max(size_x, 1e-6)`.
`spatial_scale` multiplies every `l` (floor 1e-6).

**Halo colour (warmth).** `W = halo_warmth_base + halo_warmth` (user value is additive),
then clamped to [-1.5, +1.5]. Halo sub-component weights `v_k` (sum `S`, the uniform weights)
are redistributed per channel: let `g_k` run linearly from -1 (smallest l) to +1 (largest l),
then shifted so that `sum_k v_k g_k = 0`. Channel coefficients `a = (+1.30, +0.15, -1.45)` for
(R,G,B). Per channel: `v'_k = max(0, v_k (1 + W a_c g_k))`, rescaled so `sum_k v'_k = S`.
(If a channel's sum is 0, fall back to the original weights.) With W > 0 red energy shifts
toward the outer halo and blue toward the inner halo. Core and bloom are channel-independent.
With a single halo component (n < 2) there is no redistribution.

**Discretisation (what the reference actually does).**
1. `l_max_px = bloom.l0 * bloom.spread * spatial_scale / pixel_size_um` (with size/intensity
   overrides applied).
2. Kernel radius `R = ceil(max(8 * l_max_px, 5))`, then `R = min(R, max(floor(min(H,W)/2) - 1, 1))`.
   Kernel is `(2R+1) x (2R+1)`, centre at index `R`.
3. Each channel's kernel value at integer offset (dx, dy) is the **point sample** of
   `w_c * sum exp(-r/l_k)/(2 pi l_k^2) + ...` at `r = sqrt(dx^2+dy^2)` (pixels, l in pixels,
   `l_px = l_um * spatial_scale / pixel_size_um`, floor 1e-6), **not** pixel-integrated; the
   kernel is then divided by its own sum so it integrates to exactly 1 on the grid (this also
   re-normalises truncation loss).
4. Image is reflect-padded (numpy reflect) by R, convolved per channel with its kernel (same
   size output), cropped back; `E_out = (1-p) E_in + p * blurred`.
When `l_px` of a component is < ~0.5 px the point-sampling makes it nearly a delta function at
the centre tap; the port should keep the point-sample + renormalise rule for parity.

### 4.3 Camera lens blur
`sigma_px = lens_blur_um / pixel_size_um`; `E_out = G_sigma * E_in` (per channel, same sigma).
Parameter `camera.lens_blur_um` default **0.0** (disabled; skipped when <= 0). Applied after the
diffusion filter and before halation.

### 4.4 Halation block (in-emulsion scatter + back-reflection halation)
Skipped entirely if `halation.active = false` (default true). Runs as two sequential passes on
the raw (linear) exposure `E0`.

**Pass 1 - in-emulsion scatter** (energy-preserving mixture of a Gaussian core and an
exponential tail, per channel c). Defined with scalars `s = scatter_amount`,
`S = scatter_spatial_scale`, per-channel `sigma_c` (core, um), `l_t` (tail decay, um),
`w_t` (tail weight):

`sigma_px = sigma_c * S / pixel_size_um`, `l_px = l_t * S / pixel_size_um`
`scattered = (1 - w_t) G_{sigma_px} * E0 + w_t E_{l_px} * E0`   (E as in 3.2, per channel params)
`E1 = (1 - s) E0 + s * scattered`

Executed only if `s > 0` and at least one `sigma_px` or `l_px` is positive; per-channel sigmas
floored at 1e-6 (floor = identity filter). Total energy preserved (unit-sum kernels).

**Pass 2 - back-reflection halation** (additive, multi-bounce, then optionally renormalised).
Parameters: per-channel `halation_strength` `a_tot` (times `halation_amount`), per-channel
first-bounce sigma `sigma_h` (um, times `halation_spatial_scale`), `N = halation_n_bounces`,
`rho = halation_bounce_decay`.

* weights `d_k = rho^(k-1) / sum_{j=1..N} rho^(j-1)`, k = 1..N (N=3, rho=0.5: 4/7, 2/7, 1/7).
* `sigma_k_px = max(sigma_h_px * sqrt(k), 1e-6)`.
* `H = sum_k d_k G_{sigma_k_px} * E1` (per channel own sigma).
* `E2 = E1 + a_tot * H`, per channel. If `halation_renormalize` (default true):
  `E2 = E2 / (1 + a_tot)` per channel.
* Skipped unless `N >= 1`, some `a_tot > 0` and some `sigma_h_px > 0`.

With renormalisation the pass conserves total energy exactly; without it adds `a_tot` times the
image energy.

**Defaults** (`HalationParams`): `active=true`, `scatter_amount=1.0`, `scatter_spatial_scale=1.0`,
`halation_amount=1.0`, `halation_spatial_scale=1.0`,
`scatter_core_um=(2.2, 2.0, 1.6)`, `scatter_tail_um=(9.3, 9.7, 9.1)`,
`scatter_tail_weight=(0.78, 0.65, 0.67)`, `boost_ev=0`, `boost_range=0.3`, `protect_ev=4.0`,
`halation_strength=(0.05, 0.015, 0.0)`, `halation_first_sigma_um=(65,65,65)`,
`halation_n_bounces=3`, `halation_bounce_decay=0.5`, `halation_renormalize=true`.

**Per-stock presets (override of strength and sigma_h at digest time).** For profiles that are
film (`support = film`) the profile's `use` (still/cine) and `antihalation` (strong/weak/no)
fields select (these replace the schema defaults above; paper profiles are not changed):

| use, antihalation | sigma_h (um, R,G,B) | halation_strength (R,G,B) |
|---|---|---|
| still, strong | 65,65,65 | 0.015, 0.005, 0.0 |
| still, weak | 65,65,65 | 0.08, 0.02, 0.0 |
| still, no | 65,65,65 | 0.30, 0.10, 0.015 |
| cine, strong | 50,50,50 | 0.015, 0.005, 0.0 |
| cine, weak | 50,50,50 | 0.08, 0.02, 0.0 |
| cine, no | 50,50,50 | 0.30, 0.10, 0.015 |

Physical rationale: sigma_h set by base thickness (triacetate 120-140 um -> 65 um; PET cine
base -> 50 um); strength from antihalation layer quality. Per-stock `use`/`antihalation` values in
the shipped profiles: `weak` for fujifilm_c200, fujifilm_xtra_400, kodak_gold_200,
kodak_ultramax_400, kodak_kodachrome_64; `strong` for all other film stocks (portra series,
ektar, pro_400h, provia, velvia, ektachrome, vision3 series, 2383, 2393, verita);
`use = cine` for kodak_2383, 2393, verita_200d, vision3_50d/200t/250d/500t; all others `still`.
Paper profiles carry the tags but are not used for halation.

---

## 5. Development-side effects

### 5.1 DIR-coupler inhibitor diffusion (spatial part and its chemistry)
Executed inside `develop`, after the ordinary density-from-exposure lookup and before grain.
Operates in the **log-exposure** domain (log10 of raw). Disabled (density returned unchanged)
if `dir_couplers.active = false`.

Notation: `le[i]` = film profile `log_exposure` axis (256 points, -3.0 .. +4.0, uniform step
0.02745), `D_m(le)` = density curves (N x 3) **after subtracting the per-channel minimum**
(`normalized curves`), `gamma` = `density_curve_gamma` (default 1.0; x axis is `le/gamma`),
`Dmax_m = nanmax_i D_m`.

1. **Coupler matrix** `M[k, m]` (donor layer k releases inhibitor that reduces exposure of
   receiver layer m). Diagonal `M[k,k] = gamma_samelayer_rgb[k] * inhibition_samelayer`.
   Off-diagonals, all multiplied by `inhibition_interlayer`:
   `M[0,1]=r_to_gb[0]`, `M[0,2]=r_to_gb[1]`, `M[1,0]=g_to_rb[0]`, `M[1,2]=g_to_rb[1]`,
   `M[2,0]=b_to_rg[0]`, `M[2,1]=b_to_rg[1]`. The whole matrix is then multiplied by `amount`.
2. **Curves before couplers** (so that grey ramps reproduce the profile curves). Silver-like
   quantity `T_k(le) = D_k(le)` for negative films, `T_k(le) = Dmax_k - D_k(le)` for positive
   (reversal) films. Shift `c_m(le) = sum_k T_k(le) M[k,m]`; `le0_m(le) = le - c_m(le)`.
   Pre-coupler curve `D0_m(le) = interp(le; xp = le0_m, fp = D_m)` (negative) or
   `D0_m = -interp(le; xp = le0_m, fp = -D_m)` (positive); linear interpolation with end
   clamping, `le0_m` assumed increasing.
3. **Per pixel** : `D(x)` = density from the original `log_raw` (normalised curves, gamma).
   `T(x)` as above from `D(x)`, plus `shift * T^2` with `shift = 0` (not exposed). Inhibitor
   field `q_m(x) = sum_k T_k(x) M[k,m]` (per pixel, 3 channels).
4. **Spatial diffusion** of `q` (only if `diffusion_size_um > 0`; otherwise no blur and the
   tail is ignored): `sigma_px = diffusion_size_um/pixel_size_um`, `l_px = diffusion_tail_um/pixel_size_um`;
   `q' = (1 - w) G_sigma * q + w E_l * q` with `w = diffusion_tail_weight`; Gaussian and
   exponential per 3.1/3.2 (per channel same parameters).
5. `log_raw_0 = log_raw - q'`; final density = lookup of `log_raw_0` in the pre-coupler curves
   `D0` (x axis `le/gamma`, linear interpolation, clamped ends).

**Defaults** (`DirCouplersParams`): `active=true`, `amount=1.0`, `inhibition_samelayer=1.0`,
`inhibition_interlayer=1.0`, `diffusion_size_um=20.0`, `diffusion_tail_um=200.0`,
`diffusion_tail_weight=0.06`. Gamma matrix fields are **overridden at digest time by film type
and stock** (the schema defaults 0.341/0.324/0.273 etc. are not what runs):

| Film | samelayer rgb | r_to_gb | g_to_rb | b_to_rg |
|---|---|---|---|---|
| any negative | 0.336, 0.319, 0.273 | 0.353, 0.302 | 0.154, 0.353 | 0.168, 0.226 |
| any positive (reversal) | 0.12, 0.08, 0.06 | 0.12, 0.06 | 0.08, 0.06 | 0.06, 0.06 |
| fujifilm_velvia_100 | 0.108, 0.072, 0.054 | 0.108, 0.054 | 0.072, 0.054 | 0.054, 0.054 |
| fujifilm_provia_100f | 0.156, 0.104, 0.078 | 0.156, 0.078 | 0.104, 0.078 | 0.078, 0.078 |

(Stock overrides apply after the type defaults.)

### 5.2 Grain

Stochastic, per-pixel, applied to the (post-DIR) CMY density of the **film** only. Two code
paths: the default sub-layer path (`sublayers_active = true`) and a simpler single-layer path.
The default path is specified first.

**Defaults** (`GrainParams`): `active=true`, `sublayers_active=true`, `particle_area_um2=0.2`,
`particle_scale=(1.6, 1.6, 3.2)` (R,G,B), `particle_scale_layers=(2.0, 1.0, 0.5)`,
`density_min=(0.03, 0.03, 0.03)`, `uniformity=(0.97, 0.99, 0.97)`, `blur=0.65` (pixels),
`blur_dye_clouds_um=1.0`, `micro_structure=(0.2, 30)` (um, nm), `n_sub_layers=1`.
Also `settings.use_fast_stats = false` (selects exact vs approximate integer samplers).

#### 5.2.1 Core statistical model for one layer ("layer particle model")
Inputs per pixel: layer density `D` (already offset by the density floor, see 5.2.2), layer
maximum density `Dmax`, mean particle count per pixel `n`, uniformity `u`.

```
p   = clip(D / Dmax, 1e-6, 1 - 1e-6)           development probability
s   = 1 - p * u * (1 - 1e-6)                    "saturation" factor (in (0,1])
N   ~ Poisson( n / s )                          particles in the pixel, per pixel independent
K   ~ Binomial( N, p )                          developed particles
g   = K * (Dmax / n) * s                        grain density value for this layer
```
**Exact distribution:** Poisson thinning makes `K ~ Poisson(n p / s)`, so `g` is a scaled
Poisson variable. A port can therefore draw a single Poisson with mean `lambda = n p / s`
and set `g = (Dmax s / n) * Poisson(lambda)` (statistically identical, one sampler).
Moments (per pixel, before any blur):
* `E[g] = p Dmax = D` (clip aside): **mean density is preserved by construction**, for every
  `u` and `n`; no post-hoc renormalisation is applied.
* `Var[g] = Dmax * D * s / n`. For `u = 1`, `s = 1 - p` (binomial-like, variance vanishes at
  full development); for `u = 0`, `s = 1` (Poisson-like, variance `Dmax D / n`).
  `uniformity` therefore suppresses noise in dense regions: higher `u` -> smaller `s` at high `p`.
* Skewness / kurtosis are those of the scaled Poisson (skew `1/sqrt(lambda)`).
Draws are independent across pixels, across layers and channels (different RNG streams).
The reference seeds the global RNG with `seed = channel + 10 * sublayer_index`
(channel 0,1,2 -> 0,1,2; sublayer j adds 10 j) before each layer call, so **default output is
deterministic ("frozen" grain pattern, identical for every render of equal size)**. Because
only statistics matter for the comparison, any RNG is acceptable; compare means and
variances (e.g. local std over flat patches), not pixel values.

If `blur_particle > 0` (used by the sub-layer path) the per-layer grain field `g` is blurred by
a Gaussian of `sigma_px = blur_particle * sqrt(Dmax / n)` pixels. Note that with
`n = pixel_area * f / a_p` (5.2.2) this equals `sigma_um = blur_dye_clouds_um * sqrt(Dmax_l a_p / f)`
divided by pixel size, i.e. a **fixed physical dye-cloud size independent of resolution**
(equivalently `sigma_px = blur_dye_clouds_um * sqrt(Dmax_l a_p / f) / pixel_size_um`). For
typical 35 mm/6000 px this sigma is 0.1-0.4 px and mostly resolves to the small FIR path.

Fast approximate samplers (option, `use_fast_stats`, off by default): Poisson with mean < 30:
Knuth multiplication method; mean >= 30: `round(lambda + sqrt(lambda) z)` clamped >= 0 with
`z ~ N(0,1)`. Binomial with n < 25: n Bernoulli trials; else if `n p (1-p) > 10`:
`round(n p + sqrt(n p (1-p)) z)` clamped to [0, n]; else sequential inversion. They only
approximate the exact distributions; using the exact Poisson-thinning form above is preferred.

#### 5.2.2 Sub-layer (default) path
Each colour channel `ch` (C,M,Y index 0,1,2) is modelled as 3 sub-layers (fast / medium / slow
emulsion layers: index 0 has the largest particles).

Inputs: total density `D_ch(x)` (post-DIR, `develop` passes the min-subtracted normalised
curves); profile `density_curves_layers` (N x 3 sub-layers x 3 channels, same `log_exposure`
axis; the three sub-layer curves of a channel sum to its total curve; per-sub-layer max
for Portra 400, for example, R: 0.597, 0.691, 0.703).

1. Per-sub-layer density `D_l(x)` by **inverse lookup**: map the pixel's total density `D_ch(x)`
   through the total-density curve of that channel (monotone, x axis = curve values) to the
   sub-layer curve value (linear interpolation, clamped at ends). For positive films both axes
   are negated so the axis is still increasing.
2. `Dmax_l[l,ch] = nanmax over exposure of density_curves_layers[:, l, ch]`;
   `f[l,ch] = Dmax_l[l,ch] / sum_l Dmax_l[l,ch]`.
3. Density floor: `dmin_l[l,ch] = f[l,ch] * density_min[ch]`; `D_l <- D_l + dmin_l`;
   `Dmax_l <- Dmax_l + dmin_l`.
4. Particle counts: pixel area `A = pixel_size_um^2`;
   `a_p[l,ch] = particle_area_um2 * particle_scale[ch] * particle_scale_layers[l]`;
   `n[l,ch] = A * f[l,ch] / a_p[l,ch]` (real-valued, may be < 1 or large; for small `n` the
   Poisson noise is large).
5. Per layer, per channel, draw `g[l,ch]` by 5.2.1 with `Dmax = Dmax_l[l,ch]`,
   `n = n[l,ch]`, `u = uniformity[ch]`, `blur_particle = blur_dye_clouds_um`.
6. Channel density `G_ch = sum_l g[l,ch]` (sum, no division; `E[G_ch] = D_ch + density_min_ch`).
7. **Micro-structure** (multiplicative clumping): `b = micro_structure[0] / pixel_size_um`
   (px), `t = micro_structure[1] * 0.001 / pixel_size_um` (nm -> um -> px; used as a
   relative standard deviation). If `t > 0.05`: draw a per-pixel, per-channel i.i.d.
   lognormal field with mean 1 and linear standard deviation `t`
   (log-space `sigma^2 = ln(1 + t^2)`, `mu = -sigma^2/2`), blur it by `G_b` if `b > 0.4`,
   and multiply `G` by it. Otherwise skipped. With defaults and pixel size > 0.6 um
   (ordinary renders) this never triggers; it only matters for heavy upscaling (pixel size
   below ~0.6 um).
8. Remove the floor: `G_ch <- G_ch - density_min[ch]`. Output density can be slightly negative
   (>= -density_min in the Poisson-zero limit), which is why film black references elsewhere
   use `-density_min`.
9. Final **grain blur**: if `blur > 0`, `G <- G_{blur} * G` (all three channels, scalar sigma
   in pixels, default 0.65 px). Pixel units: it does not scale with pixel size.

Worked example (not a requirement, for sanity checks): Portra 400, 6000 px, 5.833 um/px,
A = 34.03 um^2, R channel `f = (0.300, 0.347, 0.353)`, `a_p = (0.64, 0.32, 0.16)`,
`n ~ (15.9, 36.9, 75.1)` particles/px.

#### 5.2.3 Single-layer path (`sublayers_active = false`)
Per channel: `Dmax = nanmax(normalised curve) + density_min`; `n = A / (particle_area_um2 *
particle_scale[ch])` divided by `n_sub_layers` if > 1; `D <- D + density_min`; per sub-layer
(`n_sub_layers`, default 1) draw via 5.2.1 (no particle blur) with seeds `ch + 10 sl`, average
over the sub-layers, subtract `density_min`; blur by `blur` only if `blur > 0.4`. No
micro-structure, no per-layer decomposition. (The schema defaults for `particle_scale`,
`uniformity` etc. are shared; note the schema defaults are the sub-layer calibration.)

#### 5.2.4 Relationship to physical units
Pixel area is `pixel_size_um^2`; halving pixel size quarters `n` (so per-pixel noise rises by
2x in std relative) while the same physical grain per area is retained when rescaled
to a common size. Grain is not defined for the print stage (print develops without grain).

---

## 6. Print-stage and scan-stage effects

### 6.1 Enlarger diffusion filter
Same operator as 4.2 (own parameters, same defaults, `active=false`), applied to the print raw
exposure **after** film-density-to-print-exposure conversion, after the print exposure
multiplier and the black/white print correction scalar, **before** `log10`. Same pixel size
as the film (the enlarger projects the whole frame, pixel size unchanged).

### 6.2 Print glare (scan of print only)
Models veiling light / viewing flare as a random, spatially correlated additive stray light.
Not applied for film scans. `GlareParams` defaults: `active=true`, `percent=0.03`,
`roughness=0.7`, `blur=0.5` (pixels).

1. Per-pixel i.i.d. lognormal field `L` with linear-space mean `percent` and linear standard
   deviation `roughness * percent` (log-space `sigma^2 = ln(1 + roughness^2)`,
   `mu = ln(percent) - sigma^2/2`; degenerate -> constant if std tiny).
   One scalar field (shared by all channels and by X, Y, Z).
2. Blur by `G_blur` (0.5 px; mean preserved), then divide by 100:
   `glare(x) = (G_blur * L)(x) / 100` (so default mean is 0.0003 = 0.03 percent of white).
3. `XYZ(x) <- XYZ(x) + glare(x) * XYZ_white`, where `XYZ_white` is the viewing-illuminant
   white with `Y = 1` (illuminant spectrum times CMFs, divided by the integral of
   illuminant * ybar). Applied **after** the black/white XYZ correction and **before** the
   XYZ -> RGB conversion.
Skipped if inactive or `percent <= 0`. Note glare is a different random number draw each
render (no seed in the reference): use statistical comparison only. The mean added flare is tiny
at default (3e-4 of white), affecting deep shadows only.

### 6.3 Scanner blur and unsharp mask
On the linear output RGB after gamut compression (before the transfer function):
1. `scanner.lens_blur` (pixels, default **0.0**): `rgb <- G_s * rgb` if > 0.
2. Unsharp mask `scanner.unsharp_mask = (sigma, amount)`, default **(0.7, 0.7)** (sigma in
   pixels): applied only if `sigma > 0` and `amount > 0`:
   `out = rgb + amount * (rgb - G_sigma * rgb)` (per channel, no clamping, negative lobes
   allowed). With sigma 0.7 the FIR path is used with radius floor(3*0.7+0.5)=2, i.e. 5 taps.

Scanner `white_correction`, `black_correction` (defaults false), `white_level` 0.98,
`black_level` 0.01 are global scalars (not spatial) and belong to another spec.

---

## 7. Overrides applied by the parameter digest ("modes")

* **preview_mode** (`settings.preview_mode`): enlarger lens blur 0; DIR `diffusion_size_um`
  0 (which also disables the tail); grain inactive (and `particle_area_um2=0`, `blur=0`);
  glare blur 0; camera lens blur 0; scanner lens blur 0; unsharp mask (0,0). Scatter and halation
  kernels are **kept**. Camera/enlarger diffusion filters and boost are kept.
* **lut_mode** (`debug.lut_mode`): forces both deactivation groups below, auto-exposure off,
  exposure compensation 0, print exposure 1 and compensation off, `boost_ev=0`, scanner
  black/white correction off, unsharp (0,0).
* **deactivate_spatial_effects**: halation inactive with all scatter/halation sigmas zero; DIR
  diffusion size 0; grain blur 0; dye-cloud blur 0; glare blur 0; camera and enlarger lens blur
  0; both diffusion filters inactive; scanner blur 0; unsharp (0,0).
* **deactivate_stochastic_effects**: grain inactive, glare inactive.
Boost (when active) is image-global so it is not part of deterministic LUT modes.

---

## 8. Data files used by these effects, with provenance and licence

| Data | Location | Used for | Licence | Commercial closed-source use |
|---|---|---|---|---|
| Film profile JSON (`density_curves`, `density_curves_layers`, `log_exposure`, `info.use`, `info.antihalation`, `info.type`) | `src/spektrafilm/data/profiles/*.json` | grain sub-layer curves, DIR curves, per-stock halation preset selection | CC BY-SA 4.0 (author Andrea Volpato; preamble in `data/license/SPEKTRAFILM_LICENSE.txt`) | Allowed in principle (CC BY-SA permits commercial use) **but** ShareAlike: any derived/adapted data (including a re-encoded or LUT version) must itself be shared under CC BY-SA 4.0 with attribution; the app binary is not necessarily "adapted material" but a bundled derived data file is. Attribution everywhere required. The author also asks (non-binding) not to repackage as a paid LUT pack, not to train commercial AI on them, and not to use the name in branding. |
| Scalar render constants in code (all tables in sections 3-7) | Python source | effect parameters | Code is GPLv3; numeric default values are facts, but see Open points | Re-deriving constants and writing own code from this spec is the clean-room route; do not copy the source. |
| Spectral-upsampling LUTs, filters (dichroics, heat absorbing, lens transmission), ICC profiles | `data/luts`, `data/filters`, `data/icc` | **not used by any spatial effect** (covered by other specs) | LUTs: CC BY-SA/"commercial use, free share, no resale" custom licence per README; ICC: Elle Stone profiles CC BY-SA 3.0, saucecontrol profiles under their own licence (see `data/icc/README.md`); `hanatos_irradiance_xy_coeffs_250304.lut` is derived from hanatos/vkdt work (upstream licence to be confirmed in the spectral-upsampling spec). | Not needed for this step. |

No data file is read by the grain, halation, diffusion, glare or unsharp operators themselves.
The only profile-derived inputs are the density curves and the per-stock `use` / `antihalation`
tags above.

---

## 9. Suggested numerical validation strategy
1. Deterministic effects (boost, diffusion, scatter, halation, DIR diffusion, unsharp, blur):
   feed impulses / edges at a known `pixel_size_um` and compare to the reference output;
   expect agreement to ~1e-3 relative (IIR vs exact Gaussian, Gaussian-mixture surrogate for
   exponentials must be kept).
2. Grain: compare per-channel mean (must equal input density within sampling error) and local
   standard deviation over flat patches at several densities (formula `Var = Dmax D s / n`, then
   reduced by the two blurs); compare power spectra, not pixel values.
3. Glare: mean of added stray light equals `percent/100 * XYZ_white`.
4. For grain-in-the-loop DeltaE2000 comparisons, either disable stochastic effects on both sides
   or compare after averaging many seeds / large-area means.

---

## 10. Open points

1. Highlight boost mid-grey constant `0.184` is assumed to be the raw-domain mid-grey in
   the film-raw units. In the reference, raw units are whatever spectral integration produced
   (an 18.4 percent grey input does not necessarily give raw = 0.184). Behaviour is documented as
   implemented; whether it is the intended protect point is unknown. Boost is off by default so
   it does not affect the default comparison.
2. The Young-van Vliet IIR boundary is replicate-the-edge-sample, not the scipy reflect used by
   the FIR path; edge pixels may differ by up to a few 1e-3 for sigma >= 3 px (halation,
   DIR diffusion, tail Gaussians). Image borders may need excluding from a tight comparison.
3. The `fast_exponential_filter` Gaussian-mixture constants are marked as "placeholder fits"
   by the author and may be refined upstream; pin the version of the reference used for the
   comparison.
4. Grain seed behaviour: default is the fixed seed (0,1,2 + 10 j) so output is frozen, but with
   `use_fast_stats = true` the numba samplers are not seeded from that call. The scipy
   sampling stream cannot be reproduced bit-exactly in C++; only statistics match.
   Glare is unseeded.
5. The dye-cloud blur `sigma = blur_dye_clouds_um * sqrt(Dmax/n)` is dimensionally "um times
   dimensionless" but is used as pixels; section 5.2.1 shows it is equivalent to a fixed
   physical size. Confirm with the author whether this is intentional.
6. `grain.blur` is in pixels (default 0.65 px) and the unsharp-mask / scanner blur / glare blur
   are also in pixels; the result therefore depends on render resolution. Upstream may later
   move these to um.
7. The enlarger `lens_blur` parameter is dead code in this version. Check the version
   comparison target for a later implementation.
8. `density_curves_layers` are used un-normalised (not min-subtracted) while `density_curves` are
   min-subtracted before the inverse lookup; tiny negative minima (about -7e-4 in Portra 400)
   therefore produce a small clamp at the toe. Reproduce that behaviour.
9. Skimage bicubic `rescale` details (anti-aliasing flag, boundary mode) for `upscale_factor != 1`
   are not specified here; only relevant for upscaled renders.
10. Diffusion-filter pad mode (numpy reflect without edge repeat) differs from the Gaussian
    paths (scipy reflect with edge repeat); effect is limited to borders.
11. Licensing: the README states that code and anything "directly inspired by its methods" is
    GPLv3 and that JSON profiles/LUTs are CC BY-SA 4.0 (the README also mentions a "strict
    commercial use, free share, no resale" custom licence for LUTs). The legal status of
    re-implementing documented *ideas and numeric defaults* from a description (this file) is a
    question for counsel; this document records functional behaviour and constants only.
    Upstream explicitly offers alternative licensing by contact for GPL-incompatible projects.
12. Web check of upstream (hanatos/vkdt) licence for the spectral-upsampling coefficient LUT was
    not performed in this step (that file is outside the spatial-effects scope).
