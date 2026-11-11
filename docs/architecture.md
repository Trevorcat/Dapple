# Dapple architecture

Dapple turns photos arriving from a camera into finished, shareable JPEGs on a
phone. The heavy lifting lives in one portable C++23 core (`core/`); the
Flutter app talks to it through a small C API, and a desktop CLI uses the same
core for testing and calibration.

```
 camera ──(FTP / USB PTP / vendor app)──> IngestSource ──> IncomingPhoto
                                                              │
                      ImageDecoder (LibRaw / JPEG) <──────────┘
                              │  SourceImage: linear ProPhoto + ShotMetadata
                              ▼
        CameraRegistry.resolve(make, model) ──> CameraProfile
                              │
        AutoAdjust (per photo) + preset/LUT look ──> EditParams
                              ▼
                      Renderer (CPU reference) ──> Image8 ──> JPEG / RGBA
```

## Layers

### Camera profiles (`core/include/dapple/camera.h`, `core/src/camera.cpp`)
Everything camera specific lives in `CameraProfile` and `CameraRegistry`:
raw extensions, sensor type, transfer capabilities (FTP push, USB PTP, vendor
app auto-send), and the rendering baseline that makes an untouched RAW look
like that maker's JPEG (`baselineExposureEv`, `baselineContrast`,
`baselineHsl`, and the fitted `colorMatch`). `resolve()` matches exact make+model (case-insensitive), then a
maker-level generic profile (empty `model`), then the global generic profile.
It never fails, so an unknown body still develops.

### Ingest sources (`ingest.h`, `ingest.cpp`)
`IngestSource::poll()` delivers `IncomingPhoto`s. `FolderWatchSource` is the
portable implementation (size-stability polling so a 40 MB upload that is still
in flight is not read early); the FTP receiver on the phone writes into a
folder and reuses it. A PTP source or a media-store source implements the same
interface. The develop pipeline only sees `IncomingPhoto`.

### Decoders (`decoder.h`, `decoders/`)
`ImageDecoder` (`canDecode`, `probe`, `decode`) with two implementations,
selected by `DecoderRegistry`:

* `libraw` - ARW, RAF (X-Trans), DNG, CR2/CR3, NEF, ... Output is requested as
  linear 16-bit ProPhoto RGB, camera white balance, no auto-brightness, no
  highlight reconstruction, EXIF/maker-note orientation applied. `Preview`
  uses LibRaw half-size (only effective for true CFA data, see below).
* `jpeg` - 8-bit JPEG/PNG, linearised sRGB moved to linear ProPhoto,
  `sceneReferred = false`. Reads EXIF Make/Model and applies EXIF orientation.

All file IO uses `std::filesystem::path` built from UTF-8, so Chinese paths work
on Windows. `extractEmbeddedPreview()` returns the camera's own JPEG preview
(used for calibration and as an instant placeholder).

Note: the A7R V's M and S RAW sizes are binned, already 3-colour images
(no CFA mosaic). LibRaw reports `filters == 0` for them, `ShotMetadata::cfa` is
`None`, and "half size" brings no speed-up; decode time is dominated by the
lossless-compressed entropy decoding. Real CFA data (L-size ARW, RAF) takes the
half-size fast path.

### Pipeline and colour spaces (`renderer.h`, `renderer.cpp`)

| stage | space | notes |
|---|---|---|
| decode | linear ProPhoto (ROMM, D50), as-shot WB | 1.0 = sensor clip for RAW, display white for JPEG |
| white balance, exposure | linear ProPhoto | temperature/tint gains (luma-preserving) times 2^(EV + camera baseline EV) |
| tone | log2 of luminance around 0.18 | camera contrast, deep-shadow toe (scene only), highlights/shadows/whites/blacks sliders, then highlight shoulder (scene only), then the tonal-class move (`toneTarget`/`toneStrength`, on sRGB-encoded luminance) |
| colour | linear sRGB, then OKLab | camera matching profile (3x3 matrix + OKLab offset LUT, see below) or the coarse camera HSL baseline, then user HSL, vibrance, saturation, colour grading |
| gamut map | linear sRGB | desaturate towards same-luminance grey into [0,1] |
| encode | sRGB | |
| curves, LUT, vignette | display-referred sRGB | parametric + point curves (master, R, G, B), `.cube` LUT with tetrahedral interpolation, vignette |
| output | 8-bit sRGB | |

Scene-referred sources (RAW) get the camera baseline, toe and shoulder.
Display-referred sources (camera JPEG) skip them, so zero edits is a near no-op
(tested). The CPU renderer is the ground truth that the mobile GPU shaders must
match.

