# 02 - Printing, scanning and LUT acceleration (functional specification)

Clean-room functional specification of the second half of the reference film-simulation
pipeline: the virtual colour enlarger and paper (print), the virtual scanner (conversion of
the finished print, or of the film itself, to display RGB), and the look-up-table (LUT)
acceleration that replaces per-pixel spectral integration.

This document contains no reference source code. It gives equations, tables of constants,
behavioural descriptions and data-file schemas. Numeric constants and defaults are facts
transcribed from the reference. Every formula in sections 3 and 4 and the LUT behaviour in
section 6 was re-implemented independently from this text (numpy, no reference code) and
compared with the reference on test pixels: agreement was about 1e-5 absolute in linear
sRGB (section 8). The stages before this document (RGB -> film exposure -> developed film
density, spatial effects, grain) are specified in other documents; this one starts at the
developed film density and ends at output RGB.

Conventions: `lambda` = wavelength in nm; the spectral grid is 380..780 nm in 5 nm steps
(81 samples, index `w`); `D` = optical density (log10); `T = 10^-D`; images are H x W x 3;
density triplets are ordered (C, M, Y) = the same index order as the (R, G, B) exposure
channels that formed them (red-sensitive layer -> cyan dye, green -> magenta, blue ->
yellow); `E(lambda)` = illuminant spectrum; `cmf(lambda)` = CIE 1931 2-degree colour
matching functions (xbar, ybar, zbar) sampled on the same grid (table in Appendix A).

---

## 1. Data flow overview

```
                    developed film density  d_f = (C,M,Y) per pixel        [after grain; may dip to about -0.03]
                                   |
            +----------------------+------------------------------------+
            | io.scan_film = false (default: "print path")              | io.scan_film = true ("no print")
            v                                                           v
  [P1] film density -> spectral density  D_f(w) = base_f(w) + sum_k dye_f(w,k) d_f,k          |
  [P2] light through negative under enlarger source:  L(w) = 10^-D_f(w) * E_enl(w) * Tfilt(w)  |
  [P3] paper raw exposure  R_l = sum_w L(w) S_l(w)         (l = paper layer R,G,B)             |
  [P4] x exposure normaliser (mid-gray geometric mean), + preflash                             |
  [P5] x print_exposure, x black/white exposure correction (optional)                          |
  [P6] enlarger diffusion filter (spatial, see doc 03)                                         |
  [P7] logE = log10(max(R,0)+1e-10)                                                            |
  [P8] paper development: logE -> paper density d_p (C,M,Y)  (parametric curves)               |
            |                                                           |
            v                                                           v
  [S1] density -> spectral -> XYZ under the VIEWING illuminant of the print (or of the film)
  [S2] optional black/white correction on Y
  [S3] viewing glare (print only)
  [S4] XYZ -> linear RGB of the output colour space (chromatic adaptation viewing white -> output white)
  [S5] output gamut compression (default: CAM16-UCS chroma + one-sided lightness roll-off)
  [S6] scanner blur + unsharp mask (spatial, doc 03)
  [S7] output transfer function (default sRGB OETF)
```

Everything between the film density and S5 is a pure per-pixel function of the three film
densities except: the enlarger diffusion filter (P6, spatial), preflash (a constant), and
glare (S3, stochastic, mean-only if made deterministic). This is what makes a 3D LUT
indexed by film density (section 6) a faithful substitute for the whole chain.

Tap names (for implementations that expose internal taps): `rgb_in -> rgb_pre -> log_e_film
-> cmy_film -> log_e_print -> cmy_print -> rgb_out`. `cmy_film` is the developed film density,
`log_e_print` the log10 paper exposure after P7, `cmy_print` the paper density after P8.

---

## 2. Common spectral primitives

### 2.1 Spectral grid and NaN convention
All spectra live on the 81-point grid. Profile data files store `null` for "not measured"
samples (they load as NaN). This matters numerically:

* `channel_density` (81x3) and `base_density` (81) of films and papers contain NaN at the
  ends of the visible range (e.g. Portra 400: channel_density NaN at 380 and 750..780,
  base_density NaN at 380..395 and 705..780; Portra Endura: channel_density NaN at 380..400
  and 700..780, no NaN in base). A wavelength row is invalid if ANY of its three
  `channel_density` entries or its `base_density` entry is NaN (a NaN times a zero density is
  still NaN).
* After forming `T = 10^-D`, every NaN is replaced by 0, so an invalid wavelength
  contributes **zero light** to every integral (paper exposure and scan XYZ). Implement with
  a per-profile validity mask; do not interpolate or extrapolate.
* `log_sensitivity` (81x3) is converted with `S = 10^logS`, NaN -> 0. The tables contain no
  NaN in practice.

### 2.2 Density to transmitted light
For a spectral density `D(w)` and a source spectrum `E(w)`: `L(w) = E(w) * 10^(-D(w))`, NaN -> 0.
Both film transmission and paper "reflection" use this same single-pass formula (a print's
`base_density` already contains the paper white's reflectance loss; there is no separate
paper reflectance and no double pass).

Spectral density of a developed medium with dye densities `d = (d_C, d_M, d_Y)`:

`D(w) = base(w) + dye(w,C)*d_C + dye(w,M)*d_M + dye(w,Y)*d_Y`

`dye(w,k)` = profile `channel_density` (unit-normalised dye absorption spectra, the density
of dye k per unit of its characteristic-curve density), `base(w)` = profile `base_density`
(density of unexposed processed base plus fog, spectral). Note `d` can be slightly negative
for film (grain noise offset, section 3.1); the formula is applied as is.

### 2.3 Illuminants
Each illuminant is a spectrum on the grid, **normalised to unit mean over the 81 samples**:
`E_norm(w) = E(w) / ( (1/81) sum_w E(w) )`. Names accepted by the reference and their
definitions:

| name | definition |
|---|---|
| `D50`, `D55`, `D65` (any CIE daylight / standard-illuminant key of the colour-science library) | tabulated CIE spectrum aligned (trimmed) to 380..780 nm / 5 nm, then normalised |
| `T` | tabulated "Incandescent" light-source SPD (colour-science library), aligned, normalised |
| `K75P` | tabulated "Kinoton 75P" cinema projector lamp SPD, aligned, normalised |
| `BBxxxx` | black body of temperature xxxx K: `E(lambda) ~ lambda^-5 / (exp(c2/(lambda_m T)) - 1)`, `lambda_m` = lambda in metres, `c2 = h c / k = 0.014387758 m K` (h=6.62607e-34, c=299792458, k=1.38065e-23 as used by the reference library); then normalised |
| `TH-KG3` (the default enlarger source) | black body 3400 K **times** Schott KG3 heat-absorbing-glass transmittance (section 2.5, applied with weight 1), then normalised (normalisation is applied once, after the product) |
| `TH-KG3-L` | as `TH-KG3`, additionally multiplied by the transmittance of a Canon 24 mm f/2.8 IS lens (percent data / 100), then normalised. Not used by any default. |

Spectral tables of all illuminants that profiles reference (D50, D55, D65, K75P, T, TH-KG3,
TH-KG3-L), the KG3 transmittance and the lens transmittance on the 81-point grid are
printed in **Appendix A** so the engine can be built without the colour-science library.
The illuminant stored per profile: `info.reference_illuminant` (what the film/paper was
characterised under; used by the filming stage and by the enlarger default) and
`info.viewing_illuminant` (what the finished medium is viewed/scanned under).

| profile class | reference_illuminant | viewing_illuminant |
|---|---|---|
| still negative films | D55 (tungsten cine stocks Vision3 200T/500T: `T`) | D50 |
| positive (slide) films | D55 | D50 |
| photographic papers | TH-KG3 | D50 |
| cinema print films 2383 / 2393 | TH-KG3 | K75P |

### 2.4 Colorimetric normalisation (relative XYZ)
For a transmitted spectrum `L(w)` under viewing source `E_v`:

`XYZ = sum_w L(w) cmf(w) / N`, with `N = sum_w E_v(w) ybar(w)`.

No wavelength step factor (the constant 5 nm cancels in N). Hence `Y = 1` for a perfect
white (zero density) and the viewing illuminant itself has `XYZ_ill = sum_w E_v cmf / N`
(`Y = 1`). Derived chromaticities (from the 5 nm sampled data, not from nominal CIE
values): D50 xy = (0.3456746, 0.3585104), XYZ = (0.9641969, 1, 0.8251226); D55 xy =
(0.3324316, 0.3474444); D65 xy = (0.3127205, 0.3290307); K75P xy = (0.3152469,
0.3328585), XYZ = (0.9470898, 1, 1.0571901); T xy = (0.4507326, 0.4080496); TH-KG3 xy =
(0.4017433, 0.3982018).

### 2.5 Filter spectra
* **Schott KG3 / KG1 / KG5 heat-absorbing glass** and **Canon lens**: each is a two-column
  CSV (wavelength nm, transmittance as a fraction, or percent for the lens), irregularly
  spaced, digitised from published curves. Loader behaviour: sort by wavelength, drop
  duplicate wavelengths (keep first), resample onto the 81-point grid with **Akima
  interpolation without extrapolation** (all data cover 380..780). Source ranges: KG3 272.8-951.9
  nm (146 points), KG1 263.1-1094.3 nm (150), KG5 273.7-883.1 nm (139), lens 350.0-846.8 nm
  (106). KG3 is the only one used by default. Resampled KG3 and lens values are in Appendix A.
* **Filter "weight" operator** for a filter with transmittance `F(w)` and weight `v` in [0,1]:
  `F_v(w) = 1 - (1 - F(w)) * v`. For KG3 the weight is 1.

---

## 3. Printing (enlarger and paper)

### 3.1 Input density and domain
Input is the developed film density `d_f` per pixel (H x W x 3), the output of film
development **after grain** (so it can be below zero by up to the grain `density_min`,
default 0.03 per channel). It is the profile density curves **with the per-channel minimum
subtracted** (min over the log-exposure axis), evaluated by piecewise-linear interpolation,
plus DIR-coupler and grain effects (doc 01/03). The spectral base density (`base(w)`) is
added separately at P1 and is NOT part of `d_f`.

Density domain used for LUT/limits: `d_f,min = -grain.density_min` (so -0.03 by default) and
`d_f,max = max over the tabulated curve of the film's raw (un-normalised) density_curves`,
per channel (e.g. Portra 400: 1.99, 1.80, 2.16; table in section 9.3 for other stocks).

### 3.2 Enlarger light source
The enlarger lamp spectrum is `E_enl = standard_illuminant(enlarger.illuminant)`, default
`TH-KG3` (the GUI offers only this one; others would need matching neutral-filter data).

### 3.3 Dichroic colour-head filter model
The head has three dichroic filters (cyan, magenta, yellow), each inserted a variable
amount. Values are given in **Kodak CC units**: 100 units = density 1.0 of the inserted
filter in its absorption band; the filter vector is ordered (C, M, Y).

**Pure-filter transmittance curves** (the reference uses the analytic "custom" set by
default). Let `erf` be the Gauss error function and lambda in nm. Define, then map with
`F -> F/2 + 1/2`:

* Y: `erf((lambda - 516) / 12)`  (passes long wavelengths, blocks blue)
* M: for `lambda <= 550`: `-erf((lambda - 500) / 8)`; for `lambda > 550`: `erf((lambda - 610) / 8)`  (blocks green; passes blue and red)
* C: `-erf((lambda - 607) / 8)`  (blocks red)

Resulting `F_C, F_M, F_Y` (columns C, M, Y) at sample wavelengths: 380 nm (1, 1, 0); 430
(1, 1, 0); 480 (1, 0.999797, 0.000011); 530 (1, 0, 0.95052); 580 (0.999999, 0, 1); 630
(0.000024, 0.999797, 1); 680 and above (0, 1, 1). (Each pure filter: transmittance ~1 in its
pass-band, ~0 in its stop-band, edges 8-12 nm wide.)

