# 01 - Data, input handling, spectral upsampling and the filming stage (functional specification)

Clean-room functional specification of the reference film-simulation engine (Python package
`spektrafilm`, version of the profile files: `0.3.2`, profile creation date `2026-05-28`),
covering: the profile JSON schema, what the engine expects as input, how RGB is turned into a
spectrum and then into per-layer film exposure, the density-curve lookup, the DIR (development
inhibitor releasing) coupler model, masking couplers, the full user-parameter schema and the
overall stage order.

This document contains no reference source code. It contains prose, equations, tables of
constants and data-file layouts. Numeric constants and defaults are facts transcribed from the
reference. Companion documents: `03-spatial-effects.md` (blur-like and stochastic effects:
halation, diffusion filter, DIR spatial diffusion, grain, glare, unsharp mask; written
earlier) and the (separate) print/scan document for the enlarger, print paper and scanner.
Where a topic is owned by another document it is only summarised here, with a pointer.

Conventions. Images are floating point (double in the reference), shape H x W x 3,
channel order R,G,B for light/exposure and C,M,Y (equivalently "R-layer, G-layer, B-layer",
i.e. the layer that records red/green/blue light) for density. `lambda` = wavelength in nm.
`log` is base 10 unless written `ln`. `[a:b:s]` is a uniform grid from a to b step s.
`Phi` is the standard normal CDF.

---

## 0. Pipeline overview and stage order

Default (print) topology, executed strictly in this order. The reference calls the named
interfaces "taps"; each stage reads one tap and writes the next. An engine may enter or exit at
any tap (used for LUT baking and debugging).

| Order | Node | Reads tap | Writes tap | Content |
|---|---|---|---|---|
| 1 | preprocess | `rgb_in` | `rgb_pre` | keep first 3 channels, cast to double; auto-exposure gain (optional); compute pixel size; optional crop; optional cubic upscale |
| 2 | filming.expose | `rgb_pre` | `log_e_film` | RGB -> film "raw" (per-layer exposure) via spectral upsampling; `2^EV` exposure compensation; highlight boost; camera diffusion filter; camera lens blur; halation/scatter; black/white exposure correction scalar; `log10(max(raw,0)+1e-10)` |
| 3 | filming.develop | `log_e_film` | `cmy_film` | density lookup from curves (with gamma); DIR-coupler correction; grain |
| 4 | printing.expose | `cmy_film` | `log_e_print` | (other document) |
| 5 | printing.develop | `log_e_print` | `cmy_print` | (other document) |
| 6 | scanning.scan | `cmy_print` | `rgb_out` | (other document) |

If `io.scan_film = true` nodes 4-5 are skipped and node 6 scans `cmy_film` directly. The
dispatcher walks the node list in order and stops as soon as the requested collect tap exists.

Data shapes on the taps: `rgb_in`, `rgb_pre` linear RGB (H,W,3); `log_e_film` log10 exposure per
layer (H,W,3); `cmy_film` density above the per-curve minimum (H,W,3), units: optical density
(log10), "diffuse" density as characterised in the profile.

Physical story (one paragraph, for orientation). Scene RGB is converted to an irradiance
spectrum at each pixel, integrated against the three layer sensitivities to give a
relative exposure per layer; log exposure goes through the layer's characteristic curve to a
dye density; couplers (masking, DIR) modify the effective densities/exposure; the dye
densities are expanded to a spectral transmittance through per-layer dye absorption spectra;
that transmittance is later printed on paper and scanned (other document).

---

## 1. Profile JSON schema

Every film stock and print paper is one JSON file (`src/spektrafilm/data/profiles/<stock>.json`,
file stem = `info.stock`). 28 files ship; see section 1.6. A profile has three top-level
objects: `metadata`, `info`, `data`.

### 1.1 `metadata` (strings; informational only, never used by computation)

`version` (package version, e.g. "0.3.2"), `copyright` ("Copyright (c) 2026 Andrea Volpato.
Licensed under CC BY-SA 4.0."), `created` (ISO date), `license` (CC BY-SA 4.0 statement with
project URL), `citation`, `datasource` (statement that data were digitised from Kodak/Fujifilm
data sheets and scientific papers; reflectance datasets named for the fitting stage: Otsu 2018,
Munsell, NIST skin, forest colours, Japan colours). These strings carry the attribution
required by the licence and must be preserved when redistributing profiles (section 5).

### 1.2 `info` (scalars/strings)

| Field | Type / allowed values | Meaning and runtime use |
|---|---|---|
| `stock` | string | slug = file stem. Used as key into the neutral-print-filter database and for stock-specific parameter overrides (section 3.7). |
| `name` | string | display name. Unused by computation. |
| `type` | `negative` or `positive` | Sign of the process. `negative`: density increases with exposure. `positive`: reversal (slide) film or reversal-type response, density decreases with exposure. Selects the DIR model branch, the grain model branch and the black/white-correction branch. NOTE: all print papers and print films in the database are labelled `negative` (their density rises with exposure). |
| `support` | `film` or `paper` | `paper` = reflective print material. Halation presets are applied only when `support == film`. |
| `stage` | `filming` or `printing` | Role: a `filming` profile is used as the camera film; a `printing` profile as the print medium. The engine does not enforce this; parameters are simply read from the profile given for each role. |
| `use` | `still` or `cine` | Selects halation base-thickness preset (section 3.7). |
| `antihalation` | `strong`, `weak`, `no` | Selects halation strength preset. |
| `target_print` | string or null | Suggested print stock. Not used by the engine (informational / GUI). |
| `channel_model` | `color` or `bw` | All shipped profiles are `color`. `bw` is accepted by the loader; no runtime branch uses it. |
| `densitometer` | `status_M` or `status_A` | Describes the densitometry of the digitised dye curves (Status M for negatives, Status A for positives/papers). Informational; not used by the engine. |
| `log_sensitivity_density_over_min` | float (0.2 or 1.0) | Density above minimum that defined the sensitivity point on the data sheet (0.2 for most camera films, 1.0 for papers/print films and some negatives). Informational; not used at run time. |
| `reference_illuminant` | label | Illuminant for which the stock's sensitivities are balanced to give raw = 1 for an 18.4 % neutral (section 2.4). Values used: `D55` (daylight films), `T` (tungsten films), `TH-KG3` (print stocks: tungsten-halogen enlarger lamp behind a KG3 heat filter). Defines the white point for the spectral upsampling and the gamut compression. |
| `viewing_illuminant` | label | Illuminant for scanning/viewing the processed stock (`D50`, `K75P` for cine prints). Used by the scan stage (other document). |

Legacy keys `fitted_cmy_midscale_neutral_density` and `log_exposure_midscale_neutral` may occur in
old files and are ignored/removed on load. Validation on load: enumerated fields must be in their
allowed sets, and the array shape rules of 1.3 must hold, else the profile is rejected.

### 1.3 `data` (arrays; JSON numbers, `null` = NaN)