### Presets and LUTs (`xmp_preset.*`, `lut.*`)
`loadXmpPreset` reads Lightroom/Camera Raw `.xmp` presets (attribute and
element form, point curves as `rdf:Seq`) into `EditParams` and returns a list of
settings it does not reproduce (clarity, dehaze, texture, camera profiles, ...)
so the UI can be honest. `.cube` 3D LUTs are applied in sRGB at the end with a
strength slider (`lutAmount`).

### Automatic correction (`auto_adjust.*`)
Deterministic, no ML, runs on a 512 px proxy, and is built to *do no harm*: the camera's own
rendering (which the untouched render now matches closely) is the baseline of "correct", and a
correction needs clear evidence of a defect.

* **Exposure**: no EV change for a frame that has a normal range. Only a frame where nothing
  reaches a normal highlight (rendered p95 < 0.45 and p50 < 0.16) is lifted, part of the way
  (max +0.8 EV). Low-key and high-key photos are never pulled towards middle grey.
* **Tonal class**: `toneStrength 60` with an empty target (the photo's own class). Only the part of
  the deviation beyond the class tolerance (p50 0.15, p5/p95 0.10 of encoded luminance) is
  corrected, and only if the resulting curve moves more than 1% anywhere, so a photo typical of its
  class is untouched.
* **White balance**: never for a RAW whose as-shot multipliers are plausible. Otherwise (JPEG, or
  an implausible as-shot) only with strong evidence: neutral candidates are unclipped mid-tone
  pixels with OKLab chroma < 0.035, excluding blue-dominant hues (sky, clouds, shade);
  they must cover > 25% of the frame, the mean cast must exceed 0.02 and > 85% of the pixels must
  agree on its direction. The correction is half of the solved shift, capped at 12 temperature /
  tint units.

The result is an `EditParams` delta that `Developer` adds on top of the look, plus `ImageStats`,
the `ToneAnalysis` and human-readable notes. (The previous version moved the median towards a fixed
0.19 and the neutral cast towards zero on every photo; on the sky photo it read the clouds as the
cast, +22 temperature / -19 tint, and added +0.32 EV.)

### Tonal classification and tone target (`tone.h`, `tone.cpp`)
`analyzeTone()` renders the photo with its look at 256 px and measures the *encoded* luminance of
what the viewer sees: a 64-bin histogram and p5 / p50 / p95. The nine classes are key
{low, mid, high} x range {short, mid, long}, each a prototype in (p5, p50, p95), e.g. mid-mid
(0.10, 0.46, 0.76); a photo belongs to the nearest one (`nearest`, `key`, `range`, `distance`,
`margin`). `EditParams::toneTarget` (a class name, empty = nearest) and `toneStrength` (0..100)
move the image towards a class: a monotone curve through anchors at p5, p50, p95 (0 and 1 fixed)
applied to the luminance ratio, so chroma is preserved. The shifts are limited (p50 +-0.10, p5 +-0.04,
p95 +-0.05 before strength), true blacks are never lifted and clipped highlights never pushed, and
slopes stay in 0.4..2.5. The C API returns the analysis as JSON with `dapple_session_analyze_json`
(`dapple_session_analyze_params_json` for a given edit; `dapple_session_render_tone_previews` renders the nine class targets). Prototype values are set from the
distribution of the sample photos (median p50 0.46 = 18% grey); only daytime photos were available,
so the low-key prototypes are by construction rather than measured.

### EditParams (`edit_params.*`, `look.*`, `spatial.*`)
The single renderer-independent description of an edit. The JSON keys are the flat contract keys of
`docs/design/requirements.md` 6.3 (74 numeric keys plus `look`, `lookStrength`, `palette {x,y}`,
`toneTarget`, `toneStrength`, `crop`, `rot`, `flip`, `lut`/`lutAmount`, point curves). One field table
(`edit_params_fields.h`) drives `clamp()`, `operator+=`, `toJson`, `toJsonDiff`, `fromJson` and
`paramsSchemaJson()`, so the keys, ranges and defaults cannot drift apart. `fromJson` is tolerant
(unknown keys ignored, missing = default, quoted numbers, `{"params": {...}}` unwrapped, legacy keys
`temperature`, nested `hsl`, `gradeShadows`, `exposureEv` still read) and clamps every value. Note:
JSON `exposure` is in UI units (EV x 20, +-100); `EditParams::exposure` stays in EV.
Defaults that are not zero: `sharpen` 40, `sharpRadius` 10, `nrColor` 25, `gradeBlend` 50, `lookStrength` 100.
For RAW input they are the develop baseline (so an untouched RAW is sharpened a little); for
display-referred input (camera JPEG) they are the no-op level and only the excess is applied. With
`sharpen 0`, `nrColor 0` the render is byte-identical to the previous untouched render.

Pipeline order of the full parameter set: geometry front end (rotate 90 -> straighten with auto-fill
crop of the chosen ratio -> flip; pure index mapping unless straightened, then Catmull-Rom in linear
light; done first so vignette follows the crop, sharpening comes after resampling and no work is spent
on cropped pixels) -> white balance -> exposure -> tone (log domain: contrast, highlights, shadows,
whites, blacks, tone class) -> colour in OKLab (camera match, HSL mixer, vibrance, saturation,
**style + palette + strength**, **grading**: shadows/mid/highlights/global with hue, saturation,
luminance, blend = width of the zone cross-over, balance = its centre) -> gamut map -> sRGB encode ->
curves (master + R/G/B, each parametric 4 zones then point curve) -> LUT -> film fade -> vignette ->
**neighbourhood stage** (`spatial.cpp`, on encoded sRGB, acting on luminance or R-Y/B-Y so colour does
not shift): luminance NR (edge-stopped blend to a box blur) and chroma NR (on a 1/2 or 1/4 copy,
edge-stopped by luminance) -> dehaze (dark-channel transmission on a 1/4 copy, negative = veil) ->
clarity (fast guided filter on a 1/4 luminance copy; detail = Y - base, mid-tone weighted), texture
(small box base, compressed detail so edges do not halo) and sharpening (Gaussian unsharp on luminance,
soft-limited and noise-gated) accumulated and applied once -> grain (deterministic value noise, mid-tone
weighted; scales with image size). Radii are defined for a 2048 px long edge and scale with the image.
NR runs before the detail operators and grain last so neither is undone.

Colour styles (`look.cpp`): `std warm amber rose neutral cool teal film bw`, engine recipes translating the
prototype's CSS placeholders into OKLab (saturate -> chroma gain, sepia -> warm tint, hue-rotate -> hue
rotation, grayscale -> chroma removal, contrast/brightness -> S-curve slope / L offset); teal gets a real
shadow-teal / highlight-orange split tone, film a lifted black point and grain, bw removes chroma.
Palette x scales chroma by `1 + (x-0.5)*0.9*s`; y < 0.5 adds a warm tint, y > 0.5 a cool tint (max chroma
offset 0.032 in OKLab). `s = lookStrength/100`; every part scales with `s`, so `lookStrength 0` is exactly
the unstyled picture. The tone analysis renders without the neighbourhood stage.

