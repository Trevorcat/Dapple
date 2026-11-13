# iOS device results (iPhone XR / A12 / iOS 18.7.10 and iPhone X / A11 / iOS 16.7.16, 2026-11-13)

Measured with the release build of the app made with `--dart-define=DAPPLE_IOS_AUTOTEST=true` (`app/lib/platform/ios_autotest.dart`: the engine driven through the FFI session API exactly like the editor, no UI input,
one JSON line per measurement in `Documents/autotest_log.txt`, pulled with `apps pull`; scripts `scripts/ios/device-run.sh`, `device-editor.sh`). Samples: CC0 files of raw.pixls.us from `build/rawsamples` (iPhone XS DNG 12 MP, Fujifilm X-S10 RAF 26 MP,
Sony A7 V ARW 33 MP) plus a synthetic JPEG. Raw logs: `build/ios/device/*_autotest_log.txt`; "XR-CPU" = the first run on the XR before the app asked for the GPU (`DAPPLE_GPU` was unset: CPU only), the others run on Metal.
Frames: "final 1600" = non-interactive frame at 1600 px long edge, "drag" = interactive frames at 1080 px, median of 6 - 12 changes of the parameter. All values ms.

## Which GPU path ran

Both phones: `api: metal`, device `Apple A12 GPU` / `Apple A11 GPU`, state `ready`, self-test `point+geometry: mean dE 0.040 p99 0.76 max 1.46 ok; spatial: mean dE 0.076 p99 0.86 max 1.45 ok; box scan vs direct: max diff 6.8e-06 ok`
(identical numbers to the M1 and the desktop), 317 frames on the GPU executor and 3 on the CPU ("no source store (host frame)": the host-frame probes), 0 device losses, 0 demotions. Device creation + self-test 4 ms (cached verdict);
the device text: `maxImage2D 16384, shared 32 KB, maxInvocations 1024, linear-filter RGBA32F 0, unified memory 1, heap 2048 MB (A12) / 1883 MB (A11)`.
The "film check" of the self-test (a film frame against the CPU, `gpu_context.cpp`) needs the film pack at probe time: the first measurement runs probed before Dart loaded the pack, so the verdict has no film line. From the build of 01:57
the Swift start-up reads the bundled pack before the probe (see "film check" below).

## Decode, memory

| file | decode ms (XR / X) | peak RSS MB (XR / X) |
|---|---|---|
| sample.jpg | 44 / 111 | 229 / 205 |
| IMG_1105.dng | 1259 / 1045 | 265 / 247 |
| xs10.RAF | 2684 / 2446 | 409 / 388 |
| a7m5.ARW | 2235 / 1978 | 564 / 619 |

Peak RSS is the process maximum while the file was open (RAW kept as a 2048 px session + a 1600 px frame pool). A 33 MP ARW stays below 620 MB on both phones (3 GB devices, jetsam limit about 1.4 GB for the foreground app), the 1.1 GB peak of the first
(CPU) run came from the CPU film path, not from the decoder. Full-resolution export (24 - 33 MP) was not run on the phones (`export` option exists in the script, not used: it needs more memory than a measurement run should risk without the owner).

## Interactive frames and final frames

| file | plain: first frame XR / X | plain: final 1600 XR / X / XR-CPU | plain: exposure drag 1080 XR / X / XR-CPU |
|---|---|---|---|
| sample.jpg | 261 / 265 | 26 / 22 / 23 | 10 / 14 / 15 |
| IMG_1105.dng | 290 / 306 | 13 / 20 / 52 | 16 / 22 / 21 |
| xs10.RAF | 120 / 150 | 16 / 16 / 62 | 15 / 21 / 21 |
| a7m5.ARW | 146 / 153 | 15 / 15 / 75 | 15 / 20 / 24 |