Alternative measured filter sets exist in the data folder (Durst Digital Light, Thorlabs,
Edmund Optics; CSV of wavelength nm, percent transmittance, Akima-resampled; Durst/Edmund
are digitised from plots, Thorlabs is 1 nm tabulated). They are NOT used by any default
code path; the neutral-filtration database (3.6) was fitted with the analytic set, so use
the analytic set.

**Insertion model.** For filter values `cc = (cc_C, cc_M, cc_Y)` compute the per-filter
"open fraction" `t_k = 10^(-cc_k / 100)` (so `cc=0 -> t=1` filter fully out; `cc=100 ->
t=0.1`). The transmittance of filter k is

`Tfilt_k(w) = 1 - (1 - F_k(w)) * (1 - t_k)`    (weight operator with `v = 1 - t_k`)

and the filtered enlarger spectrum is `E_f(w) = E_enl(w) * Tfilt_C(w) * Tfilt_M(w) * Tfilt_Y(w)`.

Sanity values: `cc = (0, 65, 55)` on a flat spectrum gives 0.2818 (= `10^-0.55`, only the
yellow filter absorbs) at 380..430 nm, 0.2818 at 480 nm, 0.2159 at 530 nm (magenta 0.2239
times yellow 0.964), 0.2239 at 580 nm and 0.99984 at 630 nm, 1.0 at 680 nm and above.

**Effective filter values** used by the printing stage (`c, m, y` in CC units):
* main exposure: `c = c_neutral`, `m = m_neutral + m_shift`, `y = y_neutral + y_shift`
* preflash: `c = c_neutral`, `m = m_neutral + preflash_m_shift`, `y = y_neutral + preflash_y_shift`

Defaults: `c_neutral = 0, m_neutral = 65, y_neutral = 55` (overridden per film/paper pair by
the database of 3.6), all shifts 0. The GUI suggests shifts in about [-20, 20], allows
[-200, inf).

### 3.4 Paper raw exposure
Paper has three spectral sensitivities `S_l(w) = 10^(log_sensitivity(w,l))` (NaN -> 0; the
absolute scale is the datasheet's, values span ~1e-10..1e2 and are **not** renormalised per
channel). The raw (linear, relative) exposure of paper layer `l = R,G,B` is

`R_l = sum_w [ E_f(w) * 10^(-D_f(w)) ] * S_l(w)`          (NaN-masked, no wavelength step)

with `D_f` from 2.2 using the film's own `channel_density` and `base_density`. Paper layer
l is spectrally the "R/G/B" sensitivity of the paper; paper layers R, G, B drive paper cyan,
magenta, yellow dye respectively.

### 3.5 Exposure normalisation ("auto print exposure")
A scalar `g` multiplies `R_l` (same value for the three channels) so that an 18.4 % gray
patch photographed on this film prints at a fixed brightness regardless of film speed,
film base and filtration. Reference midgray patch: linear RGB `(0.184, 0.184, 0.184)`.

Reference midgray film density (computed once per film/camera setting, not per pixel):
1. Convert RGB (0.184)^3 to film raw exposure by the film exposure model (spectral
   upsampling + sensitivities), **assuming input colour space sRGB with no transfer-function
   decoding** (hard-coded, independent of `io.input_color_space`), with the filming stage's
   UV/IR filters but NO halation, diffusion, blur, highlight boost, black/white correction or
   grain; `log_raw = log10(raw + 1e-10)`.
2. Map through the film's **raw density curves without min subtraction** (not the
   normalised curves of 3.1) by linear interpolation of density versus `log_exposure / gamma`
   (gamma = `film_render.density_curve_gamma`, default 1; interpolation clamps to end
   values). This gives `d_mid`.
3. `D_mid(w) = base_f(w) + sum_k dye_f(w,k) d_mid,k`.

Let `R_mid,l = sum_w E_f(w) 10^(-D_mid(w)) S_l(w)` (floored at 1e-10) and define
`G(D) = 1 / geomean_l(R_mid,l)`, `geomean = exp(mean_l ln R_mid,l)`.

A second reference, "compensated", is the same computation with the midgray input scaled by
`2^EV` where `EV = camera.exposure_compensation_ev` (the manual exposure compensation only:
the camera auto-exposure gain is NOT included): `D_mid,comp`.

`enlarger.print_exposure_compensation` (default true, "auto compensate camera EV") and
`enlarger.normalize_print_exposure` (default true) select `g`:

| compensation | normalise | `g` |
|---|---|---|
| true | true (default) | `G(D_mid,comp)` (compensated mid-gray lands at geometric-mean raw exposure 1) |
| true | false | `G(D_mid,comp) / G(D_mid)` = `geomean(R_mid) / geomean(R_mid,comp)` |
| false | true | `G(D_mid)` |
| false | false | 1 |

When compensation is false `D_mid,comp` is not computed (treated as factor 1 in the table
formulas). Uses the enlarger filtered spectrum **with the current shifts** for both `E_f`
terms. Because the filters enter `g`, a change of `m/y` shift rebalances colour but
does not change overall exposure much. The reference recomputes `D_mid`/`D_mid,comp` only when
camera EV, film curves or film model change.

With default settings, a mid-gray scene patch gives a paper raw exposure whose geometric mean
over R,G,B is 1.0 (mean log10 = 0), i.e. mid-gray sits at logE ~ 0 on the paper's -3..4 axis.

### 3.6 Preflash
Preflash is a uniform extra exposure that does not pass through the image. Constants:
`preflash_exposure` (default 0 = off; a **linear multiplier**, not EV, despite the GUI
tooltip), `preflash_m_shift`, `preflash_y_shift` (default 0). If `preflash_exposure > 0`:

`R_pf,l = preflash_exposure * sum_w E_pf(w) 10^(-base_f(w)) S_l(w)`

where `E_pf` is the enlarger source filtered with the preflash filter values of 3.3 and the
light passes through the film base only (`base_f`, no dyes, no min-density term). `R_pf` is
**added to the raw exposure after the midgray factor g** and before the print exposure
multiplier:

`R'_l = g * R_l + R_pf,l`, then `R''_l = R'_l * print_exposure * (bw_corr)`. Hence preflash is
scaled by print exposure but not by `g`. (Effect check: preflash 0.01 on Portra 400 /
Portra Endura moves mid-gray from 0.1795 to ~0.1485 linear.)

### 3.7 Print exposure, black/white exposure correction, diffusion, log
Order of the remaining operations on the paper exposure image:

1. `R''_l = (g * R_l + R_pf,l) * print_exposure`; `print_exposure` default 1.0 (linear
   multiplier; 2.0 darkens the print ~ one stop, since more paper exposure = more paper
   density = darker).
2. times the **black/white printing exposure correction** `b` (scalar; 1.0 unless the scanner
   black/white correction flags are on, 3.10).
3. enlarger diffusion filter (default inactive; spatial, doc 03). Operates on linear raw.
   The `enlarger.lens_blur` parameter is **never used** by the reference: no enlarger blur.
4. `log_E_l = log10( max(R_final,l, 0) + 1e-10 )`.

(In the reference, steps 1-4 of 3.4-3.6 are packaged as a function returning `log10(max(.,0)
+ 1e-10)` and then exponentiated again before applying steps 1-3; the 1e-10 floor therefore
appears twice - the offset is negligible but a bit-faithful port adds it.)

### 3.8 Paper development (density curves)
Paper stock curves are evaluated analytically from a fitted model stored in the profile
(`density_curves_model`), not from the tabulated `density_curves` array (which is the
sampled version of the same model and is used only for limits and for black/white
correction). Model (`model_type = "cdfs"`): for channel c (0=R-sensitive layer -> cyan, 1
-> magenta, 2 -> yellow dye) and 3 sub-layers i, with arrays `centers[c][i]`,
`amplitudes[c][i]`, `sigmas[c][i]` (shape 3x3, see data schema 9.2):

`d_p,c(logE) = sum_i A[c][i] * Phi( (logE - mu[c][i]) / sigma[c][i] )`

with `Phi` the standard normal CDF (for profile type `negative`, which every paper has;
for `positive`, `Phi(-z)` is used). The curve is evaluated on the profile's `log_exposure`
axis (256 points, uniform `linspace(-3, 4, 256)`, step 7/255) and per pixel the density is
obtained by **piecewise-linear interpolation** of `d_p,c` against the axis, clamped to the
end values outside [-3, 4]. (An implementation may evaluate the CDF sum in closed form per
pixel; difference to the table interpolation is < 1e-4 density.) No gamma factor and no
grain on paper. Paper curve properties: `d_p(-3) ~ 0` (curves are offset-free; base density
is in `base_density`), `d_p,max = sum_i A[c][i]`, e.g. Portra Endura (2.46, 2.07, 1.82).

**Optional creative curve morph** (`print_render.density_curves_morph`, default **inactive**):
if active, before evaluation each sub-layer's parameters are changed by "coupled gamma
scaling": `sigma' = max(sigma / g, 0.05)`, `mu' = mu / g`, `A' = A`, with per-sub-layer
effective gamma `g = gamma_factor * gamma_factor_{red,green,blue}[c] * band` where the band
factor is `gamma_factor_fast` for the sub-layer with the lowest centre `mu` ("fast" grain
population) and `gamma_factor_slow` for the other two (mid and slow). All gammas default 1;
all must be > 0. `developer_exhaustion` `x in [0,1]` (default 0) blends each sub-layer's
CDF toward a Gumbel-max CDF: `cdf = (1-x) Phi(z) + x G(z)`, `G(z) = exp(-exp(-(z/W + L0)))`
with `W = 0.5 ln(2) sqrt(2 pi)` and `L0 = -ln(ln 2)` (the same z; sign-flipped for positive
profiles), and then adds a common horizontal offset to all three centres of the
channel chosen by 1D root finding (Brent, bracket initially [-0.25, 0.25] doubled up to 12
times, tolerance 1e-10) such that the channel density at `logE = 0` equals its value without
exhaustion. For `positive` profile types the sign of z is flipped. Identity when all
defaults. Constants: sigma floor 0.05.

### 3.9 Neutral filtration (auto-balance)
The enlarger is "balanced" so that an 18.4 % gray scene patch prints neutral. The reference
ships a database of filter pairs and has no runtime solver. The criterion used to build it
(verified by re-fitting): for a given film and paper, with cyan = 0, choose magenta `m` and
yellow `y` (CC units, shifts zero) such that the final scanned print of linear input
`(0.184, 0.184, 0.184)` has equal R, G, B (two equations, two unknowns; the print exposure
normalisation of 3.5 makes brightness independent of filtration). In practice a 2D
Levenberg-Marquardt / Newton solve on the full print chain from the 0.184 patch converges in
a few iterations. Re-fitting with the reference chain reproduced database values within
0.03..0.2 CC units (e.g. Portra 400 + Portra Endura: fitted (51.60, 52.56) vs database
(51.568, 52.534); neutral residual |R-G| ~ 1e-7 in linear). The users' `m_shift`/`y_shift`
are offsets from this neutral point.

Database lookup: schema (JSON): `{ paper_stock: { enlarger_illuminant: { film_stock: [c, m, y] } } }`.
Only `enlarger_illuminant = "TH-KG3"` exists; `c` is 0 in every entry; 8 papers x 20 films;
the four positive films have the placeholder (0, 50, 50). If a pair is missing the defaults
`(0, 65, 55)` are used (with a console warning). The `settings.neutral_print_filters_from_database`
switch (default true) enables the override. All entries are in Appendix B.

### 3.10 Black/white correction (optional, default OFF)
`scanner.black_correction`, `scanner.white_correction` (default false), `scanner.black_level`
0.01 and `scanner.white_level` 0.98 (both given as sRGB-**encoded** values; converted to
linear by the sRGB EOTF: 0.98 -> 0.9551316, 0.01 -> 0.00077402). The correction is a linear
rescale of relative luminance `Y` so that the medium's black reference maps to black_level and
its white reference to white_level. It applies only to media whose reference densities are
known: (a) the print when the paper is a negative-working paper (the normal print path), (b)
a positive film scanned directly. Negative films scanned directly are never corrected.