### C API (`dapple_c.h`, `c_api.cpp`)
The frozen contract with Flutter (`dart:ffi`). The shared library `dapple`
(`libdapple.so` on Android, `libdapple.dll` on Windows) links the core
statically and exports only these functions. Sessions hold a decoded
`SourceImage` so slider dragging re-renders without re-decoding; renders return
RGBA8 owned by the library (`dapple_free_image`). Errors: thread-local
`dapple_last_error()`. `.cube` LUTs are cached by path (invalidated when the
file's mtime changes). No exception crosses the boundary.


New C functions: `dapple_looks_json`, `dapple_params_schema_json` (every key with range/default),
`dapple_session_render_look_previews` (nine style thumbnails), `dapple_session_render_tone_previews`
(nine tonal-class thumbnails, strength max(60, toneStrength) by default). Existing functions are unchanged.

## Colour profile: matching the camera JPEG (Sony ILCE-7RM5)

The untouched render is tuned to resemble the camera's embedded JPEG. Matching *mean luminance*
(the first calibration) hid colour errors, so the profile is now fitted on pixels:

1. **Tone** (unchanged): `baselineExposureEv = 1.4`, `baselineContrast = 20`, toe and shoulder, from
   the median response of 50 ARW/JPEG pairs.
2. **Registration**: the in-camera JPEG engine applies lens corrections, so the preview is not framed
   like the decoded RAW (zoom 0.96..1.015, a small radial term, a few px shift). Every pair is
   registered first (`eval_common.cpp`: zoom + radial term + shift maximising the NCC of high-passed
   luma, 0.47 -> 0.97 on average) and the unusable border is excluded. Without this every number
   is dominated by edge misalignment.
3. **Fit** (`dapple fit-profile`): training = files with index mod 10 in {4,5,6} of the sorted file
   list (192 files), held-out = index mod 10 = 0 (64 files, never used for fitting, and at least
   4 frames away from any training frame so bursts of one scene do not leak). Pairs are our render
   without any colour profile (at 640 px) and the registered camera JPEG, 5.4 M pixels.
   Stage 1: 3x3 matrix in linear sRGB, rows summing to 1 (neutrals preserved), weighted least squares.
   Stage 2: an OKLab offset LUT (13 L x 17 a x 17 b nodes, a/b to +-0.32; node values are
   additive dL, da, db; trilinear lookup) by regularised least squares (sparse normal equations +
   conjugate gradients, smoothness 0.01 of the average node weight) with two IRLS passes (Huber) to
   ignore misregistered edges and clipped pixels. Larger grids (up to 25 x 25) and weaker or
   stronger smoothing changed the held-out error by < 0.02 dE (stronger smoothing, 10, costs 0.5).
4. The result is `core/src/profiles/sony_ilce7rm5.inc` (generated), stored in
   `CameraProfile::colorMatch` (`ColorMatchProfile`: matrix + LUT). It replaces `baselineHsl` for this
   body (`sony.generic` keeps the old HSL baseline). The renderer applies it right after the tone
   stage on scene-referred sources, before user colour edits. Another camera gets its own by running
   `fit-profile` on its ARW/RAF + embedded JPEG pairs and including the generated file.

The fit found a systematic **neutral error** that the HSL baseline could not express: without the
profile a neutral is about +4 b* (too yellow) against the camera JPEG, and the cyan/purple/red hue
errors and the chroma ratios depended on lightness, which a per-hue band cannot express.

Held-out result at the fit resolution: mean CIEDE2000 4.72 (no profile) -> 4.40 (matrix only) ->
2.40 (matrix + LUT); training 2.23 (small generalisation gap). What remains is not colour: the camera
applies sharpening, local contrast and lens shading/vignetting correction, which Dapple does not.

## Evaluation tool (`dapple eval`)

    dapple eval --dir D:/photos --step 10 --out build/eval [--edge 1280] [--cmp-edge 640] [--no-match]

For every N-th ARW (sorted by path): decode (cached in `<out>/cache`), render the untouched look and
the `--auto` look at `edge`, register the camera JPEG to it (reported per image), downsample both to
`cmp-edge` and compare in CIELAB D65: CIEDE2000 mean / p95, mean dL, chroma ratio, neutral a*/b*
error, and signed hue error plus chroma ratio per camera-hue sector (reds, oranges/skin, yellows,
greens, cyans, blues, purples; camera chroma >= 10). Outputs in `--out`: `eval.csv` (per image and
variant, incl. auto parameters and tonal class), `eval_sectors.csv`, `summary.txt` and
`sheet_NN.jpg` contact sheets (camera | ours | ours + auto, six photos per sheet, index in the
corner; the camera image is shown registered to our framing). `--no-match` switches the fitted
profile off.

Results on the 64-photo sample (every 10th file; none of them used for fitting):

| | before | after |
|---|---|---|
| dE2000 mean, untouched | 3.97 | 2.29 (2.24 with default sharpen 40 / chroma NR 25) |
| dE2000 p95 (per image mean), untouched | 6.84 | 4.40 |
| dE2000 mean, auto | 4.67 | 2.30 |
| auto - untouched, mean over images | +0.69 | +0.01 |
| images where auto is worse | 31 of 64 by > 0.05 dE (24 by > 0.5) | 4 of 64 by > 0.1 dE (none by > 0.5) |
| chroma ratio ours / camera | 0.96 | 1.00 |
| hue error (deg): reds / oranges / yellows / greens / cyans / blues / purples | +2.9 / +2.4 / -2.6 / -4.0 / -1.8 / -1.1 / +3.5 | +1.5 / +0.7 / -1.3 / -1.1 / +3.3 / -0.1 / -0.8 |
| blues chroma ratio, untouched / auto | 0.98 / 0.84 | 1.05 / 1.05 |

## How to add a camera

1. Add a `CameraProfile` in `builtinProfiles()` (`core/src/camera.cpp`):
   `id`, `make`/`model` exactly as LibRaw reports them (see `dapple info`),
   `rawExtensions`, `cfa`, `transfer` capabilities, notes.
2. Calibrate: set `baselineExposureEv` / `baselineContrast` from the median tone response (see
   `dapple thumb`, `dapple dump-linear`), then run `dapple fit-profile --dir <raws> --name <id>
   --out core/src/profiles/<id>.inc` and include the file in `camera.cpp`; check with `dapple eval`.
3. If the format is not supported by LibRaw, add an `ImageDecoder` and register
   it in `DecoderRegistry::withBuiltins()`.
4. If the body needs a new transport, add an `IngestSource`.

Nothing in the renderer, auto-correction or C API special-cases a model.