| file | stock | film: first frame XR / X | final 1600 XR / X / XR-CPU | strength drag 1080 XR / X / XR-CPU | exposure drag 1080 XR / X / XR-CPU |
|---|---|---|---|---|---|
| sample.jpg | kodak_portra_160 | 693 / 800 | 84 / 182 / 341 | 14 / 20 / 107 | 64 / 67 / 136 |
| sample.jpg | kodak_ektar_100 | 1050 / 1204 | 84 / 181 / 295 | 19 / 20 / 109 | 79 / 70 / 111 |
| IMG_1105.dng | kodak_portra_160 | 377 / 348 | 104 / 261 / 211 | 15 / 33 / 72 | 82 / 76 / 197 |
| IMG_1105.dng | kodak_ektar_100 | 227 / 385 | 108 / 271 / 213 | 14 / 35 / 74 | 60 / 74 / 232 |
| xs10.RAF | kodak_portra_160 | 227 / 319 | 98 / 234 / 208 | 21 / 30 / 67 | 48 / 73 / 167 |
| xs10.RAF | kodak_ektar_100 | 236 / 360 | 104 / 229 / 197 | 28 / 30 / 68 | 69 / 70 / 190 |
| a7m5.ARW | kodak_portra_160 | 218 / 314 | 100 / 233 / 227 | 20 / 33 / 71 | 62 / 68 / 294 |
| a7m5.ARW | kodak_ektar_100 | 299 / 395 | 102 / 235 / 213 | 13 / 27 / 71 | 73 / 69 / 236 |

## A11 versus A12

* Same code, same shaders, same numbers in the self-test; no feature fallback was needed on the A11: 16384 px textures, 32 KB threadgroup memory, 1024 invocations, read-write RGBA16F storage, `dispatchThreads` (non-uniform threadgroups, A11 = Apple GPU family 4)
  are all there, which is what the film path needs. Neither GPU filters 32-bit float textures (`linear-filter RGBA32F 0`; the M1 does): the film kernels use `texelFetch` and hand interpolation by design, so nothing changes.
* Plain pipeline: A11 and A12 are equal (final 1600 px 15 - 20 ms, drag 14 - 22 ms) and 2 - 5x faster than the A12's own CPU for final frames; exposure drags without film are CPU-bound (the 41^3 lattice bake) and are not faster on the GPU.
* Film: the A12 GPU renders the 1600 px final film frame in about 100 ms, the A11 in about 235 ms (2.3x; the A12 GPU has 4 cores and roughly twice the ALU / bandwidth per core); both beat the A12 CPU (200 - 340 ms). Strength drag (interactive, the case that matters while sliding):
  A12 13 - 28 ms, A11 20 - 35 ms, against 67 - 109 ms on the CPU: both are inside a 60 fps budget for the GPU part, the A11 only just. Exposure drag with a film (film tables are re-baked on the CPU) 48 - 82 ms on both; 3 - 4x faster than the CPU path.
* First frame costs: 120 - 300 ms for the plain pipeline and 220 - 1200 ms for the first film frame (pipeline state creation, film tables). The pipeline archive is not used yet on the phones (see metal-backend.md).
* A11 caveats not measured: sustained thermal throttling (runs were 25 - 35 s long), battery.

## Zero-copy display on the phones

`IosDebugOpen` probe (`probeView` in the marker file, `Documents/probe_log.txt`): `GpuViewBackend.start` created the texture, the engine view bound to the Swift frame sink, 6 frames submitted:
XR `presenter {backend: metal, adopted: true, presented: 6}`, `externalFrames 5, hostFrames 1, renderFailed 0, presentFailed 0`, present 0.02 ms; X the same with present 0.06 ms. (The first frame has no size estimate and takes the host path.)
Screenshots: `build/ios/device_x_editor_film_portra160.png` (iPhone X: editor, XS DNG, Kodak Portra 160 N160, new pad panel; the system photo-permission prompt is still on screen because nobody can answer it from here). The matching XR screenshot was
deleted: the XR permission prompt shows thumbnails of the owner's real photo library, and screenshots of the phones can contain private content.

## Film check lines

The self-test of a fresh probe (verdict cache removed, `Library/Caches/dapple-gpu`) on the phones: A12 device creation + self-test 61.5 ms, A11 78.2 ms (cached verdicts: 4 / 3 ms), verdict
`point+geometry: mean dE 0.040 p99 0.76 max 1.46 ok; spatial: mean dE 0.076 p99 0.86 max 1.45 ok; box scan vs direct: max diff 6.8e-06 ok`. The **"film check: mean dE / p99" line did not appear** in any run on either phone, with or without the film
pack loaded before the probe (the Swift start-up now loads the bundled pack first): in `gpu_context.cpp` the film comparison sits behind the `wrong` pointer of the frame test, which only the GLES verdict path supplies, so it is not part of a Metal verdict.
If the film frame is meant to be part of every backend's verdict, `wrong` has to be passed on the Metal path too (executor engineer).

## Round 3 (2026-11-13): the tree of HEAD 28486eb on the M1 and on both phones