Reference luminances:
* print: `Y_black`, `Y_white` are the scan luminance of the paper density obtained by
  evaluating the **tabulated** paper curves (gamma 1, interpolation as 3.8) at the log-exposure of
  (i) the film at minimum density (`cmy = -grain.density_min`, i.e. "black" print) and (ii) the
  film at maximum density (`max` of raw film curves, "white" print); both pass through
  P1-P4 (including g and preflash but **not** print exposure or correction), then development
  and the spectral->XYZ conversion of 4.1.
* positive film direct scan: `Y_black` from density `max(raw curve)` per channel, `Y_white`
  from zero density.

Linear map: `m = (L_w - L_b) / (Y_white - Y_black + 1e-10)`, `q = L_b - m Y_black`;
`Y' = clip(m Y + q, 0, 1)`. If only black is enabled `L_w = Y_white`; if only white is
enabled `L_b = Y_black`; otherwise `L_b, L_w` = linearised levels. The scanner applies it as a scale
`XYZ' = XYZ * Y' / (Y + 1e-10)` (hue-preserving) after XYZ computation (4.2).

Exposure side (so tonal placement of mid-gray is held): with `Y_mid' = (0.184 - q) / m` and
`d_mid = -log10(0.184)`, `d_mid' = -log10 Y_mid'`; using the **channel-mean** of the paper
tabulated curve `c_av(logE)` and `d_base = mean(base_density)` (nanmean) find
`logE_a = interp(d_mid - d_base ; c_av, logE_axis)` and `logE_b = interp(d_mid' - d_base ;
...)` (interpolating the monotonic mean curve) and multiply the paper raw exposure by
`b = 10^(logE_b - logE_a)`. For a directly scanned positive film the same is done on the
film side with decreasing curves (negated axes) and the factor is `1/10^(...)` applied to the
film raw exposure after halation (doc 03/01). Latent defect: for a *positive* paper the
function returns no value (None) when the flags are on - irrelevant since all shipped papers are
negative-working.

### 3.11 Positive (slide) films and "no print"
* **Slide film (`info.type = positive`) or any film scanned without print**: set
  `io.scan_film = true`. The print stages are skipped; the developed film density is
  scanned directly (section 4) with the **film's** `channel_density`, `base_density` and
  `viewing_illuminant` (D50). Positive film densities decrease with exposure and are handled
  entirely by the film development step (decreasing curves); the printing code has no
  positive branch. Feeding a positive film through the print path is not rejected but gives
  a tonally inverted image (paper is negative-working).
* **Scan negative** (`scan_film = true` with a negative stock): the scan returns the
  orange-masked negative as a transmission image (no inversion, no white balance, no
  base removal; section 4). Values are relative luminance with `Y = 1` for clear light
  (density 0). Sample (linear sRGB, Portra 400, mid-gray input 0.184): (0.2064, 0.0458, 0.0176).
* No additional special casing: preflash, enlarger and paper data are unused.

---

## 4. Scanning

### 4.1 Density to relative XYZ
Per pixel, with the scanned medium's profile (print paper for the print path; film for
`scan_film`):

`D(w) = base(w) + sum_k dye(w,k) d_k` ; `L(w) = E_v(w) 10^(-D(w))` ; `XYZ = (sum_w L(w) cmf(w)) / sum_w E_v(w) ybar(w)`

where `E_v = standard_illuminant(profile.info.viewing_illuminant)` (D50 for all papers except
cine prints with K75P, and D50 for films) and `d` is the print density from 3.8 (or the
developed film density). NaN wavelengths contribute 0 (2.1). The reference then returns
`log10(max(XYZ,0) + 1e-10)` and exponentiates again: net `XYZ + 1e-10` per component (matters
at ~1e-10 only).

Chromaticity of the viewing illuminant, used below: `XYZ_ill = sum_w E_v cmf / sum_w E_v ybar`,
`xy_ill = (X, Y)/(X+Y+Z)`.

Density domain for the LUT (section 6): print: per-channel min/max of the tabulated
print curves (about -0.0008..-0.001 to Dmax, e.g. Portra Endura 2.46, 2.07, 1.82); film
scan: `[-grain.density_min, max raw film curve]`.

### 4.2 Black/white XYZ correction
If enabled (3.10): `Y' = clip(m Y + q, 0, 1)`, `XYZ' = XYZ Y'/(Y + 1e-10)`. Default off.

### 4.3 Viewing glare (print only)
Print scans add veiling light: `XYZ' = XYZ + a(x,y) * XYZ_ill` where `XYZ_ill` has Y = 1
and `a(x,y)` is a spatially varying random fraction, **mean = percent / 100** with
`percent = 0.03` (so mean 3e-4 of white), standard deviation = `roughness * percent / 100`
(`roughness = 0.7`), log-normal distributed per pixel, then Gaussian-blurred with sigma
`blur = 0.5` px before dividing by 100. Parameters `print_render.glare` = (active true,
percent 0.03, roughness 0.7, blur 0.5). Active only when `active and percent > 0`; forced
off by the "deactivate stochastic effects" debug switch. Film glare parameters exist but are
never used (no glare in `scan_film`). A deterministic port should add the mean value
`0.0003 * XYZ_ill` (the blur averages the random field toward its mean). Glare is applied
after the black/white correction and before RGB conversion.

### 4.4 XYZ to linear output RGB
Output colour space default: sRGB (user choice: sRGB, Display P3, Adobe RGB (1998), ITU-R
BT.2020, ProPhoto RGB, ACES2065-1, etc. any space of the colour library). Conversion is the
library's `XYZ -> RGB` with source white = `xy_ill` and **CAT02 chromatic adaptation** (library
default, von Kries in the CAT02 cone space):

`RGB_lin = M_xyz2rgb * M_CAT02(xy_ill -> output white) * XYZ`

Details: `M_CAT02 = [[0.7328, 0.4296, -0.1624], [-0.7036, 1.6975, 0.0061], [0.0030, 0.0136,
0.9834]]`; adaptation matrix `M_CAT02^-1 diag(rho_dst / rho_src) M_CAT02` with `rho = M_CAT02 *
XYZ_white` (white XYZ from xy with Y = 1; the source white is `xy_ill`, the destination the
output colour space's declared white point, e.g. (0.3127, 0.3290) for sRGB/P3/Adobe/BT.2020,
(0.3457, 0.3585) for ProPhoto). `M_xyz2rgb` is the library's **rounded** matrix for the space
- for sRGB:
`[[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]]`
(the library's sRGB matrices are rounded to 4 decimals, so `M_xyz2rgb M_rgb2xyz` differs from
I by up to 4e-5; the rgb2xyz is `[[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722],
[0.0193, 0.1192, 0.9505]]`). Display P3 xyz2rgb: `[[2.49349691, -0.93138362, -0.40271078],
[-0.82948897, 1.76266406, 0.02362469], [0.03584583, -0.07617239, 0.95688452]]`. Resulting
complete matrices from viewing-white XYZ to sRGB (CAT02 + sRGB matrix) for the viewing
illuminants:

| viewing | matrix (row-major) | white maps to |
|---|---|---|
| D50 | [[3.1425605, -1.6326138, -0.4818556], [-0.9697409, 1.9022205, 0.0398726], [0.0592389, -0.2007043, 1.3860496]] | (0.99984, 1.00010, 1.00007) |
| K75P | [[3.2322365, -1.5349109, -0.4979842], [-0.9667543, 1.8719375, 0.0413973], [0.0557082, -0.2030972, 1.0881780]] | same |
| D65 | [[3.2405316, -1.5371841, -0.4985945], [-0.9688830, 1.8757700, 0.0414991], [0.0557001, -0.2039929, 1.0572444]] | same |

There is **no scanner white balance or white-point reference beyond this**: the scan is
"illuminant-white -> output white" (a full von Kries). Paper white (density = base, about
0.09) therefore reads about 0.8-0.9 in linear RGB, not 1.0, unless black/white correction
is on. Negative film scans keep the orange mask as is.

### 4.5 Output gamut compression (default ON: CAM16-UCS)
Settings: `io.output_gamut_compress` = (`algorithm = "cam16ucs"`, `knee = (threshold 0.0, limit
1.0, power 6.0)`, `lightness_compression = (0.7, 1.0, 2.2)`). Other algorithms: `"off"`,
`"oklch"`, `"oklrab"`, `"jzazbz"`, `"aces_rgc"` (4.5.4). With the default, output values lie in
[0, 1] without a downstream clip (to within the 1e-6 table slack and the 4e-5 matrix
rounding; the reference never clamps in the runtime). The input side has a corresponding
"input gamut compression" baked into the film spectral-upsampling LUT (doc 01).

#### 4.5.1 Reinhard knee (shared by all)
For a normalised value `x0` with parameters `(t, l, p)`: if `x0 <= t` output = `x0` (identity,
including negative values); otherwise with `s = l - t`, `u = (x0 - t)/s`:

`knee(x0) = t + s * u / (1 + u^p)^(1/p)`.

This is the ACES Reference Gamut Compression v1.3 shaper. The chroma knee uses
`(0, 1, 6)` (so `knee(d) = d / (1 + d^6)^(1/6)`, asymptote 1); the lightness knee uses
`(0.7, 1.0, 2.2)`.

#### 4.5.2 CAM16-UCS model (colour-library conventions)
All steps use CIE XYZ in the 0..1 domain (relative, white Y = 1) and the library multiplies
by 100 internally (equivalent to the standard CAM16 with `Y_w = 100`). Viewing conditions:
adapting white = the **output colour space's declared white point** (sRGB: xy (0.3127, 0.3290)
-> `XYZ_w = (0.950456, 1, 1.089058)` x 100), `L_A = 64 cd/m^2`, `Y_b = 20`, Average surround
(`F = 1, c = 0.69, N_c = 1`), no illuminant discounting. Derived: `k = 1/(5 L_A+1)`,
`F_L = 0.2 k^4 (5 L_A) + 0.1 (1 - k^4)^2 (5 L_A)^(1/3) = 0.683990`; `n = Y_b/Y_w = 0.2`;
`z = 1.48 + sqrt(n) = 1.927214`; `N_bb = N_cb = 0.725 n^-0.2 = 1.000304`; `D = F (1 -
(1/3.6) exp(-(L_A + 42)/92)) = 0.912236`.

Forward (XYZ -> J', a', b'), with `XYZ` scaled to 0..100:
1. `RGB = M16 XYZ`, `M16 = [[0.401288, 0.650173, -0.051461], [-0.250268, 1.204414, 0.045854], [-0.002079, 0.048952, 0.953127]]`.
2. Chromatic adaptation to equal-energy: `RGB_c = D_RGB * RGB`, `D_RGB = D Y_w / RGB_w + 1 - D` (`RGB_w = M16 XYZ_w`, per channel).
3. Compression `RGB_a = 400 sign(RGB_c) x/(27.13 + x) + 0.1`, `x = (F_L |RGB_c| / 100)^0.42` (same on white).
4. `a = Ra - 12 Ga/11 + Ba/11`, `b = (Ra + Ga - 2 Ba)/9`, `h = atan2(b, a)`.
5. `A = (2 Ra + Ga + Ba/20 - 0.305) N_bb`; `J = 100 (A/A_w)^(c z)`.
6. `e_t = (cos(h + 2) + 3.8)/4`; `t = (50000/13 * N_c N_cb) e_t sqrt(a^2+b^2) / (Ra + Ga + 21 Ba/20)`; `C = t^0.9 sqrt(J/100) (1.64 - 0.29^n)^0.73`; `M = C F_L^0.25`.
7. UCS: `J' = (1 + 100 c1) J / (1 + c1 J)`, `M' = ln(1 + c2 M)/c2`, `c1 = 0.007`, `c2 = 0.0228`, `a' = M' cos h`, `b' = M' sin h`.
   White (Y=1 at the declared white) gives `J' = 100`.
Inverse is the exact analytic inverse of steps 7..1 (`J = J'/(1 + 100 c1 - c1 J')`, `M = (exp(c2 M') - 1)/c2`, `C = M/F_L^0.25`, `t = (C / (sqrt(J/100)(1.64-0.29^n)^0.73))^(1/0.9)`, recover `a, b` from `t` and `h`, invert the compression `x = 27.13 |RGB_a - 0.1| / (400 - |RGB_a - 0.1|)`, `|RGB_c| = (100/F_L) x^(1/0.42)`, undo D, `XYZ = M16^-1 RGB`).

Check values (output = sRGB white point): grey Y = 0.18 -> J' = 52.28 (a',b' ~ -0.84, -0.48 due to
the rounded sRGB matrix), Y = 0.5 -> 78.79, Y = 0.7 -> 88.84, Y = 1 -> 100.0003.

