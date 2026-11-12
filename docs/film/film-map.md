# Film map (richness x contrast)

`app/assets/film/film_map.json` places every camera stock of the catalog on two measured axes:
`{"id": {"x", "y", "type": "neg|rev|cine", "code"}}`, x = colour richness (淡 -1 .. 浓 +1), y = contrast (低 -1 .. 高 +1).
Regenerate: `scripts/film/gen_film_map.py` (needs the desktop CLI in `build/filmmap/bin/dapple.exe`, built with the flags of
`tools/check-all.sh`; about 1 minute for the 20 stocks). Raw table: `build/filmmap/measure.json` (not committed). `--stocks a,b` measures
only those stocks (smoke test, writes no asset).

## Method

* Input: the 24-patch ColorChecker-like chart of `build/film/chart_exp.py` (sRGB patch values, 64 px patches, PFM in ProPhoto), identical for every stock.
* Render: `dapple film --paper default --no-spatial --auto-exposure match` = the app defaults (the stock's own paper from
  `target_print`, slides unprinted, per-stock print constant, automatic film exposure of pipeline-architecture section 18, scene-key scale 0.5;
  grain, halation and other spatial effects off). The metering is stock independent and the chart is the same, so every stock gets the same
  film exposure (log: scene Y p50 0.200 for all); only the print constant differs per stock, as in the app.
* Richness = mean over the 18 colour patches of C*ab(output) / C*ab(input), CIELAB with D65 white from the sRGB values (not CAM16-UCS).
* Contrast = least-squares slope of output L* against input L* over the 4 mid greys (sRGB 200, 160, 122, 85; the white patch is in the shoulder,
  the black one in the toe). Also stored: toe (52 -> 85), shoulder (200 -> 243) slopes, slope including the black patch, mean hue shift of the
  chromatic patches (C* > 15, degrees, + = counter-clockwise), neutral cast (mean C*ab of the mid greys).
* Mapping, per axis: the catalog median -> 0, the 5th / 95th percentile -> -0.85 / +0.85, each side scaled on its own, clamped to [-1, 1].

## Result (auto-exposure match)

| code | richness | x | contrast | y |
|---|---|---|---|---|
| N800+2 | 1.392 | +0.99 | 1.478 | +0.84 |
| N400B | 1.349 | +0.84 | 1.503 | +0.94 |
| N200A | 1.288 | +0.63 | 1.448 | +0.73 |
| N800+1 | 1.283 | +0.61 | 1.375 | +0.45 |
| R100B | 1.229 | +0.43 | 1.382 | +0.48 |
| N100 | 1.154 | +0.17 | 1.273 | +0.07 |
| N400A | 1.122 | +0.06 | 1.309 | +0.20 |
| N400C | 1.122 | +0.06 | 1.262 | +0.03 |
| N400D | 1.116 | +0.04 | 1.228 | -0.20 |
| N200B | 1.111 | +0.03 | 1.254 | -0.01 |
| N800 | 1.097 | -0.04 | 1.213 | -0.31 |
| C50D | 1.067 | -0.21 | 1.218 | -0.28 |
| N160 | 1.063 | -0.23 | 1.261 | +0.02 |
| C250D | 1.040 | -0.35 | 1.176 | -0.59 |
| C200T | 1.036 | -0.37 | 1.177 | -0.58 |
| N200C | 1.031 | -0.40 | 1.131 | -0.92 |
| C500T | 1.008 | -0.53 | 1.181 | -0.55 |
| R100C | 0.967 | -0.76 | 1.180 | -0.56 |
| R64 | 0.950 | -0.85 | 1.141 | -0.85 |
| R100A | 0.943 | -0.89 | 1.256 | +0.01 |

## Reading it, and limits

* With `--auto-exposure off` (no paper mapping, as the earlier chart studies) the numbers are lower (N800+2 1.31, R64 0.81, the quoted
  1.31x / 0.81x) but the order is nearly the same (Spearman 0.99 richness, 0.97 contrast); the only visible swap is R64 / R100A at the pale end
  (0.811 / 0.860 off, 0.950 / 0.943 on): the two are a tie, the paper/white-black mapping lifts the scanned slides unequally.
* Reversal stocks are not systematically more contrasty by the mid-grey slope (R64 1.14, R100C 1.18 are soft); what sets them apart is the toe
  (slope 1.45 - 1.63 against 0.93 - 1.12 for negatives: deeper blacks) and, for Velvia, everything. Cine stocks are softer in the mid grey
  (about 1.18 against a negative median 1.26) and, like slides, have a steep toe. N200C (Verita) is grouped as a negative in the app but measures like a cine stock.
* Synthetic chart: the patch colours are the nominal ColorChecker sRGB values, not measured, and the chart is one exposure
  (the automatic one for this chart). The numbers describe the default look only, not film_exp, film_shoot, pad moves or other papers.
  CIELAB chroma is not perceptually uniform for blue and saturated patches; the 18 patches are mostly mid-chroma, so very saturated colours are underweighted.
  The map is relative (percentile scaling): adding or removing stocks moves everyone. Nothing here was checked against real film scans.
