# Film simulation in Dapple

Spectral film + print simulation (`core/include/dapple/film.h`), driven by the profile data of
**spektrafilm** (Andrea Volpato). Architecture, performance and the GPU mapping: `architecture.md`; functional
spec: `spec/`; data provenance: `core/data/film/NOTICE.md`; licence audit of every data item: `data-licensing.md`.

## Using it from the edit parameters

Set `film` to a stock id. Everything else is optional:

| key | range | default | meaning |
|---|---|---|---|
| `film` | stock id, "" = off | "" | camera film (kind `negative` or `slide`), e.g. `kodak_portra_400` |
| `film_paper` | paper id / "none" / "" | "" | print paper or print film (kind `paper`); "none" scans the film itself; "" = default for the stock (negatives: `kodak_portra_endura`, cine stocks: `kodak_2383`, slides: none) |
| `film_strength` | 0..100 | 100 | blend of the film render against the normal pipeline |
| `film_exp` | -60..60 (= -3..3 EV x 20) | 0 | exposure offset at the camera |
| `film_warmth` | -100..100 | 0 | print filtration, positive = warmer |
| `film_tint` | -100..100 | 0 | print filtration, positive = more magenta |
| `film_grain` | 0..100 | 100 | grain amount (100 = the stock's reference grain) |
| `film_halation` | 0..100 | 100 | halation + scatter amount |

The film replaces tone + colour + look; WB / exposure act before it, curves / LUT / vignette / crop / detail after
it. A stock the library does not contain is ignored (normal pipeline). Details: `architecture.md` section 7.

```
dapple develop photo.ARW --params '{"film":"kodak_portra_400","film_warmth":15,"film_grain":60}' -o out.jpg
dapple film-render-bench          # develop pipeline timings with the film on
dapple film-info                  # list the stocks
```

## Making the pack available

The pack is **one binary file**, `core/data/film/dapple.dfp` (1.2 MB), built from `core/data/film/profiles/*.json`:

```
dapple film-pack core/data/film/profiles core/data/film/dapple.dfp
```

* **Desktop / CLI / tests**: found automatically as `core/data/film/dapple.dfp` relative to the working directory (up
  to three levels up) or via `DAPPLE_FILM_PACK=<file.dfp | profiles dir>`.
* **Android**: ship it as an asset. Copy `core/data/film/dapple.dfp` into the app's assets (Flutter: list it under
  `flutter: assets:`; native: `app/android/app/src/main/assets/film/dapple.dfp`), copy it once to a readable file
  (filesDir) or read it into memory, then call `dapple_film_load_pack(path)` or
  `dapple_film_load_pack_memory(bytes, size)`; `dapple_film_set_pack_path(path)` only records the path and loads on
  first use (keeps app start fast). No gradle step builds it: the `.dfp` is a checked-in asset, regenerate it with the
  command above when the profiles change (`film_tests` verifies pack vs JSON). The Gradle `externalNativeBuild`
  compiles the NEON path automatically (arm64-v8a).
* **Flutter app (done)**: `app/assets/film/` holds copies of `dapple.dfp`, `NOTICE.md` and `SPEKTRAFILM_LICENSE.txt`
  (declared in `app/pubspec.yaml`; `app/test/film_assets_test.dart` fails when they differ from `core/data/film/`, refresh with
  `cp core/data/film/{dapple.dfp,NOTICE.md,SPEKTRAFILM_LICENSE.txt} app/assets/film/`). `AppState.init` reads the asset with
  `rootBundle` and calls `dapple_film_load_pack_memory` (2 ms), then `dapple_film_list_json` feeds the 风格 window. Stock display
  names (neutral codes such as N400C / R100B / C250D / P4, never trademarks; scheme in the table's doc comment) and grouping live in
  `app/lib/model/film_names.dart` only; swatch colours are rendered by the engine (`dapple_film_swatches_json`: skin, grey and sky
  patches through each stock, cached in `film_swatches.json` in the app documents dir, keyed by pack size + stock ids + a version). The licences page (设置 > 关于 > 开源许可) shows
  `dapple_film_attribution()`, the notice and the licence text.
* **iOS**: same, as a bundle resource + `dapple_film_load_pack`.

C API (dapple_c.h): `dapple_film_load_pack`, `dapple_film_load_pack_memory`, `dapple_film_set_pack_path`,
`dapple_film_list_json` (`{"loaded":bool,"stocks":[{"id","name","type":"negative|slide|paper"}]}`),
`dapple_film_attribution`, `dapple_film_swatches_json` (`{"<id>":["rrggbb" x3]}`: neutral, skin, sky).

`dapple film-upsm-gen [--verify core/data/film/dapple.dfp]` regenerates the spectral-upsampling table from first principles and
checks that a pack contains exactly that table (see `data-licensing.md`).

## Licence and attribution (required)

The profile data (and therefore `dapple.dfp`, a format conversion of it) is **CC BY-SA 4.0**,
(c) Andrea Volpato, <https://github.com/andreavolpato/spektrafilm>. Requirements for any build that ships it:

1. The app's **About screen must show** (the pack carries this line): "Film and paper profiles: spektrafilm by Andrea Volpato,
   github.com/andreavolpato/spektrafilm, licensed CC BY-SA 4.0 (converted to a binary format by Dapple)".
   `dapple_film_attribution()` returns this line (empty when no pack is loaded), so the UI does not hard-code it.
2. Ship the licence text (`core/data/film/SPEKTRAFILM_LICENSE.txt`) and `NOTICE.md` with the app, and link to
   <https://creativecommons.org/licenses/by-sa/4.0/>.
3. Keep the pack a separate asset file; changes to the pack contents must be shared under the same licence.
4. The author's non-binding requests: no paid LUT packs made of the data, no commercial AI training on it, no use
   of the project name in branding.

The code of the simulator is Dapple's own (clean-room, from the functional spec); only the profile numbers come from
spektrafilm. Open licence questions (does a separate asset keep the app outside the ShareAlike scope; provenance of
the KG3 / Kinoton spectra) are listed in `NOTICE.md`.