The macOS tree built from the first sync with **no source change** (the `libraw_mosaic.inc` / MSL suspects did not break: all `demosaic_*.comp` compile through `dapple_shaderc --msl`; the iOS device/simulator xcframework and the release ipa built too).
Mac M1, Metal, 0 failures: `demosaic` flat edges zoneplate hotpixel hotfilter clip tiles threads cache wire_region wire_anchor wire_cache wire_cancel gpu_parity (bayer RGBA16F mean dE2000 0.012, X-Trans 0.016) gpu_tiles gpu_real wire_gpu (mean dE2000 0.015 / 0.020); `dapple_film_gpu_tests` frames coverage film-tiling; `dapple_film_disk_tests` (tables, all); `dapple_golden selfcheck` 0 differences (also with the film pack);
`dapple_film_tests develop_film` passes (59 checks, 0 failures; `grain knob: std dev 0 -> 0.00013, 50 -> 0.00078, 100 -> 0.00154, 150 -> 0.00231, 200 -> 0.00308`: linear, no saturation, the earlier failure is gone).
Demosaic of the 40.1 MP X-T50 RAF (X-Trans, tile 512, 176 tiles): Metal compute-only 1585 ms (device 1550), with read-back 1786 ms; RGBA16F 1580 / 2649 ms; CPU node 1800 ms (8 threads); ROI 1920x1080 at 1:1 on Metal 132 ms. 26 MP X-S10: 1033 / 1171 ms vs CPU 1150 ms. On the M1 the GPU X-Trans path is not faster than the 8-thread CPU one (xfast tier).
Warm-up on the M1 (fresh): `gpu warm-up: film pipelines + default stock 216 ms (film check: mean dE 0.063 p99 0.82 max 1.67; ... film_v3 direct 17.1 ms vs staged 13.8 ms -> staged, interactive 6.7 ms/Mpx)`.
Phones (ipa of this tree, autotest build): installed fresh on the XR (`com.lumi.dapple.00008020`) and X (`com.lumi.dapple.26d2a524`); both ran `dapple` autotest to `done`, Metal, 317 GPU frames, 0 fallbacks / device losses; film ms/Mpx 19.6 (XR) / 28.2 (X); sample.jpg film frame final 76 ms (XR) / 139 ms (X), strength drag median 11 - 13 ms / 18 - 20 ms.
XR warm-up (cached verdict on second launch): `film pipelines + default stock 377 ms (film check: mean dE 0.063 ... ; film_v3 staged (cached verdict), interactive 19.6 ms/Mpx)`. The fresh-verdict line on the phones was not captured (engine log needs `dvt launch --stream`). No editor screenshot: the fresh install shows the Photos permission prompt (with the real library), screenshot deleted.

## Round 4 (2026-11-13, HEAD 82b6bd4 "Single-shot HDR")

Mac tree made equal to HEAD with `scripts/ios/sync-worktree.sh` (clean tree; all 83 `app/lib` files compared by md5 against the PC: identical, so the four hand-copied files are overwritten; `sync.sh`/overlay.txt left unchanged, `sync-worktree.sh` is the proper path). macOS engine + tests built with no source change.
M1 / Metal, all pass: demosaic flat edges zoneplate hotpixel hotfilter clip tiles threads cache wire_region wire_anchor wire_cache wire_cancel gpu_parity (RGBA16F bayer 0.012 / X-Trans 0.016 dE2000) gpu_tiles gpu_real wire_gpu; `dapple_hdr_tests` all ids incl. parity and bench (GPU 1080 px interactive: hdr 0 1.30 ms, hdr 100 2.24 ms; 6000x4000: 61.6 vs 97.3 ms); film_gpu_tests frames; golden selfcheck 0 differences.
40.1 MP X-T50 RAF (tile 512): Metal compute-only 583 ms (was 1585), with read-back 667 ms (RGBA16F 576 / 640), CPU node 1847 ms (8 threads), 1920x1080 region 42.1 ms with read-back (was 132). The "verdict frame ..." line was not seen on the Mac (the CLI / tests set the GPU node explicitly or do not reach the verdict path).
Warm-up (M1, fresh): film_v3 direct 17.1 ms vs staged 13.8 ms -> staged, interactive 6.8 ms/Mpx; film check mean dE 0.063 / 0.094 / 0.062.
Phones: ipa built and signed for the XR (13:08), but `apps install` OVER the existing app hung for more than 6 minutes (the same installd hang as in the README); stopped as instructed, nothing uninstalled, both phones still have the old build (1.0.0 (1)) installed. Not installed on the X, no autotest, no new expiry dates, no thumbnail analysis.