All spectral arrays share **one wavelength grid**: `wavelengths = [380:780:5]` nm, 81 samples
(identical in every shipped profile; the engine also hard-codes this grid, "spectral shape
380-780 nm step 5 nm", so a profile on another grid would have to be resampled to it).
All characteristic-curve arrays share **one log-exposure grid**:
`log_exposure = linspace(-3, 4, 256)` (log10 relative exposure, step 7/255 = 0.0274510).

| Field | Shape | Units / domain | Stored meaning and normalisation |
|---|---|---|---|
| `wavelengths` | (81) | nm | as above. |
| `log_sensitivity` | (81, 3) | log10 of relative spectral sensitivity, columns R-layer, G-layer, B-layer | Spectral sensitivity of each emulsion layer, **energy-based, per-sample (no wavelength-width factor), already balanced** such that, for the stock's reference illuminant, the exposure of a neutral 18.4 % grey (RGB 0.184,0.184,0.184) is 1.0 in all three layers when the sensitivities are used through the spectral-upsampling LUT with the window adaptation disabled (verified numerically: 0.99999-1.00001 for all tested stocks; with the default window adaptation enabled the grey gives about 1.013/0.989/0.918 for Portra 400, i.e. the window is intentionally not white-preserving per layer - see 2.6). The array is therefore NOT normalised to max = 0: e.g. Portra 400 spans about -9.25 .. -0.24. The engine uses `S = 10^log_sensitivity`, with any NaN replaced by 0 after exponentiation. No NaN occurs in the shipped files. |
| `hanatos2025_adaptation_window_params` | (4) for films; empty for papers | (c_uv, sigma_uv, c_ir, sigma_ir) in nm | Parameters of an error-function band-pass "window" (section 2.6). Example Portra 400: 433.4923489, 31.5590138, 593.1340731, 68.7632050. Empty array (shape (0)) for print stocks (they never go through camera upsampling). |
| `hanatos2025_adaptation_surface_params` | (3, 15) for films; empty for papers | dimensionless | Per layer (R,G,B rows) 15 coefficients of a bivariate degree-4 polynomial correction surface, c0 (unused) .. c14, in the polynomial order listed in 2.6. **Disabled by default** (`settings.apply_hanatos2025_adaptation_surface = false`). |
| `channel_density` | (81, 3) | optical density per unit "dye amount", columns C, M, Y | Spectral absorption of the isolated dye of each layer, **normalised so that the column peak is the diffuse density of that dye at the data-sheet reference** (the profile builder scales the dye curves so that `channel_density @ density_curve(logE)` reproduces the measured neutral/spectral densities). The engine computes the spectral density of a pixel as `channel_density @ [d_C,d_M,d_Y] + base_density` (section 3.8), with `d` in the units of `density_curves`. Values can be **negative** (min about -0.039): this is how masking couplers are represented (section 3.6). NaN where the source data do not cover the wavelength: typically 380-400 nm and 700/705-780 nm (about 15-80 NaN entries in total depending on the stock). Downstream the NaN propagates into the spectral density and is turned into **zero transmitted light** (not zero density) when converting density to light (see 3.8). |
| `base_density` | (81) | optical density | Spectral density of the unexposed base + fog + (for negatives) the orange mask: added to every pixel's spectral density. Films: Portra 400 range 0.188-0.818. NaN at the wavelength extremes for negatives (4-25 samples). For **papers and positive films `base_density` is all zeros** (the minimum density is carried inside the curves / dye data instead, see 1.5). |
| `midscale_neutral_density` | (81) | optical density | Spectral density of a neutral mid-scale patch. NaN pattern like `channel_density`. **Not used by the runtime** (kept for profile QA/plots). |
| `log_exposure` | (256) | log10 exposure, relative | grid for the curves; 0 corresponds to exposure 1.0 = the exposure produced by an 18.4 % neutral grey under the reference illuminant (since raw is normalised to 1 for that grey). |
| `density_curves` | (256, 3) | optical density, columns C, M, Y (R-layer, G-layer, B-layer) | Characteristic curve per layer for a **neutral grey exposure ramp under the reference illuminant**, i.e. the density of layer m as a function of the log of that layer's own exposure. Includes the effect of the stock's own DIR couplers (the engine undoes this effect before reapplying it, section 3.5). Min over the ramp is generally not zero (base/fog level of the digitised curve); the engine subtracts the per-channel minimum before use (section 3.3). No NaN in shipped files, but the engine uses NaN-aware max/min. |
| `density_curves_layers` | (256, 3, 3) | optical density | Per colour channel, three **sub-layer** curves (fast, mid, slow emulsion sub-layers) whose sum approximates `density_curves`; indices (log-exposure, sub-layer, channel). Used **only by the grain model** (other document, un-normalised). |
| `density_curves_model` | object | see below | Parametric (fitted) form of the curves. **Used by the print stage**: the print-paper density is always evaluated from this model, not from the tabulated `density_curves` (section 1.4). Film profiles carry one too but the film path uses the tabulated curves. |

`density_curves_model` object: `model_type = "cdfs"`; `centers`, `amplitudes`, `sigmas`, each
(3 channels, n_layers) with n_layers = 3 in all shipped files. Meaning (log-exposure axis x):
`D_c(x) = sum_i A_ci * Phi( s * (x - mu_ci) / sigma_ci )` with `s = +1` for `negative`
profiles and `s = -1` for `positive` profiles. Example (Portra 400, channel 0): centers
-0.7123, 0.5038, 1.6738; amplitudes 0.5970, 0.6911, 0.7027; sigmas 0.4451, 0.5320, 0.5265.
The sum of amplitudes of a channel is the maximum density above the floor. These are the
same three sub-layers that appear in `density_curves_layers` (fast = smallest centre).

NaN handling in JSON: `null` is read back as NaN (float64).

### 1.4 Positive vs negative stocks, papers

* **Negative film** (`type = negative`, `support = film`): density rises with log exposure;
  `base_density` non-zero and includes the orange mask; DIR matrix of section 3.5 "negative"
  row; grain model for negatives.
* **Positive (reversal) film** (`type = positive`): density falls with log exposure
  (Dmax at the shadow end of the ramp). `base_density == 0`; DIR uses the "silver density
  = Dmax - D" formulation; stock gamma overrides are much smaller (section 3.5). Positive
  curves are what the viewer sees directly (scan the film), or they may be printed.
* **Print paper / print film** (`stage = printing`, all `type = negative`): sensitivities are
  those of the print material, reference illuminant `TH-KG3`, `densitometer status_A`.
  `base_density` is zero; paper white is encoded by the density floor of the curves and by the
  scan geometry. Print curves are evaluated from `density_curves_model` (1.3) at run time.
  Paper profiles have empty `hanatos2025_*` arrays.
* Which profile fields each role reads (film role / print role): film role reads
  `log_sensitivity`, adaptation arrays, `log_exposure`, `density_curves`, `density_curves_layers`,
  `channel_density`, `base_density`, `info.type/use/antihalation/stock/reference_illuminant/viewing_illuminant`;
  print role reads `log_sensitivity`, `channel_density`, `base_density`, `density_curves_model`
  (and `density_curves`/`log_exposure` for the black/white-correction anchor and for
  min/max bounds), `info.type/stock/reference_illuminant/viewing_illuminant`.

### 1.5 Where "minimum density" lives

Film negatives: fog + mask in `base_density` (spectral, added to every pixel), plus
`density_curves` min (subtracted, so curves start at 0 above base). Positives and papers:
`base_density = 0`; the curves' own floor (min over the ramp) is removed for film; the grain
parameter `density_min` (3 values, default 0.03 each, in the grain parameters) is used by the
print stage as the black reference for negative-density range (other document).

### 1.6 Shipped profiles (28)

All have `channel_model = color`, 81 wavelengths, 256 log-exposure points, 3 sub-layers.
"Win/Surf" = hanatos adaptation arrays present (films) or empty (print stocks).
Illuminants: reference / viewing. Antihalation applies to halation presets (films only).

| stock | type | support | stage | use | antihalation | ref illum | viewing | densitometer | density-over-min | target print | Win/Surf |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fujifilm_c200 | neg | film | filming | still | weak | D55 | D50 | M | 1.0 | fujifilm_crystal_archive_typeii | yes |
| fujifilm_pro_400h | neg | film | filming | still | strong | D55 | D50 | M | 1.0 | fujifilm_crystal_archive_typeii | yes |
| fujifilm_xtra_400 | neg | film | filming | still | weak | D55 | D50 | M | 1.0 | fujifilm_crystal_archive_typeii | yes |
| fujifilm_provia_100f | pos | film | filming | still | strong | D55 | D50 | A | 0.2 | - | yes |
| fujifilm_velvia_100 | pos | film | filming | still | strong | D55 | D50 | A | 0.2 | - | yes |
| kodak_ektachrome_100 | pos | film | filming | still | strong | D55 | D50 | A | 0.2 | - | yes |
| kodak_kodachrome_64 | pos | film | filming | still | weak | D55 | D50 | A | 0.2 | - | yes |
| kodak_ektar_100 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_gold_200 | neg | film | filming | still | weak | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_portra_160 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_portra_400 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_portra_800 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_portra_800_push1 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_portra_800_push2 | neg | film | filming | still | strong | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_ultramax_400 | neg | film | filming | still | weak | D55 | D50 | M | 0.2 | kodak_portra_endura | yes |
| kodak_verita_200d | neg | film | filming | cine | strong | D55 | D50 | M | 0.2 | kodak_2383 | yes |
| kodak_vision3_50d | neg | film | filming | cine | strong | D55 | D50 | M | 0.2 | kodak_2383 | yes |
| kodak_vision3_250d | neg | film | filming | cine | strong | D55 | D50 | M | 0.2 | kodak_2383 | yes |
| kodak_vision3_200t | neg | film | filming | cine | strong | T | D50 | M | 0.2 | kodak_2383 | yes |
| kodak_vision3_500t | neg | film | filming | cine | strong | T | D50 | M | 0.2 | kodak_2383 | yes |
| kodak_2383 | neg | film | printing | cine | strong | TH-KG3 | K75P | A | 1.0 | - | no |
| kodak_2393 | neg | film | printing | cine | strong | TH-KG3 | K75P | A | 1.0 | - | no |
| kodak_portra_endura | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |
| kodak_ektacolor_edge | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |
| kodak_endura_premier | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |
| kodak_supra_endura | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |
| kodak_ultra_endura | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |
| fujifilm_crystal_archive_typeii | neg | paper | printing | still | strong | TH-KG3 | D50 | A | 1.0 | - | no |

`kodak_portra_800_push1/push2` are separate profiles of the same emulsion with curves/dyes
measured after one/two stops of push development: **push/pull is implemented as choosing a
different profile, not as a runtime parameter** (section 3.4).

Default pair when none is specified: film `kodak_portra_400`, print `kodak_portra_endura`.

### 1.7 Loader details a re-implementation must mimic

JSON numbers -> float64; `null` -> NaN. Required array shapes: `log_exposure` (N), `density_curves`
(N,3), `log_sensitivity` (K,3), `wavelengths` (K), `channel_density` (K,3), `base_density` (K),
`midscale_neutral_density` (K). The profile pairs a runtime object with the three groups; the
runtime copies profiles by reference (they are never mutated by ordinary parameter edits).

---

## 2. Input handling and spectral upsampling

### 2.1 What the engine expects as input

* An image array H x W x C (C >= 3, only channels 0..2 are used), float, **scene-referred
  linear** RGB in the colour space named by `io.input_color_space`, **unbounded above 1**
  (values > 1 are valid highlights), no clipping applied by the engine. Negative values are
  not clipped on input (they are handled by the chromaticity clamp, 2.5, and by the final
  `max(raw, 0)`).
* `io.input_color_space` default `"ProPhoto RGB"` (primaries R 0.7347,0.2653; G 0.1596,0.8404;
  B 0.0366,0.0001; white D50 0.3457,0.3585). `io.input_cctf_decoding` default **false**: the
  input is already linear. If true, the colour space's standard decoding curve is applied
  first (sRGB EOTF for sRGB/Display P3/DCI-P3 family as defined by the colour library;
  ProPhoto: ROMM RGB decoding = `v/16` for `v < 0.03125`, else `v^1.8`; BT.2020: ITU inverse
  OETF; ACES2065-1: identity). The colour spaces offered by the reference GUI: sRGB, DCI-P3,
  Display P3, Adobe RGB (1998), ITU-R BT.2020, ProPhoto RGB, ACES2065-1 (the engine accepts any
  colour name known to the colour library).
* **Scaling / middle grey.** The engine's absolute reference is **18.4 % grey = 0.184**
  (not 0.18): a linear neutral of 0.184 maps to log-exposure ~0 (raw ~ 1.0) on the film's
  log-exposure axis. In the reference (the Dapple cross-check harness) the input is supplied as
  linear ProPhoto with the exposure at which mid grey = 0.184, and `camera.auto_exposure` off
  with `exposure_compensation_ev` as the only exposure control.
* RAW development in the reference's own tool chain: LibRaw with camera or custom white
  balance, linear 16-bit **ACES2065-1** (AP0) output, no auto-brightness, gamma 1; it then
  declares the input colour space ACES2065-1. This is a front-end choice, not part of the
  engine. (Dapple provides linear ProPhoto.)

### 2.2 Auto-exposure (preprocess, optional)

Enabled by `camera.auto_exposure` (default **true**; forced off in `lut_mode`). It is a single
global multiplicative gain applied to the full-resolution image **before** crop/upscale:

    image <- image * 2^EV_auto,   EV_auto = -log2( Ymeter / 0.184 ),  Ymeter = weighted mean of Y

* `Y` is the luminance of the input image: Y = second row of the RGB->XYZ matrix of the input
  colour space (after cctf decoding if enabled), no chromatic adaptation. For ProPhoto:
  Y = 0.2880 R + 0.7119 G + 0.0001 B (the library's rounded constants, section 2.3).
* Measured on a thumbnail: long edge at most 256 px, produced by scikit-image-style rescale
  with nearest-neighbour sampling **including the default anti-aliasing Gaussian pre-filter
  applied for down-scaling** (this is what the reference does; verified that anti-aliasing is on).
  Thumbnail dimensions = round(H*s), round(W*s), s = 256/max(H,W). Using a different
  decimation changes EV_auto only slightly (flat-field average).
* `camera.auto_exposure_method` (default `center_weighted`), all return Ymeter before
  dividing by 0.184. Coordinates for the weighted methods: with n = long edge, `x_j = (j/W - 0.5) * W/n`,
  `y_i = (i/H - 0.5) * H/n` (so the long edge spans [-0.5,0.5), the pixel centre offset of half a
  pixel is NOT applied):
  * `average`: mean Y.
  * `median`: median Y.
  * `center_weighted`: Gaussian mask `exp(-(x^2+y^2)/(2*0.2^2))`, normalised to sum 1; Ymeter = sum(Y*mask).
  * `partial`: mean of Y over pixels with radius sqrt(x^2+y^2) < 0.15 (all pixels if none).
  * `matrix`: 5x5 grid, cell size floor(H/5) x floor(W/5) starting at the top-left (the remainder
    rows/columns are ignored); per-cell mean; weight = 0.5*(1+cos(pi*dist)) with
    dist = sqrt(dx^2+dy^2)/sqrt(2), dx,dy = (index-2)/2; weights normalised to 1.
  * `multi_zone`: rings of radius [0,0.05), [0.05,0.25), [0.25,0.5) with weights 0.5,0.3,0.2
    applied to the ring means; normalised by the weights of non-empty rings.
  * `highlight_weighted`: sum(Y^3)/sum(Y^2) (weights Y^2; if sum(Y^2) < 1e-12 use plain mean).
  * any other string: Ymeter = 0.184 (EV = 0).
  If EV_auto is infinite (Ymeter = 0) it is set to 0 (a warning is printed).

After the gain, `exposure_compensation_ev` (user EV, default 0, GUI range +-100) is applied as
a further `2^EV` factor **inside filming.expose to the raw exposure** (equivalent, since the
spectral conversion is linear in the input scale).

### 2.3 RGB -> XYZ with chromatic adaptation to the film's reference illuminant

Let `W_ref` be the chromaticity (x,y) of the film's reference illuminant, **computed from the
engine's own 81-sample illuminant** (2.4) and the CIE 1931 2-degree observer sampled at the
same grid: `X = sum_l I(l) xbar(l)`, similarly Y,Z, `x = X/(X+Y+Z)`. Computed values:

| illuminant | x | y |
|---|---|---|
| D55 | 0.3324316389 | 0.3474443989 |
| T (incandescent) | 0.4507325976 | 0.4080496034 |
| TH-KG3 | 0.4017433451 | 0.3982017988 |
| D50 | 0.345674591 | 0.3585103882 |
| D65 | 0.3127205252 | 0.329030685 |
| K75P | 0.3152468589 | 0.3328584711 |

Conversion of each pixel: `XYZ = M_cat * M_rgb * rgb_linear`, where

* `M_rgb` is the colour space's RGB->XYZ matrix **as hard-coded in the colour library**
  (rounded for some spaces; reproduce these values, do not rederive):
  ProPhoto RGB `[[0.7977,0.1352,0.0313],[0.288,0.7119,0.0001],[0,0,0.8249]]` (native white D50
  0.3457,0.3585); sRGB `[[0.4124,0.3576,0.1805],[0.2126,0.7152,0.0722],[0.0193,0.1192,0.9505]]`
  (white 0.3127,0.3290); Adobe RGB `[[0.57667,0.18556,0.18823],[0.29734,0.62736,0.07529],[0.02703,0.07069,0.99134]]`;
  Display P3 `[[0.4865709486,0.2656676932,0.1982172852],[0.2289745641,0.6917385218,0.0792869141],[0,0.0451133819,1.0439443689]]`;
  BT.2020 `[[0.6369580483,0.1446169036,0.1688809752],[0.2627002120,0.6779980715,0.0593017165],[0,0.0280726930,1.0609850577]]`;
  ACES2065-1 (AP0, white 0.32168,0.33767) `[[0.9525523959,0,0.0000936786],[0.3439664498,0.7281660966,-0.0721325464],[0,0,1.0088251844]]`.
* `M_cat` is a von Kries adaptation from the colour space's native white `xy_src` to `W_ref`, both
  expanded to XYZ with Y = 1, using the **CAT16** cone-response matrix
  `M16 = [[0.401288,0.650173,-0.051461],[-0.250268,1.204414,0.045854],[-0.002079,0.048952,0.953127]]`:
  `M_cat = M16^-1 * diag( (M16 XYZ_dst) / (M16 XYZ_src) ) * M16`  (full adaptation, D = 1).
  Check: ProPhoto -> D55 composite matrix =
  `[[0.7894029154,0.1228212235,0.0444682067],[0.2869884109,0.7134785957,-0.0004666708],[0.0014710101,0.0038855140,0.9157837998]]`.

Then `b = X+Y+Z` (kept as the brightness scale; NaN replaced by 0) and
`xy = (X,Y) / max(b, 1e-10)`.

Rationale: the encoded white of the input (e.g. D50) is moved onto the stock's own balance
illuminant so that a neutral input integrates to a neutral exposure.

### 2.4 Illuminants and observer

Spectral grid for all runtime spectra: 380..780 step 5 nm (81). **Observer:** CIE 1931 2-degree
standard colorimetric observer, values at the grid wavelengths from the standard 1 nm table
(no extra normalisation: `sum(ybar)` over the 81 samples is about 21.37). Sums over wavelength
are plain sums (no delta-lambda factor); normalisations below make this irrelevant except where
stated.

`standard_illuminant(label)` returns the illuminant sampled on the grid and **normalised to have
mean 1 over the 81 samples** (divide by `sum/81`). Labels used by filming:

* `D55`: CIE D55 daylight relative SPD (CIE table, 10 nm tabulated values linearly
  interpolated to 5 nm by the colour library); 81 normalised values in Appendix A.
* `T`: the colour library's measured "Incandescent" light-source SPD (domain 380-780 step 5,
  linear interpolation; note the data end with repeated tail values, see Appendix A).
* `TH-KG3`, `TH-KG3-L`, `D50`, `D65`, `K75P`, `BBnnnn` (black body at nnnn K, Planck radiance):
  used by printing/scanning, defined in the other document. For completeness: `TH-KG3` = 3400 K
  Planck radiance (grid wavelengths in metres) multiplied by the transmittance of Schott KG3
  heat-absorbing glass (digitised curve, Akima-interpolated to the grid), then mean-normalised.

### 2.5 Spectral upsampling: method (default `settings.rgb_to_raw_method = "hanatos2025"`)

**Nature of the spectra: irradiance (emission) spectra, not reflectance.** The method assigns to
a chromaticity `(x,y)` a smooth, bounded *emission* spectrum whose chromaticity equals `(x,y)`
(an RGB pixel is treated as the light that reaches the film, scene colour already containing the
illuminant). There is no explicit illuminant multiplication or reflectance integration in the
default path. (By contrast the alternative `mallett2019` method is reflectance-based and does
multiply by the reference illuminant, 2.8.) The equal-energy white (x=y=1/3) maps to an
(approximately) equal-energy spectrum; with the CAT16 step of 2.3 the stock's white maps to a
spectrum close to the reference illuminant's shape scaled to mean ~0.3.

Data flow per pixel:

1. rgb -> (xy, b) as in 2.3.
2. xy -> `tc` ("triangle-coordinates", a 2-D map chosen so that the visible locus is sampled
   evenly): with `(tx,ty) = (x,y)`:

       tc0 = clip( (1 - tx)^2 , 0, 1 )
       tc1 = clip( ty / max(1 - tx, 1e-10) , 0, 1 )

   inverse: `tx = 1 - sqrt(tc0)`, `ty = tc1 * sqrt(tc0)`. The clip is the only out-of-gamut
   protection at this point (chromaticities outside the lower triangle are clamped on the
   LUT border).
3. A per-film **raw LUT** `R[i,j,m]` (192 x 192 x 3), pre-computed from the spectra table and
   the film's sensitivities (2.6), is sampled at `tc` with the cubic kernel of 2.7.
4. `raw_m = R(tc)_m * b`.

Because the sensitivities in the profile are pre-balanced (1.3) the result is already in
exposure units with neutral 0.184 -> ~1.0 (no further normalisation in the default path).

#### 2.5.1 Spectra table (data file, 192 x 192 x 81)

`irradiance_xy_tc.npy`: NumPy v1 file, dtype float16 (`<f2`), C order, shape (192,192,81):
`T[i,j,l]` is the spectrum (81 samples, 380..780 step 5) at `tc0 = i/191`, `tc1 = j/191`. The
engine promotes to double after loading. The values (min about 2.4e-6, max about 5.85,
strictly positive in every cell) are **exactly the output of the generation recipe 2.5.2 at
lut size 192, smoothing 1 step, rounded to float16** (verified bit-for-bit). An implementation may
ship this table or regenerate it. The XYZ sum of table spectra varies between 13.5 and 23.5
(in the 81-sample-sum CMF units); this variation is part of the data (bounded spectra obey the
MacAdam limit) and must NOT be normalised away.

#### 2.5.2 Generation recipe from the coefficient table (option: regenerate instead of shipping)

Data file `hanatos_irradiance_xy_coeffs_250304.lut` (4 194 741 bytes):

* Header: 4 little-endian int32: `1234, 17039362, 512, 512` (the last two are width, height; the
  first two are a magic/version, ignored).
* Payload: width*height records of 4 little-endian float32 `(c0, c1, c2, c3)`, written with the
  **first index (called i, the tc0 axis) varying fastest within a row, rows indexed by j (tc1
  axis)**: record for (i,j) is at byte `16 + 16*(j*512 + i)`. So the table is `C[i,j,0..3]`, i
  along tc0, j along tc1.
* Trailer (421 ASCII bytes): "spectral upsampling table for Jakob 2019 style sigmoid spectra.
  lookup using triangle-to-quad xy chromaticities. this is meant to be used for emissions,
  i.e. querying at the equal energy white coordinates (1/3,1/3) will yield the equal energy
  spectrum. the fourth channel is the brightness b=X+Y+Z of the corresponding colour
  coordinate. this is not 1.0/constant because the bounded spectra are subject to MacAdams limit."
* Coordinate range: node k of each axis sits at `k/511`.

Recipe (the reference does it at "lut_size" 192 nodes per axis):

1. For each target node `(tc0,tc1) = (i/191, j/191)`, interpolate the four coefficient channels
   from the 512 x 512 table with a tensor-product **cubic spline** (scipy "cubic"
   RegularGridInterpolator, not-a-knot end conditions) on the grid `k/511`. (A bicubic
   Catmull-Rom/Mitchell substitute is a possible acceleration; it was not compared. Expect
   small differences near the border of the table.)
2. On a 1 nm grid `lambda_u = 360, 361, ..., 800` (441 samples): `x(l) = (c0*l + c1)*l + c2`,
   `s(l) = 0.5 + 0.5 * x / sqrt(x^2 + 1)` (Jakob-Hanika sigmoid of a quadratic polynomial; `l`
   in nm), then `s <- s / c3`.
3. Smooth along wavelength with a Gaussian of sigma = **5 samples = 5 nm** (the "smooth_steps"=1
   times the 5 nm grid step, applied on the 1 nm grid), truncated at 4 sigma, reflect boundary.
4. Evaluate at the 81 grid wavelengths 380..780 (linear interpolation; they coincide with 1 nm
   nodes) and store as float16.

The final `s/c3` makes the table entries scale like "spectrum per unit X+Y+Z".

### 2.5.3 Input-gamut compression (default ON, baked into the per-film raw LUT)

`io.input_gamut_compress` = {active = true, algorithm = "xy", knee = (0.0, 1.0, 6.0)}.
Purpose: chromaticities outside the spectral locus (wide-gamut or camera-primary inputs) have no
physical spectrum; they are pulled toward the film white.

For a chromaticity `p` and white `w = W_ref` (the film reference illuminant xy from 2.3):

* `d = |p - w|`, direction `u = (p - w)/d`. `B` = distance from `w` to the spectral locus along `u`:
  the locus is the closed polygon of CIE 1931 2-degree chromaticities of the monochromatic
  stimuli at 380, 385, ..., 700 nm, closed by repeating the first vertex (the purple line
  is the closing edge); first intersection with t > 1e-9, edge parameter in [0,1].
* `d_n = d / max(B, 1e-12)`.
* Reinhard knee with `(t, l, p) = (0, 1, 6)`: for `d_n > t`: `s = l - t`, `n = (d_n - t)/s`,
  `d_n' = t + s * n / (1 + n^p)^(1/p)`; otherwise unchanged. With the defaults
  `d_n' = d_n / (1 + d_n^6)^(1/6)` (always on; e.g. d_n 0.5 -> 0.4987, 0.8 -> 0.769, 1.0 -> 0.891,
  asymptote 1.0).
* `p' = w + u * d_n' * B`. If `d < 1e-9` the point is left unchanged.

Bake: for every cell `(i,j)` of the 192 x 192 raw LUT take its tc, decode to xy (inverse map of
2.5), apply the compression, re-encode to tc' (2.5 map with clip), and set
`R_new[i,j,:] = bilinear sample of R_old at tc'` (grid-index coordinates `tc'*191`, linear
interpolation, **nearest/edge clamp** outside). The per-pixel runtime path is unchanged and
simply samples `R_new`. Note: the compression is thus a function of chromaticity only, not of
brightness, and **applies even to in-gamut colours** (default threshold 0).

Alternative algorithm `"oklch"` (not default): xy at Y=1 -> OkLab -> chroma only compressed with
the same knee against the maximal chroma `C_max(L,h)` of the locus (table 64 L-values in [0.05,1]
x 720 hue values in [-pi,pi), 18-step bisection against the locus polygon, bilinear lookup), L and h
held; result back to xy. Spec validation: threshold in [0,1), limit > 0, power > 0.

### 2.6 Per-film raw LUT construction (sensitivity adaptation)

Inputs: sensitivity `S(l,m) = 10^log_sensitivity` (NaN -> 0) possibly modified by the camera
band-pass filter (2.9); the film's adaptation parameters; settings. The LUT is computed
once per (film, adaptation, gamut-compression, sensitivity) and cached.

    spectra' = spectra table T (192,192,81), double
    if settings.spectral_gaussian_blur sigma_g > 0 (default 0): spectra' = Gaussian blur of T along the
        wavelength axis with sigma_g in samples (i.e. sigma_g*5 nm), scipy default boundary
    if apply_window (default TRUE):
        w(l,m) = band-pass window (below)
        w(l,m) <- w(l,m) / [ sum_l S(l,m) I_ref(l) w(l,m) / sum_l S(l,m) I_ref(l) ]
        R(i,j,m) = sum_l spectra'(i,j,l) * S(l,m) * w(l,m)
    else: R(i,j,m) = sum_l spectra'(i,j,l) * S(l,m)
    if apply_surface (default FALSE): R(i,j,m) *= 2^{ surf(i,j,m) }     (below)
    if input gamut compression active: R <- bake as in 2.5.3

`I_ref` = the film's reference illuminant, mean-normalised (2.4). The window normaliser makes the
**window preserve the exposure of an I_ref-shaped spectrum**, i.e. the white balance of a spectrum
proportional to the illuminant is unchanged (note this is not the same as keeping the
LUT's grey at 1 because the LUT spectrum for white is not exactly I_ref).

**Window ("erf4")**, wavelength-only, identical for the three layers:

    w(l) = 0.5 * ( 1 + erf( (l - c_uv) / (sigma_uv * sqrt 2) ) ) * 0.5 * ( 1 - erf( (l - c_ir) / (sigma_ir * sqrt 2) ) )

with `(c_uv, sigma_uv, c_ir, sigma_ir) = hanatos2025_adaptation_window_params` (nm). Meaning: an
effective camera/optics/emulsion band pass that reduces the UV/blue-violet and the far-red
response seen by the layers relative to the raw sensitivity data. (An 8-parameter "logiflex8"
window variant exists in the reference but is not selected anywhere by default; ignore.)

**Surface (optional, default off).** Per layer m, with polynomial coefficients
`c1..c14 = surface_params[m][1..14]` (c0 not used, i.e. zero at the white point), `tc_ill =
tc(W_ref)`, and `(X,Y) = tc - tc_ill` evaluated on the 192 x 192 tc grid (`tc0 = i/191`, `tc1 = j/191`):

    P = c1 X + c2 Y + c3 X^2 + c4 Y^2 + c5 XY + c6 X^3 + c7 Y^3 + c8 X^2 Y + c9 X Y^2
        + c10 X^4 + c11 Y^4 + c12 X^3 Y + c13 X^2 Y^2 + c14 X Y^3
    surf = P / sqrt( 1 + (P/2)^2 )                      (soft bound to +-2 stops)

R is multiplied by `2^surf` (an exposure change up to +-2 stops per layer vs chromaticity).
Hence neutral (white) is unchanged. The model name "poly4" is the one used; a "poly4_warp_xy" variant
(radial warp) exists in the reference code but its parameter layout is 16 values per layer and it is
unused by the shipped profiles (those have 15).

### 2.7 Runtime LUT sampling kernel (the "raw LUT" lookup)

Input: `tc = (u,v)` in [0,1]^2 (after the clip of 2.5), LUT size L = 192 (nodes at k/(L-1)).
Let `a = clamp(u*(L-1), 0, L-1)`, same for `v`. If `a >= L-1`: base `ia = L-2`, fraction 1; else
`ia = floor(a)`, fraction `fa = a - ia`. The 4 x 4 taps are at integer indices `ia-1 .. ia+2`
along each axis with **reflective index mapping** (`k < 0 -> -k`; `k >= L -> 2(L-1) - k`).
Weights are the **Mitchell-Netravali cubic kernel with B = C = 1/3** evaluated at
`fa+1, fa, fa-1, fa-2` (kernel `k(t)`: for |t| < 1: `(1/6)[(12-9B-6C)|t|^3 + (-18+12B+6C)|t|^2 + (6-2B)]`; for 1 <= |t| < 2:
`(1/6)[(-B-6C)|t|^3 + (6B+30C)|t|^2 + (-12B-48C)|t| + (8B+24C)]`; else 0), 2-D weight = product,
output = sum(weight * LUT) / sum(weights) (the sum is 1 to rounding). **Mitchell(1/3,1/3) is
slightly smoothing, not interpolating**: the sampled value at a node differs slightly from the
node value. Reproduce it for numerical parity. Tiny LUTs (L < 2) fall back to bilinear.

### 2.8 Alternative method `mallett2019` (non-default)

Spectral reconstruction of reflectance from three smooth sRGB basis spectra (Mallett & Yuksel 2019,
3 basis functions on the 380-780/5 grid from the colour library's `MSDS_BASIS_FUNCTIONS_sRGB_MALLETT2019`,
that sum to a flat spectrum of 1). Pipeline: convert input to **linear sRGB** (cctf decoding per
`input_cctf_decoding`, **no chromatic adaptation**, primaries conversion only), then
`raw_m = sum_k lrgb_k * sum_l basis_k(l) I_ref(l) S(l,m)`, divided by
`raw_mid = sum_l 0.184 I_ref(l) S(l,G)` (the green-layer value: **one scalar, so layers are
not individually normalised and a grey does not give exactly (1,1,1)**; measured for Portra 400:
(1.0258, 1.0001, 0.9077)). No gamut compression, no window, no surface are applied in this branch.
NaN -> 0. Data basis files come from the colour library (BSD-3, from the paper's published data);
a re-implementation would need the basis data (not reproduced here).

### 2.9 Camera filters (optional, default off)

`camera.filter_uv = (amplitude, wavelength_nm, width_nm)` default (0, 410, 8);
`camera.filter_ir` default (0, 675, 15). If either amplitude > 0:

    amp = clip(amplitude, 0, 1)
    F_uv(l) = 1 - amp_uv + amp_uv * ( 0.5 + 0.5 erf( (l - wl_uv) / width_uv ) )
    F_ir(l) = 1 - amp_ir + amp_ir * ( 0.5 + 0.5 erf( (l - wl_ir) / (-width_ir) ) )
    F = F_uv * F_ir      (same for the three layers)
    norm_m = sum_l S(l,m) F(l) I_ref(l) / sum_l S(l,m) I_ref(l)
    S'(l,m) = S(l,m) * F(l) / norm_m

(Note erf argument here is `(l-wl)/width` WITHOUT the sqrt2 of the window of 2.6.) This
filter is applied to the sensitivity **before** the LUT is built, so the LUT cache is rebuilt
when it changes. Neutral exposure under I_ref is preserved by the normalisation.

### 2.10 Worked reference values (to validate an implementation)

Settings: film `kodak_portra_400`, print `kodak_portra_endura`, defaults (window on, surface off,
gamut compression on), input linear ProPhoto, auto-exposure off, EV 0, all spatial and
stochastic effects off (so no halation, no DIR blur, no grain). `log_e_film` and developed
`cmy_film` (density above minimum):

| input RGB (ProPhoto linear) | log10 raw (R,G,B) | density C,M,Y with DIR | density without DIR |
|---|---|---|---|
| 0.184,0.184,0.184 | 0.005433, -0.004599, -0.037299 | 0.692898, 0.704642, 0.832530 | 0.689825, 0.702740, 0.845029 |
| 0.0184 grey | -0.994567, -1.004599, -1.037299 | 0.163793, 0.202774, 0.255060 | - |
| 0.736 grey | 0.607493, 0.597461, 0.564761 | 1.017897, 1.006003, 1.185955 | - |
| 0.5,0.1,0.1 | 0.634810, -0.451096, -0.130811 | 1.170164, 0.237869, 0.724925 | 1.029544, 0.493545, 0.793474 |
| 0.1,0.4,0.1 | -0.389908, 0.357974, -0.424760 | 0.444005, 1.122071, 0.474925 | 0.491660, 0.880330, 0.640103 |
| 0.1,0.1,0.5 | -0.684340, -0.060549, 0.414337 | 0.191806, 0.723574, 1.423281 | 0.324617, 0.675836, 1.112827 |
| 0.6,0.4,0.2 | 0.603657, 0.261611, -0.007414 | 1.092154, 0.801189, 0.717192 | - |
| 0,0,0 | -10, -10, -10 | 0, 0, 0 | - |
| 1,1,1 | 0.740615, 0.730583, 0.697883 | 1.094575, 1.074399, 1.264672 | - |

For `kodak_ektachrome_100` (positive) the same inputs give (log raw -> density): 0.184 grey:
(0.010812,-0.000659,-0.019558) -> (0.489029,0.495013,0.524512); black ->
(2.511014,2.804945,2.811332) (Dmax); white 1.0 -> (0.058398,0.051407,0.063604).
Mid grey with window ON gives raw (1.0126, 0.9895, 0.9177) for Portra 400; with window and surface
off, (0.999998, 0.999997, 0.99995). With density_curve_gamma = 1.2 (Portra 400, DIR on), grey
densities become (0.618311, 0.596853, 0.692591) - see Open points on the gamma/DIR interaction.

---

## 3. Filming stage

### 3.1 Exposure: order of operations (`filming.expose`)

Input `rgb_pre` (after optional auto-exposure gain, crop, upscale). Then, in this exact order:

1. `raw = rgb_to_film_raw(rgb)` (section 2): `raw_m = R(tc)_m * b`, double precision.
2. `raw *= 2^(camera.exposure_compensation_ev)`.
3. Highlight boost (`film_render.halation.boost_ev`; default 0 = no-op) - spatial doc 03 s4.1.
4. Camera diffusion filter (`camera.diffusion_filter.active`, default off) - 03 s4.2; pixel size
   `pixel_size_um = film_format_mm*1000 / max(H0, W0)`, see 03 s2.
5. Camera lens blur (`camera.lens_blur_um`, default 0) - 03 s4.3.
6. Halation block: in-emulsion scatter then back-reflection halation (`film_render.halation`,
   default active) - 03 s4.4.
7. `raw *= f_BW`, a scalar from the black/white correction service: **1.0 unless
   `scanner.black_correction or scanner.white_correction` is on AND the film is positive AND
   `io.scan_film` is on** (slide scanning with levels correction). Default off => factor 1.
8. `log_e = log10( max(raw, 0) + 1e-10 )`. The floor 1e-10 means log_e = -10 for black.

The exposure model per layer is therefore

    E_m(x) = b(x) * sum_l T_{tc(x)}(l) * S'(l,m) * w(l,m)        [then spatial effects, EV gain]

i.e. `E_m = integral of (scene irradiance spectrum) x (layer sensitivity)`, with the irradiance
spectrum a smooth function of the pixel chromaticity and its magnitude b, and **no explicit lens
or camera transmission** (the lens/optics, UV cut and layer cross-talk are in the window
`w`, 2.6; the "generic lens transmission" curve in the data folder is only used inside the
enlarger illuminant label `TH-KG3-L`, other document).

Reciprocity, shutter, f-number: not modelled; exposure is relative and set only by EV.
Layer cross-talk, i.e. the response of a layer to light of other colours, is the spectral overlap
in `S`.

### 3.2 `film_render.density_curve_gamma` and the density lookup

`develop_simple`: for each layer m and pixel, with `x = log_e` and `g = density_curve_gamma`
(scalar, or per-layer triple):

    d_m(x) = interp1( x ; xp = log_exposure / g_m , fp = Dn_m )

where `Dn_m = density_curves[:,m] - min_k density_curves[k,m]` (per-layer minimum subtracted;
NaN-aware), `interp1` = **piecewise-linear interpolation with end clamping** (x below the first
node returns the first value, above the last returns the last; for repeated xp values it
follows right-biased "np.interp" semantics, no repeated nodes occur in shipped data).
Equivalently `d_m(x) = D_m(g_m x)` (as `log_exposure/g` is the abscissa): **gamma > 1 steepens
the curve about log E = 0 (contrast up), gamma < 1 flattens it.** The curves are NOT pivoted
on the toe or any other point; the pivot is the origin (mid-grey exposure 1).
Default `density_curve_gamma = 1.0` (GUI minimum 0, step 0.05).

This is the only film-side "development" control in the engine. There is **no development time,
temperature, or push/pull parameter**: push/pull is selected by profile (`kodak_portra_800_push1`,
`push2`), and developer exhaustion / per-layer gamma exist only for the *print* material
("density_curves_morph", other document; default inactive).

Result in the profile unit of optical density, floor 0.

### 3.3 Order inside `develop`

    1. Dn = density_curves - nanmin per channel                      # (256,3)
    2. d  = lookup(log_e, Dn, gamma)                                   # 3.2
    3. d  = DIR correction(d, log_e, Dn, ...)                          # 3.5 (when dir_couplers.active)
    4. d  = grain(d, ...)                                              # other document 03 s5.2
    return d  (cmy_film)

(Black pixels, log_e = -10, map to the first curve value = 0.)

### 3.4 Dev-related parameter summary

| Control | Where | Effect |
|---|---|---|
| `density_curve_gamma` | film_render | contrast scaling about log E = 0 (3.2) |
| stock choice incl. push/pull stocks | profile | different measured curves / dyes |
| DIR couplers `amount`, `inhibition_*`, per-pair `gamma_*` | film_render.dir_couplers | 3.5 |
| `diffusion_size_um`, `diffusion_tail_um`, `diffusion_tail_weight` | dir_couplers | spatial diffusion of the inhibitor, 03 s5.1 |
| `density_curves_morph` (gamma factors, developer exhaustion) | print_render | print only, other document |

### 3.5 DIR-coupler model (full)

Physical idea. When a layer forms dye it releases a development inhibitor in proportion to the
developed silver amount. The inhibitor reduces the effective exposure in the same layer
(self-inhibition: lower contrast, "gamma reduction") and diffuses into neighbouring layers
(interimage effect: increases saturation); with lateral diffusion it also modulates local
contrast ("edge effect"). The model is a shift of **log exposure**, which can be computed
independently per pixel (spatial diffusion is optional).

**Parameters** (`film_render.dir_couplers`, defaults as in the schema; see 3.5.5 for the overrides
that replace the gamma matrix values): `active` (true), `amount` (1.0), `inhibition_samelayer`
(1.0), `inhibition_interlayer` (1.0), `gamma_samelayer_rgb` (3), `gamma_interlayer_r_to_gb`
(2), `gamma_interlayer_g_to_rb` (2), `gamma_interlayer_b_to_rg` (2), `diffusion_size_um` (20.0),
`diffusion_tail_um` (200.0), `diffusion_tail_weight` (0.06). The matrix gamma defaults in the
schema are (0.341,0.324,0.273), (0.355,0.305), (0.154,0.358), (0.171,0.225) but they are
**always overwritten by the digest** (3.5.5) for the stock type; the effective values are the
ones listed there.

#### 3.5.1 Coupling matrix

`M` is 3x3, **row = donor layer k (releases inhibitor), column = receiver layer m (whose
exposure is reduced)**:

    M[k,k] = gamma_samelayer_rgb[k] * inhibition_samelayer
    M[0,1] = gamma_interlayer_r_to_gb[0] * inhibition_interlayer     (R -> G)
    M[0,2] = gamma_interlayer_r_to_gb[1] * inhibition_interlayer     (R -> B)
    M[1,0] = gamma_interlayer_g_to_rb[0] * inhibition_interlayer     (G -> R)
    M[1,2] = gamma_interlayer_g_to_rb[1] * inhibition_interlayer     (G -> B)
    M[2,0] = gamma_interlayer_b_to_rg[0] * inhibition_interlayer     (B -> R)
    M[2,1] = gamma_interlayer_b_to_rg[1] * inhibition_interlayer     (B -> G)
    M <- amount * M

Effective negative-film matrix (rows donors R,G,B; columns receivers R,G,B), verified:

    [[0.336, 0.353, 0.302],
     [0.154, 0.319, 0.353],
     [0.168, 0.226, 0.273]]

#### 3.5.2 "Silver" (inhibitor-producing) quantity

For negatives: `T_k = Dn_k` (density formed). For positives (reversal film): `T_k = Dmax_k - Dn_k`,
where `Dmax_k = nanmax_i Dn_k(i)` (the amount of silver developed in the first development
of a reversal process is the complement of the final dye density; the interimage effects are
assumed to act in that stage). An extra term `shift * T^2` exists with `shift = 0` (not
user-exposed; ignore).

#### 3.5.3 Calibration so that grey ramps reproduce the stock curves

The tabulated curves already contain the stock's own DIR effect. To avoid applying it twice the
engine derives "pre-coupler" curves `D0_m` such that applying the coupler model to a grey ramp
gives back `Dn_m`:

    c_m(x)  = sum_k T_k(x) M[k,m]                (T from the tabulated curve at node x of log_exposure)
    x0_m(x) = x - c_m(x)
    D0_m(x) = interp1( x ; xp = x0_m , fp = Dn_m )                                   (negative film)
    D0_m(x) = - interp1( x ; xp = x0_m , fp = -Dn_m )                                 (positive film)

`interp1` as in 3.2 (linear, end clamped; **xp must be increasing, otherwise results are
undefined** - this holds for the shipped data and default matrices; keep a monotonicity guard).
Note: the gamma factor g is **not** used in this calibration step (the curves are used on the
original log-exposure axis).

#### 3.5.4 Per-pixel application

1. `d(x)` = ordinary density from `log_e` (3.2, includes gamma).
2. `T(x)` from `d(x)` as in 3.5.2 (positives: `Dmax_k = nanmax Dn_k`).
3. Inhibitor field `q_m(x) = sum_k T_k(x) M[k,m]` (3 channels).
4. Optional spatial diffusion of `q` (03 s5.1): when `diffusion_size_um > 0`,
   `q' = (1 - w) * Gaussian(sigma = diffusion_size_um/pixel_size_um) * q + w * Exp(l = diffusion_tail_um/pixel_size_um) * q`
   with `w = diffusion_tail_weight`; when `diffusion_size_um <= 0` the whole spatial step (including
   the tail) is skipped, `q' = q`.
5. Effective exposure: `x' = log_e - q'`.
6. Final density: `d_final_m = interp1( x' ; xp = log_exposure/g_m , fp = D0_m )` (3.2 form,
   gamma applied, using the pre-coupler curves).

For a spatially uniform neutral grey patch at the calibration condition the result reproduces `Dn`
to within interpolation error (Portra 400: grey density changes by ~0.003 between DIR on and off,
table in 2.10); for saturated colours the inter-layer terms desaturate the inhibited layers'
exposure and raise colour density differences (e.g. pure red 0.5,0.1,0.1: M-layer density 0.494 -> 0.238, i.e. more saturated).

Disabled entirely (`active = false`): `d_final = d`.

#### 3.5.5 Digest-time overrides (always applied, `apply_stocks_specifics = true`)

Applied after the user/default values, in this order: film-type override, then stock override.

| Case | gamma_samelayer_rgb | r_to_gb | g_to_rb | b_to_rg |
|---|---|---|---|---|
| any `negative` film | 0.336, 0.319, 0.273 | 0.353, 0.302 | 0.154, 0.353 | 0.168, 0.226 |
| any `positive` film | 0.12, 0.08, 0.06 | 0.12, 0.06 | 0.08, 0.06 | 0.06, 0.06 |
| stock `fujifilm_velvia_100` | 0.108, 0.072, 0.054 | 0.108, 0.054 | 0.072, 0.054 | 0.054, 0.054 |
| stock `fujifilm_provia_100f` | 0.156, 0.104, 0.078 | 0.156, 0.078 | 0.104, 0.078 | 0.078, 0.078 |

(The reference comments record the fitted optimisation results for negatives, e.g. a
"high saturation" candidate `[[0.3416,0.3556,0.3052],[0.1545,0.3246,0.3583],[0.1711,0.2255,0.2731]]`;
the shipped numbers are the table values above. Author notes mark the off-diagonal values
"eyeballed, optimise".)

### 3.6 Masking couplers

Masking couplers (coloured couplers that give a negative its orange mask and compensate the
unwanted side-absorptions of the dyes) are **not simulated as a runtime step**. They are
represented in the profile data:

* the unexposed mask density is part of `base_density` (negatives only; zero for others);
* the mask is consumed where dye forms, which the profile encodes by fitting the isolated dye
  spectra `channel_density` including **negative lobes** (values down to about -0.04) so that
  `channel_density @ d + base_density` reproduces the spectral density of the processed film
  at all exposures. The negative lobes are a faithful part of the model; do not clamp.

Consequently the only run-time work is the matrix product of 3.8. (The reference README
describes this as "the presence of masking couplers is simulated with a negative absorption
contribution in the isolated dye absorption spectra".)

### 3.7 Digest (parameter post-processing before running)

`digest_params` is applied once; executed in this order:

1. **Neutral print filters** (`settings.neutral_print_filters_from_database`, default true): look up
   `database[print.stock][enlarger.illuminant][film.stock] = [C,M,Y]` in
   `data/filters/neutral_print_filters.json` and set `enlarger.c/m/y_filter_neutral`; warn and keep
   defaults (C 0, M 65, Y 55) if absent (print stage, other document).
2. `settings.preview_mode`, then film/print specifics (3.5.5 and the halation preset below), then
   `debug.lut_mode`, `debug.deactivate_spatial_effects`, `debug.deactivate_stochastic_effects`
   (the switch groups are listed in 03 s7; preview_mode zeroes grain, DIR diffusion, blurs, unsharp;
   lut_mode also forces auto-exposure off, exposure compensation 0, print exposure 1,
   boost 0, scanner corrections off).
3. **Halation presets** (films only, keyed by `(use, antihalation)`) set
   `halation_first_sigma_um` and `halation_strength` (R,G,B):

   | use | antihalation | sigma (um, all layers) | strength R,G,B |
   |---|---|---|---|
   | still | strong | 65 | 0.015, 0.005, 0.0 |
   | still | weak | 65 | 0.08, 0.02, 0.0 |
   | still | no | 65 | 0.30, 0.10, 0.015 |
   | cine | strong | 50 | 0.015, 0.005, 0.0 |
   | cine | weak | 50 | 0.08, 0.02, 0.0 |
   | cine | no | 50 | 0.30, 0.10, 0.015 |

   (Rationale in the reference: still = triacetate base 120-140 um thick, cine = PET 95-125 um;
   strength about 2 x first-bounce amplitude / (1 - 0.5).) The schema defaults
   (0.05,0.015,0.0 and sigma 65) are therefore overridden for every shipped film profile.

### 3.8 Spectral density composition (hand-off to printing/scanning)

For a pixel with density triple `d = (d_C, d_M, d_Y)` from `cmy_film`:

    D(l) = sum_{m} channel_density(l, m) * d_m  +  base_density(l)            [81 samples]
    T(l) = 10^(-D(l)) * L(l)                       (L = illuminant), then NaN -> 0 (zero light)

NaN in `channel_density`/`base_density` therefore removes those wavelengths (380-400 nm and
705-780 nm typically) from the transmitted light. The mid-grey reference density used by the
printing stage is this composition evaluated for `rgb = 0.184` grey through the same filming path
(default colour interpretation sRGB, no decoding; neutral so equivalent), without spatial
effects (other document).

---

## 4. Top-level parameter schema

`RuntimePhotoParams` = { `film` (Profile), `print` (Profile), `film_render`, `print_render`,
`camera`, `enlarger`, `scanner`, `io`, `debug`, `settings`, `taps` }. Defaults below are those
of the schema **before** digest. "GUI range" = bounds enforced by the reference GUI; the engine
itself enforces ranges only where stated. Units: um = micrometre, px = pixel.

### 4.1 `camera`

| Field | Default | Meaning / range |
|---|---|---|
| `exposure_compensation_ev` | 0.0 | stops; GUI range -100..100 |
| `auto_exposure` | true | 2.2 |
| `auto_exposure_method` | "center_weighted" | one of center_weighted, matrix, multi_zone, partial, highlight_weighted, median, average |
| `lens_blur_um` | 0.0 | Gaussian sigma in um at film plane (03 s4.3) |
| `film_format_mm` | 35.0 | physical length of the long image edge; GUI 8..120 |
| `filter_uv` | (0.0, 410.0, 8.0) | (amplitude 0..1, edge nm, width nm), 2.9 |
| `filter_ir` | (0.0, 675.0, 15.0) | idem |
| `diffusion_filter` | see below | optional diffusion filter (03 s4.2) |

### 4.2 `film_render`

| Field | Default |
|---|---|
| `density_curve_gamma` | 1.0 |
| `grain` | GrainParams |
| `halation` | HalationParams |
| `dir_couplers` | DirCouplersParams (3.5) |
| `glare` | GlareParams (unused for film) |

`GrainParams`: `active` true; `sublayers_active` true; `particle_area_um2` 0.2;
`particle_scale` (1.6,1.6,3.2); `particle_scale_layers` (2.0,1.0,0.5); `density_min`
(0.03,0.03,0.03); `uniformity` (0.97,0.99,0.97); `blur` 0.65 (px); `blur_dye_clouds_um` 1.0;
`micro_structure` (0.2, 30); `n_sub_layers` 1. (Semantics in 03 s5.2.)
`HalationParams`: `active` true; `scatter_amount` 1; `scatter_spatial_scale` 1; `halation_amount`
1; `halation_spatial_scale` 1; `scatter_core_um` (2.2,2.0,1.6); `scatter_tail_um` (9.3,9.7,9.1);
`scatter_tail_weight` (0.78,0.65,0.67); `boost_ev` 0; `boost_range` 0.3; `protect_ev` 4.0;
`halation_strength` (0.05,0.015,0.0) [overridden by presets]; `halation_first_sigma_um` (65,65,65)
[overridden]; `halation_n_bounces` 3 (GUI 1..5); `halation_bounce_decay` 0.5 (GUI 0..1);
`halation_renormalize` true. (03 s4.4.)
`DirCouplersParams`: 3.5. `GlareParams`: `active` true; `percent` 0.03; `roughness` 0.7; `blur` 0.5 (px).

### 4.3 `print_render`

`glare` (GlareParams, used in print scan) ; `density_curves_morph` (PrintCurvesMorphParams with
`active = false` by default; fields `gamma_factor`, `gamma_factor_fast`, `gamma_factor_slow`,
`gamma_factor_red/green/blue` all 1.0 (GUI 0.5..2.0), `developer_exhaustion` 0.0 (0..1)).

### 4.4 `enlarger` (print stage; listed for completeness)

`illuminant` "TH-KG3"; `print_exposure` 1.0; `print_exposure_compensation` true;
`normalize_print_exposure` true; `y_filter_shift` 0.0; `m_filter_shift` 0.0 (GUI >= -200);
`y_filter_neutral` 55; `m_filter_neutral` 65; `c_filter_neutral` 0 (Kodak CC units, 100 CC =
density 1.0); `lens_blur` 0.0 (dead parameter, 03 s1); `diffusion_filter`; `preflash_exposure` 0.0;
`preflash_y_filter_shift` 0.0; `preflash_m_filter_shift` 0.0.

### 4.5 `scanner`

`lens_blur` 0.0 (px); `white_correction` false; `black_correction` false; `white_level` 0.98;
`black_level` 0.01 (both given as encoded sRGB values, converted to linear for use);
`unsharp_mask` (0.7, 0.7) = (sigma px, amount).

### 4.6 `io`

`input_color_space` "ProPhoto RGB"; `input_cctf_decoding` false; `output_color_space` "sRGB";
`output_cctf_encoding` true; `input_gamut_compress` (2.5.3); `output_gamut_compress`
(other document; default algorithm "cam16ucs", knee (0,1,6), lightness_compression (0.7,1.0,2.2));
`crop` false; `crop_center` (0.5,0.5); `crop_size` (0.1,0.1); `upscale_factor` 1.0;
`scan_film` false.

### 4.7 `debug`, `settings`, `taps`

`debug`: `deactivate_spatial_effects` false; `deactivate_stochastic_effects` false; `print_timings`
false; `lut_mode` false.
`settings`: `rgb_to_raw_method` "hanatos2025" (or "mallett2019"); `apply_hanatos2025_adaptation_window` true;
`apply_hanatos2025_adaptation_surface` false; `spectral_gaussian_blur` 0.0; `use_enlarger_lut` false;
`use_scanner_lut` false; `lut_resolution` 17 (3-D grid nodes for the enlarger/scanner acceleration
LUTs, other document); `use_fast_stats` false; `preview_max_size` 640 (GUI >= 128);
`preview_mode` false; `neutral_print_filters_from_database` true.
`taps`: `inject` and `collect`, both null (= `rgb_in` -> `rgb_out`); valid names: rgb_in, rgb_pre,
log_e_film, cmy_film, log_e_print, cmy_print, rgb_out.

### 4.8 `DiffusionFilterParams` (camera and enlarger)

`active` false; `filter_family` "black_pro_mist" (glimmerglass, black_pro_mist, pro_mist,
cinebloom); `strength` 0.5 (stops: 0, 1/8, 1/4, 1/2, 1, 2, interpolated; GUI 0..2); `spatial_scale`
1.0; `halo_warmth` 0.0 (soft-clamped to +-1.5); `core_intensity`, `core_size`, `halo_intensity`,
`halo_size`, `bloom_intensity`, `bloom_size` all 1.0 (core_intensity GUI 0..4). Semantics in 03 s4.2.

### 4.9 Resolution-dependent parameters

All `*_um` parameters are converted to pixels with `pixel_size_um = film_format_mm * 1000 /
max(H0,W0)` (03 s2); `px` parameters are not. A render at another resolution therefore changes
the look of px-valued effects.

---

## 5. Data files: provenance and licence

| Data | Location | Used by this document | Licence | Closed-source commercial app could ship it? |
|---|---|---|---|---|
| Film profiles (all fields of s1; `log_sensitivity`, `channel_density`, `base_density`, `density_curves`, `density_curves_layers`, `density_curves_model`, adaptation arrays) | `src/spektrafilm/data/profiles/*.json` | yes | CC BY-SA 4.0, author Andrea Volpato; text in `data/license/SPEKTRAFILM_LICENSE.txt` (CC BY-SA 4.0 + a non-binding "spirit" preamble). Underlying measurements are digitised from Kodak/Fujifilm data sheets (the `datasource` field says "original data are property of the respective holders"); the *raw* data-sheet curves are factual, but the fitted/derived arrays (dye unmixing, adaptation parameters, sub-layer fits) are the author's work. | Commercial use is permitted. **ShareAlike**: any adapted/derived data (re-encoded, LUT, resampled, merged) that you distribute must be CC BY-SA 4.0 with attribution ("spektrafilm by Andrea Volpato, https://github.com/andreavolpato/spektrafilm, CC BY-SA 4.0", plus "modified" note); attribution must be visible (about screen). The app code need not be share-alike if the data are shipped as separable files, but this is a legal judgement (see Open points). The author asks, non-bindingly, not to sell the data as a paid LUT pack, not to train commercial AI on them, and not to use the project name in branding. |
| Spectral spectra table `irradiance_xy_tc.npy` (192x192x81 fp16) | `data/luts/spectral_upsampling/` | yes (2.5.1) | The reference README states LUTs are under a custom "commercial use, free share, no resale" licence while `SPEKTRAFILM_LICENSE.txt` says the CC BY-SA 4.0 covers "profiles ... and all direct derivatives ... such as LUTs". Underlying coefficient table is by hanatos (vkdt project: code BSD-2-clause with "some unmarked files GPLv3"; its spectral-input data repository has no stated licence on the pages checked). **Unclear / conflicting**. | **Do not ship until the licences are clarified with the authors.** Safe path: re-derive an independent table from the Jakob-Hanika method (their published rgb2spec code is BSD) with your own fit, or obtain written permission. |
| Coefficient table `hanatos_irradiance_xy_coeffs_250304.lut` | same | optional (2.5.2) | derived from hanatos' work; licence not stated in the file (the 421-byte text is descriptive only). Same status as above. | same |
| Dichroic/heat/lens filter curves | `data/filters/**` | not by filming (print/scan: KG3 heat filter enters the `TH-KG3` illuminant) | digitised from vendor datasheets (Edmund Optics, Thorlabs, Schott KG1/KG3/KG5, Canon 24 mm f/2.8 IS lens, Durst) per the `info.txt` files; no licence file; these are factual transmittance curves digitised from public plots (no copyright in facts, but check vendors' terms). | Re-digitise or use as facts; low risk, document source. |
| ICC profiles | `data/icc/` | none (output embedding only) | Elle Stone profiles CC BY-SA 3.0; saucecontrol Compact-ICC-Profiles MIT | Not needed for the engine. |
| Standard data (CIE 1931 2-degree observer, CIE D-series SPD, CIE A/incandescent measurement, Planck law) | colour-science library (BSD-3) / CIE | yes | CIE tabulated colorimetric data are standards; the "Incandescent" SPD comes from the colour library's datasets | Free to re-use; re-derive from CIE publications. |
| `neutral_print_filters.json` | `data/filters/` | no (print) | author-generated from the profiles; licence not stated (treat as CC BY-SA 4.0 derivative) | as profiles |
| Code | `src/spektrafilm/**` | read for this specification | GPLv3 | **Not copied.** This spec is written in own words; no code appears here. |

Attribution text required when shipping any profile-derived data (from the licence file):
"spektrafilm by Andrea Volpato / https://github.com/andreavolpato/spektrafilm / Licensed CC BY-SA 4.0"
(plus "Derived from ... Modified by [name]" when modified). The README additionally asks to
acknowledge spektrafilm in plugin descriptions/credits and not to use the name in branding.

---

## 6. Suggested validation strategy

1. Parse all 28 profiles; check shapes and NaN positions (1.3).
2. Reproduce the CAT16 composite matrices and illuminant xy values (2.3).
3. Regenerate (or load) the 192x192x81 table; check min/max/sums quoted in 2.5.1.
4. Build the Portra 400 raw LUT with window on; check grey -> (1.0126,0.9895,0.9177) and, with window and
   surface off, (0.999998,0.999997,0.99995) (2.10).
5. End-to-end: table 2.10 with all spatial/stochastic effects off (tolerances: log raw 1e-3,
   density 2e-3; the float16 table and Mitchell kernel must be reproduced to meet this).
6. DIR: compare grey ramps with DIR on/off (should coincide to ~0.01) and coloured patches (table 2.10).

---

## Appendix A. Illuminant tables (mean-normalised, 81 values, 380..780 step 5 nm, 6 significant digits)

D55:

    0.379283 0.411305 0.443338 0.576397 0.709456 0.753711 0.797979 0.815567 0.833167 0.811854
    0.790529 0.893498 0.996455 1.06855 1.14065 1.15503 1.1694 1.1662 1.163 1.17945
    1.1959 1.16878 1.14164 1.15679 1.17193 1.17202 1.1721 1.16798 1.16386 1.18844
    1.21302 1.20075 1.18848 1.19352 1.19856 1.18129 1.16402 1.14781 1.13161 1.13471
    1.13781 1.10104 1.06428 1.08167 1.09905 1.10325 1.10744 1.10209 1.09673 1.07478
    1.05283 1.06378 1.07474 1.0545 1.03427 1.04279 1.0513 1.07244 1.09359 1.07035
    1.0471 0.987283 0.927452 0.945855 0.96427 0.975933 0.987597 0.902566 0.817546 0.870311
    0.923076 0.956203 0.989331 0.913018 0.836694 0.725612 0.614518 0.74916 0.883802 0.859881
    0.835972

T (incandescent, as supplied by the colour library; note the last four values repeat the
preceding four, a quirk of the source data's end handling, reproduced for parity):

    0.0442909 0.0515858 0.0606954 0.0805264 0.100602 0.119433 0.137289 0.154671 0.168665 0.182908
    0.199767 0.217308 0.23268 0.251044 0.26978 0.290521 0.311795 0.333722 0.35452 0.378584
    0.399583 0.42212 0.446707 0.470499 0.496321 0.522978 0.547919 0.575537 0.602386 0.630529
    0.658219 0.686881 0.717372 0.747604 0.777826 0.809162 0.841971 0.874315 0.905941 0.93795
    0.969729 1.00222 1.03435 1.06665 1.09926 1.13109 1.16321 1.19515 1.22565 1.25709
    1.28687 1.31755 1.34861 1.37855 1.40819 1.4385 1.46846 1.49786 1.53044 1.5644
    1.59885 1.62789 1.65814 1.6838 1.70818 1.73292 1.7642 1.78699 1.8126 1.84162
    1.86352 1.87918 1.90679 1.93682 1.95618 1.97371 2.00294 1.93682 1.95618 1.97371
    2.00294

(The tail of T above 760 nm is therefore not monotonic. If an implementation integrates T against
a smooth sensitivity the effect is negligible, but it affects the CAT white `W_ref` for `T` stocks
in the 4th digit; use the listed values and the xy of 2.3.)

---

## 7. Open points

1. **Licence status of the spectral-upsampling LUT is unresolved.** README says "commercial use,
   free share, no resale"; `SPEKTRAFILM_LICENSE.txt` says CC BY-SA 4.0 for profiles and direct
   derivatives; the table is derived from hanatos' (vkdt) coefficient data whose licence I could not
   find (vkdt code is BSD-2-clause with some GPLv3 files; the camera-constant/spectral data repo
   is unlicensed in the pages checked). A closed-source commercial app should not bundle it
   without written permission. A clean alternative (own fit of the Jakob-Hanika sigmoid
   coefficients to a self-chosen spectral dataset, own normalisation) would not reproduce the
   reference numerically, only approximately.
2. **Profile data and ShareAlike scope.** Whether shipping CC BY-SA profile JSON next to a closed binary
   makes the app an "adaptation" is a legal question; shipping them as separate, attributed,
   unmodified files is the conservative reading. Converting them (e.g. to a packed binary, or
   resampling) creates adapted material that must itself be CC BY-SA 4.0.
3. The statement "sensitivities are pre-balanced so a 0.184 grey gives raw = 1" was verified
   numerically (window and surface off) for four stocks; it relies on the spectra table and the
   author's balancing; the window then deliberately offsets it by up to -8 % in blue (Portra 400).
   Whether the window is meant to be white-preserving per layer is unclear (its normaliser uses
   `I_ref`, not the table's white spectrum).
4. The coefficient-table cubic interpolation is scipy's tensor-product spline with not-a-knot
   boundary; a C++ implementation will differ slightly unless the pre-generated 192x192x81 float16
   table is used. The float16 quantisation (relative about 5e-4) is part of the reference's
   numbers.
5. **Gamma / DIR interaction.** Because the pre-coupler curves D0 are built on the un-scaled
   log-exposure axis while the first and last lookups use `log_exposure/gamma`, setting
   `density_curve_gamma != 1` shifts mid-grey density (Portra 400, gamma 1.2: 0.693 -> 0.618 in the first
   layer) instead of pivoting about it. Reproduced as implemented; unclear if intended.
6. For positive films the construction of D0 (3.5.3) requires `x0_m` to be increasing; no guard exists
   in the reference. Large user `amount`/`inhibition_*` values can violate this and produce
   undefined results in the reference.
7. The thumbnail for auto-exposure uses scikit-image's anti-aliased order-0 rescale (Gaussian
   pre-filter, sigma = (scale-1)/2, mirror boundary, then nearest). I verified only that
   anti-aliasing is on, not the exact kernel for every size; the EV difference between plausible
   implementations is expected to be small (hundredths of a stop) but was not measured.
8. `mallett2019` basis data are not reproduced here; that path is non-default.
9. `log_exposure` grid is identical for every shipped profile; the loader does not enforce 256 points
   or the [-3,4] range. The engine's behaviour for other grids is untested.
10. Inert profile fields (`midscale_neutral_density`, `densitometer`, `target_print`,
    `log_sensitivity_density_over_min`, `channel_model`) are not read at run time in this version;
    later versions might use them.
11. `film_render.glare` and `enlarger.lens_blur` exist in the schema but are not consumed by any stage (03 s1).
12. NaN handling: NaN spectral density becomes zero transmitted light only inside the
    density-to-light conversion; other consumers of `channel_density` (e.g. plotting) are not covered.
    Dapple must replicate "NaN -> 0 light" explicitly (a literal 0 density would be wrong).