#### 4.5.3 Per-pixel algorithm (default)
1. `XYZ_o = M_rgb2xyz * RGB` with the output space's matrix and NO adaptation (illuminant =
   output white).
2. `(J', a', b')` by 4.5.2 with the output space's white.
3. **Lightness compression (one-sided):** `J'_norm = J' / J'_white` (`J'_white = 100`
   for any space whose white defines `XYZ_w`, i.e. exactly 100); apply the knee (4.5.1) with
   `(0.7, 1.0, 2.2)`; `J' <- 100 * knee(J'_norm)`. Identity below 70 (thus J' < 70 untouched, black
   at 0 anchored, negative J' untouched). Notable consequence: a neutral of linear 1.0 maps to
   J'_norm = 0.919 -> linear 0.7718; Y = 0.5 maps to ~0.4956; so highlights
   are compressed even in perfectly in-gamut neutrals. This is part of the "look".
4. `C' = hypot(a', b')`, `h = atan2(b', a')`.
5. `C'_max = Cmax(J', h)` from the per-output-space table (4.5.5), bilinear lookup with `J'`
   clamped to [1, 110], `h` periodic; `d = C' / max(C'_max, 1e-9)`; `d_c = knee(d)` with
   `(0, 1, 6)`; `C'_new = d_c * max(C'_max, 1e-9)`.
6. `(a'_new, b'_new) = C'_new (cos h, sin h)`; inverse CAM16-UCS (J' from step 3) -> XYZ; `RGB = M_xyz2rgb XYZ` (no adaptation).
Chroma is modified at constant J' and h; chroma of colours inside the gamut is still
reduced slightly by the always-on knee (`d/(1+d^6)^(1/6)`: 0.3 % reduction at d = 0.5, 5 % at d = 0.85)
- this is intended.

#### 4.5.4 Other algorithms (not default)
* `aces_rgc`: `ach = max(R,G,B)`; for each channel `d = (ach - c)/ach`; `d' = knee(d; (t,l,p))`;
  `c' = ach (1 - d')`; pixels with `ach <= 1e-12` unchanged. No lightness compression.
* `oklch`: RGB -> XYZ (no adaptation) -> OkLab (Ottosson, as in the colour library); lightness
  knee on `L` with `L_white = 1`; `C = hypot(a,b)`, `h`; table `Cmax(L, h)` built with
  `L` grid `linspace(0.02, 1, 64)`, chroma search upper bound 0.5; same chroma knee; back.
* `oklrab`: as oklch but the table and lightness coordinate are Ottosson's `Lr`:
  `Lr = 0.5 (t + sqrt(t^2 + 4 k2 k3 L))`, `t = k3 L - k1`, with `k1 = 0.206`, `k2 = 0.03`,
  `k3 = (1 + k1)/(1 + k2)`; inverse `L = Lr (Lr + k1) / (k3 (Lr + k2))`.
* `jzazbz`: JzAzBz with `Y_w = 100 cd/m^2` (RGB 1.0 = 100 cd/m^2), table grid `linspace(0.002,
  0.18, 64)`, chroma upper bound 0.3; lightness knee normalised by the Jz of the white.

#### 4.5.5 Cmax table (per output colour space)
For each (J', h) on a grid find the largest chroma such that the reconstructed linear RGB (4.5.3 step 6 inverse path, `RGB = M_xyz2rgb XYZ`
with no adaptation) lies in the cube `[-1e-6, 1 + 1e-6]^3`. Grid: `J'` = `linspace(1, 110,
64)`; `h` = 720 samples `-pi + 2 pi i/720` (`endpoint = false`); bisection per cell between
`lo = 0`, `hi = 150`, 18 iterations, recording the final `lo` (the largest known in-gamut chroma)
(so resolution 150/2^18). Lookup is bilinear: `h` index `(h + pi)/(2 pi/720)` with wrap-around,
`J'` index `(J'_clamped - 1)/109 * 63` using the cell `floor` clipped to [0, 62]. The table
depends on output space and its white; it can be pre-baked offline per space (64 x 720 floats
= 184 kB, in the order J' outer, h inner) or built at start-up (~0.8 M CAM16 inversions).
Example `Cmax` values for sRGB (rows J' index 0,16,32,48; columns h index 0,90,...,630):
row 0 (J'=1): 3.470, 3.544, 6.214, 7.774, 7.211, 4.903, 3.470, 5.165; row 16 (J'=34.5):
20.672, 20.734, 31.670, 37.491, 36.967, 28.347, 21.397, 29.272; row 32 (J'=68.9): 28.349,
28.420, 39.128, 45.490, 48.165, 38.031, 29.386, 39.107; row 48 (J'=103.3): 34.550, 28.019,
22.302, 26.337, 23.174, 21.078, 35.846, 46.779.

Reference output samples (default spec, output sRGB, linear in -> linear out):
(0.18,0.18,0.18) -> (0.180003, 0.180010, 0.180003); (0.5,0.5,0.5) -> (0.495599, 0.495647,
0.495634); (0.7,0.7,0.7) -> (0.645581, 0.645950, 0.645991); (1,1,1) -> (0.771826, 0.773194,
0.773420); (2,2,2) -> (0.897150, 0.902328, 0.903273); (0.01,0.01,0.01) -> (0.01, 0.010001,
0.01); (0.9,0.3,0.1) -> (0.866123, 0.309449, 0.116713); (1.2,0.1,0) -> (0.963025,
0.176279, 0.060912); (-0.1,0.5,0.2) -> (0.025812, 0.445879, 0.232112); (0.2,-0.02,0.9) ->
(0.178652, 0.025748, 0.588971); (0.05,0.05,0.4) -> (0.052001, 0.052555, 0.372937); (0,0.9,0.9)
-> (0.105809, 0.806271, 0.803769).

### 4.6 Scanner blur and unsharp mask
Applied on the linear RGB after gamut compression and before the transfer function:
Gaussian blur (`scanner.lens_blur` sigma in px, default 0) and unsharp mask
`(sigma, amount) = scanner.unsharp_mask`, default (0.7, 0.7), applied when both > 0 (doc 03
gives the kernels). Disabled by the preview/lut_mode/deactivate-spatial switches. These steps
are not clamped, so unsharp can push values slightly outside [0, 1].

### 4.7 Output transfer function
`io.output_cctf_encoding` (default true) applies the output space's standard encoding function
via the library's colourspace->same-colourspace conversion with decoding off and encoding on.
This round-trips through XYZ with the rounded matrices (sRGB 1.0 becomes 1.0000065 before the
curve; harmless). Curves: sRGB / Display P3: `v <= 0.0031308 ? 12.92 v : 1.055 v^(1/2.4) - 0.055`
applied also to negative values on the linear branch (e.g. -0.01 -> -0.1292); Adobe RGB: pure
power `v^(1/2.19921875)`; ITU-R BT.2020: BT.2020 OETF (`alpha = 1.09929682680944`,
`beta = 0.018053968510807`); ProPhoto: ROMM RGB (linear below 1/512 with slope 16, else power
1/1.8). The reference runtime API returns whatever this gives; **no clamp to [0,1] is performed
anywhere in the runtime**, saving to 8/16-bit formats clips. (Test harness converts to 16-bit
PNG with clip.) With `output_cctf_encoding = false` the result is linear.

---

## 5. Camera-side exposure interaction (summary, for completeness)

The auto print exposure of 3.5 keys on `camera.exposure_compensation_ev` only. The camera
`auto_exposure` (default true, `center_weighted`) is a separate scene-referred gain applied
to the input before the film: gain `2^(-log2(Y_meter / 0.184))` where `Y_meter` is the
metered luminance (Y of the RGB image in its own colour space) from a nearest-neighbour
reduced copy (long edge <= 256 px). `center_weighted`: weights `exp(-(x^2+y^2)/(2*0.2^2))`
normalised to sum 1 on coordinates normalised so the long edge spans [-0.5, 0.5]
(`x = (i/W - 0.5) * W/max(W,H)`, same for y); others: `average`, `median`, `partial`
(mean in radius < 0.15), `matrix` (5x5 raised-cosine), `multi_zone`
(rings 0-0.05/0.05-0.25/0.25-0.5 with weights 0.5/0.3/0.2), `highlight_weighted`
(weights Y^2). If the meter is zero the gain is 1 (0 EV). Because this gain is NOT in the
print compensation, the film sees the auto-exposed image but the print normalisation assumes a
mid-gray at EV compensation only: auto-exposure therefore changes the print brightness
(mid-gray 0.184 meter -> unity gain; other scenes are shifted toward mid-gray).

---

## 6. Spectral LUT acceleration

All of 4.x's spectral math and 3.x's spectral math are replaced, as an option, by 3D LUTs
over a reduced domain. The reference ships this as an option (`settings.use_enlarger_lut`,
`settings.use_scanner_lut`, both default **false**; `settings.lut_resolution` default 17).
It is a pure accelerator: three functions are tabulated.

### 6.1 What is tabulated
**Enlarger LUT.** Input: film density triplet `d_f` (3D, normalised below). Output: the
function of 3.4-3.6, i.e. `log10(max(g R + R_pf, 0) + 1e-10)` for each of the three paper
layers (so it already contains `g`, the enlarger filtration, preflash). Domain per channel:
`[d_f,min, d_f,max]` = `[-grain.density_min, max raw film curve]`. After the lookup the
engine does `10^out`, multiplies by `print_exposure` and `b`, applies diffusion, then log10 again. Therefore
`print_exposure`, black/white correction and diffusion do **not** invalidate the LUT; film
curves, film/paper stock, filters (including shifts), preflash, EV compensation and the
enlarger illuminant do.

**Scanner LUT.** Input: the print (or film) density triplet. Output: `log10(max(XYZ,0) +
1e-10)` of 4.1 (three channels X, Y, Z, log domain). Domain per channel: print: `[min over the
tabulated curve, max over the tabulated curve]` (about [-0.001, 2.0..2.5]); film scan:
`[-grain.density_min, max raw film curve]`. Glare, black/white correction, XYZ->RGB, gamut
compression, blur, unsharp and OETF are applied after the lookup (per pixel).

**Filming (input) spectral-upsampling LUT** (doc 01 describes the content): a 2D LUT
of dimension 192 x 192 x 81 (float16) of spectra on triangular-to-square chromaticity
coordinates `tc` in [0,1]^2, contracted with the film sensitivity (optionally window and
surface adaptation, input gamut compression baked in by remap-resampling) into a 192 x 192 x 3
raw-exposure LUT. Per pixel: chromaticity -> `tc` -> **2D Mitchell-Netravali cubic (B = C =
1/3)** lookup (6.3) -> multiply by `b = X+Y+Z` (the "brightness" factor). Rebuilt whenever
film sensitivity, adaptation params, reference illuminant or input gamut-compression spec
change.

### 6.2 LUT geometry, encoding and sizes
* 3D cube with `N` samples per axis, `N = lut_resolution` (default 17; N^3 x 3 values).
  The reference's creator tool uses `N = 33` for exported .cube bundles; 9, 17, 25, 33 were
  measured (6.5).
* Sample positions: per channel, `N` equally spaced values from `xmin_c` to `xmax_c`
  inclusive (`linspace`); table indexed `lut[i][j][k]` = f(sample_C_i, sample_M_j, sample_Y_k)
  with channel order C, M, Y (axis 0 = first channel). Values stored in double precision (a port
  may use float32; the outputs are log10 quantities of magnitude ~[-10, 1]).
* Encoding of the LUT **input**: affine normalisation per channel to the unit interval:
  `u_c = (d_c - xmin_c)/(xmax_c - xmin_c)`, then grid coordinate `p_c = u_c (N - 1)`; values
  outside the domain **clamp** to the cube boundary (density saturates; no extrapolation).
  Requires `xmax > xmin`.
* Encoding of the LUT **output**: log10 of the physical quantity. The log domain is
  important: the interpolant is applied to smooth log curves, and log10 keeps accuracy in
  dark values (the floor 1e-10 bounds the log).
* The reference creator for external LUTs (cmy_film -> output RGB "wire" LUTs) uses
  the same normalisation: `code = clip((D - dmin)/(dmax - dmin), 0, 1)` per channel, with
  `dmin = -headroom` below fog for grain noise and `dmax` measured by sampling the film half
  of the pipeline on a 9^3 grid and rounding outward (with a margin); log-E wires use `code
  = (logE - min)/(max - min)`.
* Invalidation: the reference compares the result of the spectral function on a fixed 2x2
  test image of densities {0.1,0.2,0.3; 0.4,0.5,0.6; 0.7,0.8,0.9; 1.0,1.1,1.2} with the
  value stored when the LUT was baked and rebakes if different (a cheap hash for "did any
  dependent parameter change"). A port should instead rebake on explicit parameter changes.

### 6.3 Interpolation
**Default 3D interpolation: tricubic Hermite with PCHIP-limited slopes ("pchip").**
Precompute, once per LUT, for each of the three axes and each of the three output channels
the monotone slope at every node along every grid line (line values `v_0..v_{N-1}`, deltas
`delta_i = v_{i+1} - v_i`, uniform spacing 1):
* N = 1: slope 0; N = 2: both slopes `delta_0`.
* Interior node i: if `delta_{i-1} * delta_i <= 0` (or either zero) slope 0; else harmonic
  mean `2 delta_{i-1} delta_i / (delta_{i-1} + delta_i)`.
* End nodes (three-point rule): left slope `s = (3 delta_0 - delta_1)/2`; if `s * delta_0 <= 0`
  then 0; else if `delta_0 * delta_1 < 0` and `|s| > |3 delta_0|` then `3 delta_0`. Right end mirrored
  using `delta_{N-2}`, `delta_{N-3}`.
Also precompute, per cell and channel, the min and max of the 8 corner values.

Evaluation at grid coordinates `(p_C, p_M, p_Y)` (clamped to [0, N-1]; a coordinate exactly
`N-1` uses cell `N-2` with fraction 1): cell indices `i,j,k` and fractions `tr, tg, tb`. For each
output channel:
1. Four Hermite interpolations along axis 0 at the cell's 4 corners `(j,k), (j+1,k),
   (j,k+1), (j+1,k+1)` using the node values and the **axis-0 slopes** at `i` and `i+1`, at `tr`.
2. For the same four lines blend the **axis-1 slopes** linearly across `i -> i+1` with `tr`.
3. Hermite along axis 1 (between the `j` and `j+1` results for each of `k` and `k+1`), with the
   blended slopes, at `tg` -> two values `vz0, vz1`.
4. Blend the **axis-2 slopes** bilinearly over the four lines `(i,j), (i+1,j), (i,j+1),
   (i+1,j+1)` with `(tr, tg)` for `k` and for `k+1` -> `sz0, sz1`.
5. Hermite along axis 2 between `vz0, vz1` with `sz0, sz1` at `tb`.
6. Clamp the result to [min, max] of the 8 corner values of the cell (guarantees no
   overshoot).
Hermite basis: `h00 = 2t^3 - 3t^2 + 1`, `h10 = t^3 - 2t^2 + t`, `h01 = -2t^3 + 3t^2`, `h11 = t^3
- t^2`; value `= h00 y0 + h10 m0 + h01 y1 + h11 m1` (slopes per cell, i.e. per unit grid step).
It is not a true tricubic but is smooth enough that N = 17 is accurate (6.5). Cost per
sample: 8 node fetches of (value + 3 slopes) per channel; mobile implementations may
precompute per-node slope textures. The reference recomputes the slopes at every call (an
inefficiency that should be cached).

**Optional alternative ("mitchell")**: separable 4x4x4 Mitchell-Netravali kernel (B = C = 1/3),
`w(x) = (1/6)[(12-9B-6C)|x|^3 + (-18+12B+6C)|x|^2 + (6-2B)]` for `|x| < 1`, `(1/6)[(-B-6C)|x|^3
+ (6B+30C)|x|^2 + (-12B-48C)|x| + (8B+24C)]` for `1 <= |x| < 2`, else 0; neighbours
`base-1..base+2` with **symmetric reflection** at the borders (index `-i -> i`, `i >= N ->
2(N-1)-i`), weights divided by their sum. It is not interpolating (it smooths). The 2D version
(used for the filming LUT and its Hald-style cousins) is the same with 4x4 neighbours and
cell/fraction logic clamped to [0, N-1]; N = 1 falls back to bilinear.
A trilinear (or tetrahedral) lookup would also work at a higher N (see 7).

### 6.4 When LUTs replace spectral math
`use_enlarger_lut` and `use_scanner_lut` are independent per-stage switches (defaults false:
the reference runs full spectral math per pixel, which is its slow path). The enlarger LUT
is used with `spectral_compute_enlarger(...)`, the scanner LUT with `spectral_compute_scanner(...)`;
if the switch is off the spectral function is applied directly. The filming LUT is always used
(the "hanatos2025" method). The `debug.lut_mode` switch forces a deterministic per-pixel
chain (spatial and stochastic effects off, no auto-exposure, no print exposure compensation,
`print_exposure = 1`, highlight boost off, scanner corrections and unsharp off) so that the
whole pipeline can be sampled into an RGB -> RGB LUT (the reference's separate creator tool
builds 1-, 2-, 3-, 4-LUT bundles, default N = 33, this way).

### 6.5 Measured accuracy of the LUT path (reference vs reference-without-LUT)
Portra 400 + Portra Endura, sRGB output linear, default params, 2040 random colours with
intensities 0..1.2 plus a 40-step grey ramp 0.002..2 (input ProPhoto linear): delta E 2000 of
LUT vs exact spectral (clipped to [0,1]):

| N | enlarger LUT only: max / mean | scanner LUT only: max / mean | both: max / mean |
|---|---|---|---|
| 9 | 0.041 / 0.0049 | 0.162 / 0.0163 | 0.164 / 0.0178 |
| 17 (default) | 0.0054 / 0.00058 | 0.0236 / 0.0021 | 0.0242 / 0.0023 |
| 25 | 0.0016 / 0.00017 | 0.0084 / 0.00062 | 0.0084 / 0.00067 |
| 33 | 0.00053 / 0.00007 | 0.0029 / 0.00023 | 0.0029 / 0.00026 |

(Max |dRGB| at N=17 both LUTs: 3.2e-4.) So N = 17 (4913 nodes) is already visually exact (dE <
0.025).

---

## 7. Options for a mobile-efficient implementation (not reference behaviour)

* **Whole-chain 3D LUT on film density.** After grain, steps P1-P5, P7, P8 and S1-S5 are
  a deterministic function of the three film densities and a few scalars (print exposure,
  filtration, preflash; glare mean). It can be baked into a single N^3 x 3 LUT mapping film density -> final
  linear (or encoded) output RGB, including the (heavy) CAM16-UCS gamut compression and the
  lightness roll-off, so the per-pixel cost is one 3D lookup. Rebake (17^3-33^3 evaluations of the
  full spectral chain, plus the table of 4.5.5 which is image independent) on slider changes
  that alter filtration, exposure compensation, print exposure (if baked in), stock, output
  space. Print exposure alone can instead be applied as an additive log offset on the
  intermediate log-exposure if the LUT is split (film density -> log E_print, then per-channel
  1D paper curves, then a 3D LUT print-density -> output RGB): "2-LUT" and "4-LUT" topologies of the creator tool.
* Domain choices that keep nodes useful: film density axis `[-density_min, dmax_film]`; paper
  density `[~0, dmax_paper]` per channel.
* Use log-domain outputs (log E for enlarger, log XYZ for scanner) or, for an end-to-end
  RGB LUT, a perceptual shaper at the output; keep N >= 17 with the PCHIP-limited
  tricubic; N = 33 for end-to-end RGB LUTs including the gamut compression.
* The spectral sums are 81-wavelength dot products with three weight vectors; the dye and
  base spectra and `10^-D` are the expensive part, which the LUT eliminates. The wavelength
  validity mask (2.1) must be baked into the model before sampling.
* A float32/half LUT texture with hardware trilinear at N = 33..65 over the same normalised density domain is an
  alternative to the Hermite scheme; the reference's accuracy numbers above do not apply.

---

## 8. Validation vectors (reference behaviour)

Conditions: film Kodak Portra 400, paper Kodak Portra Endura, default parameters, camera auto
exposure off, spatial and stochastic effects off (no grain, halation, glare etc.), input
colour space ProPhoto RGB linear (D50), output sRGB, output transfer function off (linear),
neutral filters from the database (0, 51.568015, 52.534004), default gamut compression
(cam16ucs) unless marked.

Film density `d_f` (cmy_film tap) and output (linear sRGB, **gamut compression off**):

| input (linear ProPhoto) | d_f (C, M, Y) | output, compression off |
|---|---|---|
| (0.184,0.184,0.184) | 0.692898, 0.704642, 0.832530 | 0.179458, 0.179308, 0.179180 |
| (0.02,0.02,0.02) | 0.180767, 0.222147, 0.275702 | 0.006091, 0.007181, 0.007484 |
| (0.7,0.7,0.7) | 1.004322, 0.994358, 1.173435 | 0.587708, 0.599399, 0.604585 |
| (0.5,0.1,0.1) | 1.170164, 0.237869, 0.724925 | 0.478172, 0.010270, 0.066269 |
| (0.1,0.5,0.1) | 0.448700, 1.199713, 0.400420 | -0.013314, 0.294882, 0.008606 |
| (0.1,0.1,0.5) | 0.191806, 0.723574, 1.423281 | -0.077458, 0.152138, 0.531649 |
| (2,1,0.3) | 1.451635, 0.950303, 0.726411 | 0.861840, 0.475074, 0.124249 |

Independent re-implementation from sections 2-4 gives (0.179469, 0.179308, 0.179172) for the first
row, i.e. |delta| <= 1.1e-5 (cause: Akima/library rounding of tabulated spectra; acceptable).

Default (cam16ucs) output for further inputs (same conditions, linear sRGB): 0.184 gray ->
(0.179461, 0.179318, 0.179183); 0.05 -> (0.019989, 0.022372, 0.021151); 0.5 -> (0.472391,
0.490104, 0.495991) (uncompressed 0.475781, 0.493541, 0.499470); 1.0 -> (0.633270, 0.640657,
0.638981) (uncompressed 0.682688, 0.690116, 0.688313); 4.0 -> (0.703765, 0.703274, 0.690887) (uncompressed 0.809671, 0.808382, 0.794606); black 0 ->
(0.003905, 0.004238, 0.005496) (deep shadow: the paper's maximum density, i.e. the print's lowest reachable level); (0.5,0.1,0.1) -> (0.435209, 0.024768,
0.073074); (0.1,0.5,0.1) -> (0.025904, 0.275824, 0.031837); (0.1,0.1,0.5) -> (0.007380,
0.144178, 0.326679).

Parameter probes (0.184 grey, same conditions, linear, compression on): `normalize_print_exposure =
false`: (0.113741, 0.113180, 0.111372); camera EV +1 with print compensation: (0.17434,
0.181219, 0.182528) (not exactly neutral: the film is non-linear); EV +1 without
compensation: (0.371197, 0.384069, 0.390616); `print_exposure = 2`: (0.024447, 0.025241,
0.023745); `m_filter_shift = +10`: (0.152877, 0.214004, 0.155556); preflash 0.01:
(0.150795, 0.147593, 0.145875); black+white correction on (levels 0.01/0.98): (0.178288,
0.17778, 0.176812). Input colour space sRGB / BT.2020 instead of ProPhoto for the grey: (0.179526,
0.179295, 0.179248) / (0.179489, 0.17931, 0.179268).

Direct scans (default settings incl. gamut compression; input list 0.184, 0.02, 0.05, 0.5, 1,
4 greys): Portra 400 negative (`scan_film`) 0.184 -> (0.206400, 0.045842, 0.017570), 0.02 ->
(0.509435, 0.138668, 0.071924), 1.0 -> (0.102828, 0.020664, 0.005917); Ektachrome 100
(`scan_film`, positive) 0.184 -> (0.189245, 0.184012, 0.173820), 0.02 -> (0.003866, 0.003169,
0.002862), 0.5 -> (0.485076, 0.476091, 0.457662), 1.0 -> (0.639959, 0.625294, 0.618821).

Mallett-2019 upsampling (alternative film model) would give (0.182599, 0.180677, 0.173980):
the film model matters; the print/scan stages are model-independent.

---

## 9. Data files, schemas and licensing

### 9.1 Overview (files used by printing/scanning/LUT)
| file or table | used for | provenance | licence |
|---|---|---|---|
| `data/profiles/*.json` (28 stocks: 20 films, 8 papers) | film/paper spectral data, density curves, models, illuminants | built by the author from Kodak/Fujifilm datasheets + generic dye densities (datasheet figures digitised; dye spectra fitted; "original data are property of the holders" per the profile's own datasource note) | CC BY-SA 4.0, (c) Andrea Volpato, attribution + share-alike on the file and "all direct derivatives ... LUTs or any other format that encodes the same content" (data/license/SPEKTRAFILM_LICENSE.txt) |
| `data/filters/neutral_print_filters.json` | per film/paper neutral C/M/Y values (3.9) | output of the author's balancing calibration over the profiles | not named in the CC BY-SA text (folder is not `profiles`); treat as part of the GPLv3 repository or a derivative of the profiles, i.e. NOT freely reusable. Regenerate with own solver (3.9) |
| `data/filters/dichroics/{durst_digital_light,thorlabs,edmund_optics}` | alternative dichroic spectra (unused by defaults) | digitised from vendor plots/datasheets (Edmund: vendor curve; Durst: plot digitisation; Thorlabs: 1 nm table) | unspecified, vendor-copyright data; do not ship |
| `data/filters/heat_absorbing/schott/KG1,KG3,KG5.csv` | KG3 forms the default `TH-KG3` illuminant | digitised from Schott/Newport heat-absorbing glass datasheet curves (`info.txt` cites newport.com) | unspecified; underlying data is published manufacturer data, digitisation by the author. Replaceable by self-measured/own-digitised curve or a smooth fit |
| `data/filters/lens_transmission/canon/canon_24_f28_is.csv` | only `TH-KG3-L` | lens transmittance from a Lensrentals spectrometry blog post | unspecified; unused by default, do not ship |
| `data/luts/spectral_upsampling/irradiance_xy_tc.npy` (192x192x81 float16, 5.97 MB) and `hanatos_irradiance_xy_coeffs_250304.lut` (4 MB; binary: 4 int32 header, then 4 float32 per cell) | filming spectral upsampling (doc 01), not used by print/scan | supplied by "hanatos" (the author of the vkdt filmsim module; code header thanks him for LUTs and sample code), derived from the spectral-upsampling work of Jakob & Hanika (algebraic sigmoid spectra) | **uncertain**: spektrafilm's CC BY-SA statement is about its own profiles; vkdt is BSD-2-Clause (code) + GPLv3 files, with data repos not licensed in the readme; NOT cleared for closed-source shipment. Regenerate the LUT with Jakob-Hanika-style coefficient fitting (the Jakob&Hanika 2019 paper is open) |
| `data/icc/ellelstone/*.icc`, `data/icc/saucecontrol/*.icc` | embedding in saved images only | Elle Stone's well-behaved profiles (CC BY-SA 3.0), Compact-ICC-Profiles (MIT) | CC BY-SA 3.0 / MIT, not needed by the engine |
| illuminant/CMF tables (CIE D50/D55/D65, Incandescent, Kinoton 75P, CIE 1931 2-degree CMFs) | S1, 2.3 | from the colour-science library (BSD-3-Clause) which embeds CIE tables (CIE data: free to use; attribution to CIE) and a lamp SPD collection (Kinoton 75P / Incandescent entries; their original source was not verified here) | CIE standard data is freely usable; the K75P and T SPDs should be re-sourced or replaced for a closed-source product |
| colour-science constants (matrices CAT02, CAM16, sRGB rounding etc.) | 4.4-4.5 | published standards | free |

Can a commercial closed-source app ship them? Summary: **No, not as is.** (i) The
profiles are CC BY-SA 4.0: share-alike also applies to "direct derivatives... encoding the same
content" (LUTs and other formats), so a closed-source app that embeds profile data (even
recompiled into C++ tables) would have to release those tables under CC BY-SA 4.0 with
attribution; the app code itself would not need to be open source per the author's note
(the note also says paid apps are welcome, no repackaging as paid LUT packs), but whether
embedding data in a binary triggers share-alike on the whole app is the ordinary CC BY-SA
"adaptation" ambiguity - take legal advice or re-derive the data. (ii) The hanatos LUT and
the neutral-filter table have unclear licences. (iii) The Python code is GPLv3: this
specification and the clean-room rule exist to avoid it. Own-measured profiles avoid all of
this.

### 9.2 Profile JSON schema (the fields printing/scanning use)
Top-level `{ "metadata": {...}, "info": {...}, "data": {...} }`.
`info`: `stock` (id), `name`, `type` (`negative` | `positive`; papers: `negative`), `support`
(`film` | `paper`), `stage` (`filming` | `printing`), `use` (`still` | `cine`),
`antihalation` (`strong`|`weak`|`no`), `target_print` (stock id or null), `channel_model`
(`color`|`bw`), `densitometer` (`status_M` films / `status_A` papers & slides),
`log_sensitivity_density_over_min` (0.2 for films, 1.0 for papers), `reference_illuminant`,
`viewing_illuminant`.
`data` (arrays; `null` = NaN): `wavelengths` (81, 380..780 step 5), `log_sensitivity` (81x3,
log10 relative spectral sensitivity per layer R,G,B), `channel_density` (81x3, columns C, M,
Y dye spectra), `base_density` (81; NaN means not measured; papers have none NaN, many films
have NaN at both ends), `midscale_neutral_density` (81, unused here), `log_exposure` (256,
-3..4), `density_curves` (256x3; sampled characteristic curves), `density_curves_layers`
(256x3x3 sub-layer curves, grain only), `density_curves_model` (`{model_type:"cdfs",
centers, amplitudes, sigmas}` each 3x3: channel x sub-layer), the film-only
`hanatos2025_adaptation_window_params` (4) and `_surface_params` (3x15) (doc 01).
Validation rules: shapes above; `log_exposure` length equals `density_curves` rows.

### 9.3 Per-profile limits relevant here
(`dmax` = max of the raw density curve per channel, used as LUT upper bounds; `dmin` ~
-0.0001..-0.0016, below grain `density_min`.)
Films (type, dmax C/M/Y): fujifilm_c200 neg 1.70/2.04/2.64; fujifilm_pro_400h neg
1.58/1.60/2.13; fujifilm_xtra_400 neg 2.13/2.25/2.86; kodak_ektar_100 neg 1.89/1.78/2.27;
kodak_gold_200 neg 1.63/1.51/1.69; kodak_portra_160 neg 1.67/1.74/2.02; kodak_portra_400 neg
1.99/1.80/2.16; kodak_portra_800 neg 1.90/1.73/2.02; portra_800_push1 neg 2.30/1.96/2.26;
push2 neg 2.52/2.01/2.52; kodak_ultramax_400 neg 1.80/1.70/2.06; kodak_verita_200d neg
1.80/1.68/1.98; vision3_200t neg 1.67/1.63/1.85; vision3_250d neg 1.69/1.78/1.91;
vision3_500t neg 1.74/1.64/1.92; vision3_50d neg 1.62/1.70/1.91; positives: ektachrome_100
2.51/2.80/2.81; kodachrome_64 2.91/2.80/2.26; velvia_100 2.73/3.57/2.86; provia_100f
2.56/2.54/2.08.
Papers: kodak_portra_endura 2.46/2.07/1.82; kodak_supra_endura 2.48/2.01/1.92;
kodak_ultra_endura 2.39/1.52/2.13; kodak_endura_premier 2.43/1.89/1.92; kodak_ektacolor_edge
2.25/2.02/1.84; fujifilm_crystal_archive_typeii 2.45/2.19/1.96; cine print films
kodak_2383 3.80/3.29/3.02 (K75P viewing), kodak_2393 4.63/4.48/4.23 (K75P viewing).

---

## 10. Appendices

### Appendix A. Spectral tables (380..780 nm, 5 nm)

Illuminants are normalised to unit mean over the 81 samples (section 2.3). KG3 and lens columns are transmittance fractions (Akima-resampled, section 2.5). xbar, ybar, zbar are the CIE 1931 2-degree colour matching functions as used by the reference (library values, 6 significant digits).

| nm | D50 | D55 | D65 | K75P | T | TH-KG3 | TH-KG3-L | KG3 | lens | xbar | ybar | zbar |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 380 | 0.285009 | 0.379283 | 0.570728 | 0.200355 | 0.0442909 | 0.271155 | 0.0945697 | 0.862712 | 0.31131 | 0.001368 | 3.9e-05 | 0.00645 |
| 385 | 0.316329 | 0.411305 | 0.597409 | 0.302659 | 0.0515858 | 0.293363 | 0.135822 | 0.86224 | 0.413261 | 0.002236 | 6.4e-05 | 0.01055 |
| 390 | 0.34766 | 0.443338 | 0.624091 | 0.412553 | 0.0606954 | 0.315409 | 0.180679 | 0.858845 | 0.511321 | 0.004243 | 0.00012 | 0.02005 |
| 395 | 0.460765 | 0.576397 | 0.784582 | 0.558948 | 0.0805264 | 0.336981 | 0.225979 | 0.852427 | 0.598578 | 0.00765 | 0.000217 | 0.03621 |
| 400 | 0.573882 | 0.709456 | 0.945074 | 0.699999 | 0.100602 | 0.358678 | 0.267735 | 0.845104 | 0.666284 | 0.01431 | 0.000396 | 0.06785 |
| 405 | 0.615805 | 0.753711 | 0.994928 | 0.850219 | 0.119433 | 0.38052 | 0.304922 | 0.837209 | 0.71527 | 0.02319 | 0.00064 | 0.1102 |
| 410 | 0.657739 | 0.797979 | 1.04478 | 0.996796 | 0.137289 | 0.403013 | 0.340412 | 0.83 | 0.753952 | 0.04351 | 0.00121 | 0.2074 |
| 415 | 0.678223 | 0.815567 | 1.05589 | 1.10302 | 0.154671 | 0.426787 | 0.372545 | 0.824676 | 0.77916 | 0.07763 | 0.00218 | 0.3713 |
| 420 | 0.698719 | 0.833167 | 1.06701 | 1.14838 | 0.168665 | 0.451652 | 0.402075 | 0.820652 | 0.794625 | 0.13438 | 0.004 | 0.6456 |
| 425 | 0.685823 | 0.811854 | 1.02846 | 1.16645 | 0.182908 | 0.478065 | 0.431446 | 0.818574 | 0.805562 | 0.21477 | 0.0073 | 1.03905 |
| 430 | 0.672928 | 0.790529 | 0.989925 | 1.21798 | 0.199767 | 0.506671 | 0.462579 | 0.819237 | 0.814928 | 0.2839 | 0.0116 | 1.3856 |
| 435 | 0.771892 | 0.893498 | 1.09375 | 1.30629 | 0.217308 | 0.536658 | 0.495125 | 0.821024 | 0.823525 | 0.3285 | 0.01684 | 1.62296 |
| 440 | 0.870867 | 0.996455 | 1.19757 | 1.41536 | 0.23268 | 0.568077 | 0.528922 | 0.82389 | 0.831081 | 0.34828 | 0.023 | 1.74706 |
| 445 | 0.943156 | 1.06855 | 1.26691 | 1.47612 | 0.251044 | 0.599922 | 0.562668 | 0.82634 | 0.837175 | 0.34806 | 0.0298 | 1.7826 |
| 450 | 1.01544 | 1.14065 | 1.33625 | 1.58183 | 0.26978 | 0.630515 | 0.595826 | 0.826286 | 0.843495 | 0.3362 | 0.038 | 1.77211 |
| 455 | 1.03503 | 1.15503 | 1.34084 | 1.63746 | 0.290521 | 0.661222 | 0.629959 | 0.825833 | 0.850402 | 0.3187 | 0.048 | 1.7441 |
| 460 | 1.05461 | 1.1694 | 1.34543 | 1.7418 | 0.311795 | 0.692091 | 0.66441 | 0.825148 | 0.856903 | 0.2908 | 0.06 | 1.6692 |
| 465 | 1.05901 | 1.1662 | 1.32858 | 1.91914 | 0.333722 | 0.723665 | 0.700554 | 0.82493 | 0.864098 | 0.2511 | 0.0739 | 1.5281 |
| 470 | 1.06341 | 1.163 | 1.31173 | 1.99687 | 0.35452 | 0.757601 | 0.740887 | 0.826978 | 0.872912 | 0.19536 | 0.09098 | 1.28764 |
| 475 | 1.08517 | 1.17945 | 1.31779 | 1.81802 | 0.378584 | 0.793069 | 0.781824 | 0.830189 | 0.879948 | 0.1421 | 0.1126 | 1.0419 |
| 480 | 1.10695 | 1.1959 | 1.32386 | 1.70329 | 0.399583 | 0.829085 | 0.822288 | 0.833478 | 0.885287 | 0.09564 | 0.13902 | 0.81295 |
| 485 | 1.08864 | 1.16878 | 1.28325 | 1.71462 | 0.42212 | 0.865485 | 0.86258 | 0.836718 | 0.889608 | 0.05795 | 0.1693 | 0.6162 |
| 490 | 1.07033 | 1.14164 | 1.24264 | 1.66576 | 0.446707 | 0.902213 | 0.903066 | 0.839898 | 0.893449 | 0.03201 | 0.20802 | 0.46518 |
| 495 | 1.09221 | 1.15679 | 1.24573 | 1.67921 | 0.470499 | 0.93899 | 0.943635 | 0.842814 | 0.89702 | 0.0147 | 0.2586 | 0.3533 |
| 500 | 1.11411 | 1.17193 | 1.24884 | 1.59914 | 0.496321 | 0.9747 | 0.983017 | 0.84456 | 0.90022 | 0.0049 | 0.323 | 0.272 |
| 505 | 1.11928 | 1.17202 | 1.23998 | 1.56892 | 0.522978 | 1.00874 | 1.02056 | 0.844783 | 0.903058 | 0.0024 | 0.4073 | 0.2123 |
| 510 | 1.12445 | 1.1721 | 1.23112 | 1.55055 | 0.547919 | 1.04092 | 1.05645 | 0.843505 | 0.905923 | 0.0093 | 0.503 | 0.1582 |
| 515 | 1.12745 | 1.16798 | 1.21392 | 1.53716 | 0.575537 | 1.07156 | 1.09033 | 0.841155 | 0.908246 | 0.0291 | 0.6082 | 0.1117 |
| 520 | 1.13046 | 1.16386 | 1.19672 | 1.53142 | 0.602386 | 1.10091 | 1.12192 | 0.838052 | 0.909635 | 0.06327 | 0.71 | 0.07825 |
| 525 | 1.15938 | 1.18844 | 1.21327 | 1.52091 | 0.630529 | 1.13193 | 1.15528 | 0.836463 | 0.911017 | 0.1096 | 0.7932 | 0.05725 |
| 530 | 1.1883 | 1.21302 | 1.22983 | 1.52091 | 0.658219 | 1.1639 | 1.18957 | 0.835778 | 0.912292 | 0.1655 | 0.862 | 0.04216 |
| 535 | 1.18048 | 1.20075 | 1.21107 | 1.51068 | 0.686881 | 1.19636 | 1.22439 | 0.835622 | 0.91351 | 0.22575 | 0.91485 | 0.02984 |
| 540 | 1.17266 | 1.18848 | 1.19232 | 1.51029 | 0.717372 | 1.23034 | 1.26081 | 0.836664 | 0.914711 | 0.2904 | 0.954 | 0.0203 |
| 545 | 1.18175 | 1.19352 | 1.19027 | 1.50169 | 0.747604 | 1.2683 | 1.30173 | 0.840474 | 0.916136 | 0.3597 | 0.9803 | 0.0134 |
| 550 | 1.19084 | 1.19856 | 1.18822 | 1.48618 | 0.777826 | 1.30685 | 1.34547 | 0.844679 | 0.91898 | 0.43345 | 0.99495 | 0.00875 |
| 555 | 1.17736 | 1.18129 | 1.16512 | 1.474 | 0.809162 | 1.34286 | 1.38593 | 0.847286 | 0.921229 | 0.51205 | 1 | 0.00575 |
| 560 | 1.16387 | 1.16402 | 1.14202 | 1.46516 | 0.841971 | 1.37571 | 1.42133 | 0.848047 | 0.922206 | 0.5945 | 0.995 | 0.0039 |
| 565 | 1.1507 | 1.14781 | 1.12108 | 1.462 | 0.874315 | 1.40463 | 1.4515 | 0.846638 | 0.922389 | 0.6784 | 0.9786 | 0.00275 |
| 570 | 1.13751 | 1.13161 | 1.10015 | 1.47895 | 0.905941 | 1.4309 | 1.47939 | 0.843969 | 0.922853 | 0.7621 | 0.952 | 0.0021 |
| 575 | 1.1444 | 1.13471 | 1.09703 | 1.48396 | 0.93795 | 1.4554 | 1.50695 | 0.840644 | 0.924219 | 0.8425 | 0.9154 | 0.0018 |
| 580 | 1.15128 | 1.13781 | 1.09391 | 1.47913 | 0.969729 | 1.47718 | 1.53352 | 0.836163 | 0.926649 | 0.9163 | 0.87 | 0.00165 |
| 585 | 1.11974 | 1.10104 | 1.05336 | 1.47172 | 1.00222 | 1.49641 | 1.55481 | 0.830705 | 0.927439 | 0.9786 | 0.8163 | 0.0014 |
| 590 | 1.08821 | 1.06428 | 1.0128 | 1.43577 | 1.03435 | 1.51367 | 1.57593 | 0.824637 | 0.92932 | 1.0263 | 0.757 | 0.0011 |
| 595 | 1.11258 | 1.08167 | 1.02034 | 1.38536 | 1.06665 | 1.52807 | 1.59434 | 0.81753 | 0.931313 | 1.0567 | 0.6949 | 0.001 |
| 600 | 1.13696 | 1.09905 | 1.02788 | 1.33878 | 1.09926 | 1.53815 | 1.60714 | 0.808664 | 0.932643 | 1.0622 | 0.631 | 0.0008 |
| 605 | 1.14616 | 1.10325 | 1.02556 | 1.32256 | 1.13109 | 1.54594 | 1.61783 | 0.799186 | 0.934112 | 1.0456 | 0.5668 | 0.0006 |
| 610 | 1.15536 | 1.10744 | 1.02324 | 1.32293 | 1.16321 | 1.55159 | 1.62641 | 0.789188 | 0.935648 | 1.0026 | 0.503 | 0.00034 |
| 615 | 1.15404 | 1.10209 | 1.01238 | 1.34166 | 1.19515 | 1.55403 | 1.63189 | 0.77816 | 0.937325 | 0.9384 | 0.4412 | 0.00024 |
| 620 | 1.15272 | 1.09673 | 1.00153 | 1.34868 | 1.22565 | 1.55357 | 1.63246 | 0.766299 | 0.937935 | 0.85445 | 0.381 | 0.00019 |
| 625 | 1.1334 | 1.07478 | 0.97635 | 1.32678 | 1.25709 | 1.55006 | 1.62955 | 0.753561 | 0.93838 | 0.7514 | 0.321 | 0.0001 |
| 630 | 1.11408 | 1.05283 | 0.951169 | 1.27598 | 1.28687 | 1.54083 | 1.62102 | 0.738688 | 0.939063 | 0.6424 | 0.265 | 5e-05 |
| 635 | 1.13233 | 1.06378 | 0.953513 | 1.20316 | 1.31755 | 1.53207 | 1.61288 | 0.724693 | 0.939681 | 0.5419 | 0.217 | 3e-05 |
| 640 | 1.15057 | 1.07474 | 0.955858 | 1.12476 | 1.34861 | 1.51949 | 1.59961 | 0.709516 | 0.93967 | 0.4479 | 0.175 | 2e-05 |
| 645 | 1.13201 | 1.0545 | 0.934888 | 1.08246 | 1.37855 | 1.50308 | 1.58066 | 0.693195 | 0.938672 | 0.3608 | 0.1382 | 1e-05 |
| 650 | 1.11344 | 1.03427 | 0.913918 | 1.06773 | 1.40819 | 1.47999 | 1.55605 | 0.674448 | 0.93848 | 0.2835 | 0.107 | 2.11758e-22 |
| 655 | 1.12813 | 1.04279 | 0.914991 | 1.04128 | 1.4385 | 1.45999 | 1.53366 | 0.657752 | 0.937647 | 0.2187 | 0.0816 | 0 |
| 660 | 1.14281 | 1.0513 | 0.916063 | 1.04963 | 1.46846 | 1.43675 | 1.50587 | 0.640205 | 0.935541 | 0.1649 | 0.061 | 0 |
| 665 | 1.17082 | 1.07244 | 0.927844 | 1.04954 | 1.49786 | 1.40323 | 1.4663 | 0.618702 | 0.932723 | 0.1212 | 0.04458 | 0 |
| 670 | 1.19882 | 1.09359 | 0.939625 | 0.985743 | 1.53044 | 1.37184 | 1.42858 | 0.598773 | 0.92952 | 0.0874 | 0.032 | 0 |
| 675 | 1.1763 | 1.07035 | 0.916821 | 0.821432 | 1.5644 | 1.34046 | 1.39081 | 0.579427 | 0.926136 | 0.0636 | 0.0232 | 0 |
| 680 | 1.15378 | 1.0471 | 0.894018 | 0.616553 | 1.59885 | 1.30195 | 1.34528 | 0.55758 | 0.922314 | 0.04677 | 0.017 | 0 |
| 685 | 1.08539 | 0.987283 | 0.845122 | 0.42886 | 1.62789 | 1.26491 | 1.29926 | 0.536924 | 0.916847 | 0.0329 | 0.01192 | 0 |
| 690 | 1.017 | 0.927452 | 0.796228 | 0.265096 | 1.65814 | 1.22614 | 1.25223 | 0.516063 | 0.9116 | 0.0227 | 0.00821 | 0 |
| 695 | 1.04157 | 0.945855 | 0.807008 | 0.155231 | 1.6838 | 1.18021 | 1.20092 | 0.492717 | 0.908266 | 0.01584 | 0.005723 | 0 |
| 700 | 1.06615 | 0.96427 | 0.817787 | 0.0898228 | 1.70818 | 1.13687 | 1.14866 | 0.470957 | 0.901863 | 0.0113592 | 0.004102 | 0 |
| 705 | 1.07363 | 0.975933 | 0.833431 | 0.0624934 | 1.73292 | 1.09265 | 1.09547 | 0.449307 | 0.894905 | 0.00811092 | 0.002929 | 0 |
| 710 | 1.08111 | 0.987597 | 0.849077 | 0.0457131 | 1.7642 | 1.04524 | 1.03927 | 0.426795 | 0.887501 | 0.00579035 | 0.002091 | 0 |
| 715 | 0.987802 | 0.902566 | 0.776302 | 0.0384708 | 1.78699 | 1.00048 | 0.984471 | 0.405789 | 0.878322 | 0.00410946 | 0.001484 | 0 |
| 720 | 0.894482 | 0.817546 | 0.703527 | 0.0271473 | 1.8126 | 0.948058 | 0.923665 | 0.382085 | 0.869638 | 0.00289933 | 0.001047 | 0 |
| 725 | 0.950686 | 0.870311 | 0.750816 | 0.0257626 | 1.84162 | 0.902381 | 0.87057 | 0.361483 | 0.861138 | 0.00204919 | 0.00074 | 0 |
| 730 | 1.00688 | 0.923076 | 0.798104 | 0.0235033 | 1.86352 | 0.854665 | 0.813933 | 0.340411 | 0.850063 | 0.00143997 | 0.00052 | 0 |
| 735 | 1.0422 | 0.956203 | 0.827805 | 0.0213853 | 1.87918 | 0.792226 | 0.745655 | 0.313834 | 0.840132 | 0.000999949 | 0.0003611 | 0 |
| 740 | 1.07751 | 0.989331 | 0.857505 | 0.0209526 | 1.90679 | 0.734521 | 0.683155 | 0.289486 | 0.830183 | 0.000690079 | 0.0002492 | 0 |
| 745 | 0.994005 | 0.913018 | 0.791871 | 0.0224102 | 1.93682 | 0.689229 | 0.630259 | 0.270324 | 0.816234 | 0.000476021 | 0.0001719 | 0 |
| 750 | 0.910497 | 0.836694 | 0.726238 | 0.0217907 | 1.95618 | 0.65225 | 0.590096 | 0.254658 | 0.807546 | 0.000332301 | 0.00012 | 0 |
| 755 | 0.790979 | 0.725612 | 0.62817 | 0.0180283 | 1.97371 | 0.607482 | 0.542267 | 0.236167 | 0.796779 | 0.000234826 | 8.48e-05 | 0 |
| 760 | 0.671461 | 0.614518 | 0.530103 | 0.0159057 | 2.00294 | 0.571755 | 0.50291 | 0.221388 | 0.785125 | 0.000166151 | 6e-05 | 0 |
| 765 | 0.818284 | 0.74916 | 0.646516 | 0.0268102 | 1.93682 | 0.52694 | 0.456726 | 0.203272 | 0.773665 | 0.000117413 | 4.24e-05 | 0 |
| 770 | 0.965118 | 0.883802 | 0.762928 | 0.0273295 | 1.95618 | 0.486676 | 0.413091 | 0.187085 | 0.757644 | 8.30753e-05 | 3e-05 | 0 |
| 775 | 0.938069 | 0.859881 | 0.743385 | 0.0308641 | 1.97371 | 0.44954 | 0.375671 | 0.17225 | 0.74593 | 5.87065e-05 | 2.12e-05 | 0 |
| 780 | 0.911009 | 0.835972 | 0.723841 | 0.0256897 | 2.00294 | 0.414553 | 0.341994 | 0.158368 | 0.736372 | 4.15099e-05 | 1.499e-05 | 0 |

### Appendix B. Neutral print filters (TH-KG3 enlarger source)

Entries are `m / y` in Kodak CC units; cyan is 0 in every entry. Rows: film stock, columns: paper stock. The four positive films carry the placeholder 50 / 50 (not meaningful). Provenance/licence: see 9.1.

| film \ paper | kodak_ultra_endura | kodak_endura_premier | kodak_ektacolor_edge | kodak_supra_endura | kodak_portra_endura | fujifilm_crystal_archive_typeii | kodak_2383 | kodak_2393 |
|---|---|---|---|---|---|---|---|---|
| kodak_ektar_100 | 71.22 / 55.58 | 76.47 / 62.01 | 75.38 / 62.34 | 73.53 / 63.78 | 74.44 / 60.74 | 88.53 / 85.90 | 79.47 / 69.95 | 91.10 / 84.71 |
| kodak_portra_160 | 57.15 / 48.86 | 62.42 / 55.32 | 60.90 / 55.85 | 59.06 / 56.24 | 59.97 / 53.21 | 75.47 / 77.69 | 65.25 / 64.10 | 72.36 / 75.75 |
| kodak_portra_400 | 49.88 / 49.49 | 55.16 / 55.88 | 52.96 / 55.77 | 50.73 / 55.57 | 51.57 / 52.53 | 68.41 / 76.80 | 57.81 / 63.61 | 65.16 / 75.53 |
| kodak_portra_800 | 62.82 / 61.34 | 68.15 / 67.77 | 66.86 / 68.16 | 65.03 / 69.12 | 65.89 / 66.08 | 81.63 / 91.07 | 70.89 / 74.40 | 77.93 / 85.01 |
| kodak_portra_800_push1 | 63.02 / 63.34 | 68.36 / 69.77 | 67.03 / 70.17 | 65.22 / 71.08 | 66.06 / 68.04 | 81.88 / 92.83 | 71.17 / 76.54 | 78.20 / 87.19 |
| kodak_portra_800_push2 | 63.25 / 64.92 | 68.60 / 71.35 | 67.26 / 71.77 | 65.44 / 72.63 | 66.28 / 69.59 | 82.17 / 94.23 | 71.43 / 78.21 | 78.47 / 88.90 |
| kodak_gold_200 | 56.83 / 52.11 | 62.01 / 58.51 | 60.07 / 57.99 | 57.71 / 58.58 | 58.66 / 55.55 | 74.42 / 82.42 | 63.96 / 64.03 | 71.94 / 75.25 |
| kodak_ultramax_400 | 57.14 / 56.00 | 62.39 / 62.40 | 60.15 / 61.80 | 57.78 / 62.20 | 58.66 / 59.17 | 74.95 / 85.10 | 65.10 / 68.73 | 72.98 / 80.26 |
| kodak_vision3_50d | 43.64 / 43.99 | 48.85 / 50.43 | 40.05 / 43.11 | 34.86 / 40.22 | 35.79 / 37.19 | 47.56 / 58.34 | 50.57 / 57.62 | 60.15 / 70.86 |
| kodak_vision3_250d | 44.04 / 40.57 | 49.26 / 47.02 | 40.26 / 39.59 | 34.99 / 36.44 | 35.93 / 33.40 | 47.44 / 54.20 | 50.75 / 54.48 | 60.29 / 67.78 |
| kodak_verita_200d | 59.42 / 61.80 | 64.80 / 68.22 | 63.96 / 69.56 | 62.52 / 70.20 | 63.31 / 67.16 | 79.79 / 90.50 | 67.06 / 75.58 | 72.95 / 85.83 |
| kodak_vision3_200t | 38.01 / 40.47 | 43.26 / 46.92 | 34.16 / 39.57 | 29.01 / 36.27 | 29.91 / 33.24 | 41.72 / 53.33 | 45.03 / 54.78 | 54.32 / 68.21 |
| kodak_vision3_500t | 37.60 / 42.81 | 42.86 / 49.26 | 33.62 / 41.75 | 28.41 / 38.44 | 29.30 / 35.40 | 41.13 / 55.30 | 44.61 / 56.76 | 53.95 / 70.09 |
| fujifilm_pro_400h | 37.47 / 27.25 | 42.46 / 33.66 | 36.85 / 29.32 | 32.49 / 27.66 | 33.54 / 24.63 | 48.15 / 50.74 | 42.57 / 39.94 | 51.75 / 52.70 |
| fujifilm_c200 | 73.84 / 86.40 | 79.12 / 92.75 | 73.73 / 88.81 | 68.89 / 86.91 | 69.71 / 83.85 | 85.91 / 108.32 | 78.61 / 100.12 | 88.81 / 113.31 |
| fujifilm_xtra_400 | 76.53 / 89.28 | 81.81 / 95.62 | 75.44 / 90.63 | 70.14 / 88.47 | 70.96 / 85.41 | 85.85 / 109.10 | 81.53 / 102.67 | 92.11 / 116.03 |
| kodak_ektachrome_100 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 |
| kodak_kodachrome_64 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 |
| fujifilm_velvia_100 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 |
| fujifilm_provia_100f | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 | 50.00 / 50.00 |

---

## 11. Open points

1. **CIE/lamp SPDs are library data.** The K75P ("Kinoton 75P") and T ("Incandescent")
   spectra, the D-series at 5 nm, and the 2-degree CMFs come from colour-science's embedded
   tables; Appendix A transcribes them at 6 significant digits. Rounding to 6 digits
   changes Y by < 1e-6. A closed-source product should verify CIE table provenance and replace the lamp SPDs.
2. **Digitisation provenance of KG3** is not documented beyond the vendor link; its data
   drive the enlarger source tint (every neutral-filter number depends on it). If a different KG3 curve is
   used the neutral database must be re-fitted (3.9).
3. **Neutral filter database vs re-fit:** database values deviate by up to ~0.2 CC units
   (e.g. Ektar 100 + 2383: database 79.47/69.95, re-fit 79.33/69.76 with sRGB input) from a
   re-fit with the current chain; the exact input colour space/version used to create the database is
   unknown. Residual neutral error with database values ~1e-3 relative (grey (0.17946, 0.17932,
   0.17918)), dE2000 < 0.1.
4. **Midgray reference RGB space:** the mid-gray density for the exposure normaliser is
   computed with sRGB-primaries/D65 input regardless of the user's input space (3.5). A
   gray's spectral upsampling depends on the input white point; we replicate by assuming sRGB.
   Not tested whether D65 vs D50 mid-gray changes `g` noticeably (the measured grey outputs
   above vary < 1e-4 across input spaces).
5. **Wavelength masking effect on K75P and papers' dyes**: NaN regions (> 700 nm for several
   papers, < 405 nm) drop out of XYZ. A port using zero-filled spectra gives identical results;
   a port that extrapolates would not. Behaviour for density `inf` unspecified.
6. **CAM16-UCS edge cases:** negative or NaN XYZ (e.g. from extreme input), J' < 1
   (lookup clamps to 1) and chroma knee when `C_max < 1e-9` are handled as documented but
   not exhaustively tested. Behaviour of the library's CAM16 for negative `A` (NaN from the
   power) is unverified; the reference never feeds negative XYZ (scan output is `>= 1e-10`
   before CAT).
7. **Library numerics:** the colour-science `XYZ_to_RGB` with CAT02 and the CAM16
   implementation may have tiny differences in rounding of the HPE/M16 matrices; our
   independent implementation matched to < 1e-5 for the RGB path and exactly (6 digits) for
   the CAM16-UCS forward.
8. **Glare is stochastic:** a faithful per-pixel random field needs the reference's RNG; the
   deterministic mean (3e-4) differs by noise of order 2e-4 in shadow Y.
9. **Print morph (3.8) and `black_white_printing_exposure_correction`** were only inspected,
   not numerically regressed; the Brent root-finding tolerances are as read.
10. **Enlarger `lens_blur`, film `glare`, `enlarger.diffusion_filter` ordering** are as in the
   code but `lens_blur` is dead; confirm with doc 03 for diffusion semantics.
11. **Positive film through the print path** is allowed but untested; the neutral database has
   only placeholders (0, 50, 50) for positives.
12. **LUT creator topologies** (1/2/3/4-LUT) were only skimmed (docstrings), their exact
   below-fog headroom and outward-rounding margin constants were not recorded.
13. **Licence of the hanatos LUT and neutral filter database** unresolved (9.1); a web search of
   github.com/hanatos/vkdt shows BSD-2-Clause code plus GPLv3 files, no data licence statement.
14. **Colour-science version:** the reference pins `colour-science ~= 0.4.6`; numbers
   in this document were produced with 0.4.7.
