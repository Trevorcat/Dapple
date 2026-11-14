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

Round 4, phones (second attempt, own bundle ids uninstalled then installed, as the README): both installed with the HEAD 82b6bd4 autotest ipa; new profile expiry 2026-11-20 05:20 UTC (XR) and 05:16 UTC (X) (embedded.mobileprovision).
Autotest (sample.jpg, public X-S10 and X-T50 RAF from Documents/Photos, kodak_portra_160), XR / X: 149 GPU frames + 3 CPU host frames on both, 0 fallbacks, 0 device losses, filmMsPerMpx 19.8 / 28.7.
Session open (decode) ms: sample.jpg 65 / 79, X-S10 26 MP RAF 2229 / 2105, X-T50 40 MP RAF 2241 / 2541 (the X is not slower on the RAF: mosaic decode dominates). Film frame final median at the RAF size: 91 / 167 ms, strength drag median 18 - 21 / 28 ms.
Warm-up lines were captured only on a second launch (cached verdict): `film_v3 staged (cached verdict), interactive 19.6 ms/Mpx` style, film check mean dE 0.063 / 0.093 / 0.062, 416 ms (XR), 487 ms (X); the fresh direct-vs-staged line of the first launch was not streamed. The `dapple_demosaic` verdict line cannot be seen on iOS: `demosaic_gpu.cpp` logLine writes with fprintf(stderr) (and logcat on Android) but not NSLog, so `dvt launch --stream` shows nothing; needs an NSLog/os_log branch.
Home-screen album thumbnails (old build, streamed before the uninstall, both phones): `dapple photos: queryNew authorization 3` then `status 3, 0 assets match (since <now-7d>)`: PhotoKit has full access (3 = authorized) and the first-run listing returns 0 assets (it only lists photos created in the last 7 days). So no photo was found or copied and nothing failed in the listing; the four grey tiles are not from this scan. Photos brought in by the album picker "+" leave no log line in the release build (no HEIC / copy failure line exists), so their cause is undetermined from logs.

## Round 5 (2026-11-13, HEAD 67ae519 colour-managed import)

Mac M1 / Metal, all pass: dapple_wide_tests (62 checks), dapple_uhdr_tests, dapple_hdr_tests, demosaic (flat ... cache, wire_*, gpu_parity, gpu_tiles, gpu_real, wire_gpu), dapple_golden selfcheck 0 differences. xcframework and ipa built, no source change.
Install: over-install hung again (installd), so Dapple's own two bundle ids were uninstalled then installed; profile expiry 2026-11-20 07:00 UTC (XR) / 07:03 UTC (X).
Autotest (sample.jpg, X-S10 + X-T50 RAF, synthetic P3 HEIC from a sips conversion of the repo sample.jpg) XR / X: all four opens ok; open ms RAF 40 MP 2239 / 2527, 26 MP 2292 / 2102, p3.heic 51 / 61; 197 GPU + 3 CPU frames, 0 fallbacks.
Log line on both real iPhones: `dapple: source colour space Display P3 (cICP, sRGB curve)`.
Not captured: the `dapple_demosaic` verdict (os_log lines do not appear in `dvt launch --stream`, `syslog live` or a collected logarchive on the phones; and no `demosaic_verdict_*.txt` exists in Library/Caches/dapple-gpu on either phone, only `film_v3_verdict` `0 19.88` (XR, staged) / `0 29.03` (X, staged) - the autotest never reached the GPU demosaic measurement), the fresh film_v3 first-launch line (launched before the stream), `dapple ingest:` lines (the autotest does not ingest).

## Round 6 (2026-11-13): threads for the RAW open on Apple

Profile (M1, `dapple_stage_bench`, os_log stage lines read with `log stream`; before = single-threaded LibRaw, no libomp): X-T50 RAF 40 MP preview open 1381 ms = mosaic 1326 (unpack 1276 single core: Fuji compressed strips, + scale_colors 124) + block reduction 33 + anchor 14 + the rest; detail 1617 (the 4096 session re-reads the mosaic: 1609); first 1:1 region 594 (GPU demosaic region, 9 ms GPU vs 32 CPU in the verdict); export open 682 + render 829 + encode 166. Sony A7R IV ARW: open 351 = unpack 182 + block reduction 74 + anchor 30; later opens 106 (cached mosaic). Single-threaded on Apple before this round: LibRaw unpack of Fuji strips (`fuji_decode_loop` is `omp parallel for`; 4 usable P cores on the M1, 2 on A11 / A12), Nikon HE, CRX, Panasonic pana8 and LibRaw's own demosaic / raw2image loops (all `#pragma omp` inside LibRaw; there is none in `core/`, our loops use the ThreadPool). Sony ARW unpack (182 ms on M1) has no OpenMP path in LibRaw: stays on one core. The engine ThreadPool used all cores incl. efficiency cores at default QoS and no QoS call existed anywhere in core.
Chosen: (a) static libomp (LLVM 19.1.7, same major as the NDK's), not (b): the build needed one script and 20 lines of CMake with no patch to the LibRaw snapshot, covers every OpenMP loop in LibRaw (Fuji, Nikon HE, CRX, demosaics) instead of one, and the same hunk builds all three Apple slices; (b) would be a local LibRaw patch to re-apply on every snapshot. Size: `libomp.a` 0.86 MB per slice; ipa 11,117,008 -> 11,330,671 bytes (+0.21 MB compressed, includes other HEAD changes), dapple.framework (device) 6.24 MB. Licence note added to docs/legal/libomp.md.
Threads / QoS (core/src/sysperf.h, omp_apple.cpp): burst = performance cores via `hw.perflevel0.logicalcpu` (2 on A11 / A12, 4 on M1; fallback 2 for 6-core phones, half otherwise) for the ThreadPool and the OpenMP team (`OMP_NUM_THREADS` still overrides); pool workers and the render thread run at user-initiated QoS, `setBackground(true)` (prefetch, baking) at utility; `dapple_session_open` / `_open_region` raise the calling thread to user-initiated for the call (before: the calling Dart worker thread was at default QoS, never background), and the OpenMP team is created once under that QoS (workers inherit the creator's QoS, so a team first started from a utility prefetch would stay utility).
Before / after (open ms, preview 2048; before = same build with OMP_NUM_THREADS=1 on the phones):
M1: RAF 40 MP 1381 -> 583 (unpack 1276 -> 462), preview mosaic 1326 -> 518; 4096 session 1617 -> 694; ARW unchanged (351 -> 388, unpack 182 -> 185). M1 CPU export of the 61 MP ARW got slower (render + encode 3.2 -> 4.3 s; pool 8 -> 4 threads, the efficiency cores no longer help; not tuned).
XR (autotest openMs): X-T50 RAF 2207 -> 1772, X-S10 RAF 1619 -> 1237, A7 V ARW 1080 -> 1214 (noise, unchanged code path); round 5 build: 2239 / 2292.
X: X-T50 RAF 2387 -> 1596, X-S10 1731 -> 1264, ARW 1176 -> 1232; round 5: 2527 / 2102.
Correctness: golden selfcheck 0 differences, all demosaic.* pass (incl. gpu_parity, wire_*), dng tests ok; `dapple develop` of the RAF and the ARW is byte-identical with OMP_NUM_THREADS=1 and 4 (md5 equal).
Not done: the phones' stage lines (os_log of Runner is not exposed by dvt stream / syslog), so the phone profile is only open totals; ARW unpack (Sony decoders) and the remaining 2-core limit of A11 / A12 (RAF still 1.6-1.8 s: unpack ~2x parallel + the serial anchor / block reduction / mosaic copy); `open_timing.txt` is written by the DAPPLE_OPEN editor path, not the autotest, and was not exercised; the pool-size change was not tuned for CPU-only exports. Profiles renewed: expiry 2026-11-20 (XR 10:11 UTC, X 10:12 UTC).

## Camera capture (2026-11-13, build with `DappleCapturePlugin`)

| phone | lens | RAW kind | max pixels | modes | shot (RAW + JPEG) | engine |
|---|---|---|---|---|---|---|
| XR (iPhone11,8, iOS 18.7.10) | back wide 1x | Bayer (`rgg4`), no ProRAW | 4032x3024 | raw+jpeg, raw, jpeg | DNG 9.6 MB + JPEG in 0.4 s | developed 3024x4032 in 1.9 s (RAW only: 0.9 s) |
| XR | front | none | 3088x2316 | jpeg | - | - |
| X (iPhone10,3, iOS 16.7.16) | back wide 1x | Bayer, no ProRAW | 4032x3024 | raw+jpeg, raw, jpeg | DNG 9.7 MB + JPEG in 0.5 s | developed in 3.3 - 3.8 s |
| X | back tele 2x | Bayer | 4032x3024 | raw+jpeg, raw, jpeg | DNG 10.0 MB + JPEG, 0.6 s | developed in 3.3 s |
| X | front | none | 3088x2320 | jpeg | - | - |

Settle 0.70 - 0.76 s after session start. Apple's DNG carries a 320x240 embedded preview when asked (`rawEmbeddedThumbnailPhotoFormat`), so a RAW-only shot has an exposure-anchor reference without the viewfinder JPEG (the tier itself is not exposed by the C API; not read). Orientation: the DNG decodes as 3024x4032 (portrait) with the phone upright. The preview content could not be judged for uprightness (phones lie face up on a desk); layout and rotation are set by the portrait preview connection.

## Round 3: HEAD d28acf3 on both iPhones (2026-11-13 / 14, built from a clean `git worktree`, no working-tree files)

Mac (M1, tree = HEAD + `scripts/ios/build-xcframework.sh`): `dapple.xcframework` now built with both slices (device + simulator; the script fails when one is missing), `flutter build ios --simulator` and the device ipa build. ctest on macOS: demosaic.* (incl. half_zone / half_tiles / half_flatclip / gpu_*, half_gpu on Metal), p3.*, base_previews.*, engine.modern_*, export_meta.*, engine.selfcheck: 64 of 64 pass, 14 skipped (need the real sample files; golden skipped by design).
Profiles: both signed 2026-11-13 16:01 UTC, **expire 2026-11-20 16:01 UTC** (XR `com.lumi.dapple.00008020`, X `com.lumi.dapple.26d2a524`).

| check | XR (iOS 18.7.10) | X (iOS 16.7.16) |
|---|---|---|
| a fresh install, "允许访问" pill -> Photos prompt -> Allow Full Access ("允许访问所有照片" on iOS 16) -> card gone, scan starts | pass: `scan: 2 new` | pass: `scan: 0 new` ("没有新照片", the library has nothing from the last 7 days) |
| b library photos develop | pass: 2 of 2 developed (`IMG_0001.PNG`, `IMG_0002.PNG` 750x1334, HEIC via the 16-bit PNG path, 94 - 213 ms); Documents samples developed: JPEG, HEIC (p3.heic 1800x1200), DNG, ARW, RAF x2 (no failures). No colour-space log line exists in HEAD | nothing to scan; samples all developed (a7m5.ARW 3.0 s, RAF 1.0 - 1.4 s, p3.heic 0.75 s) |
| c "+" -> 从相册选择 -> first item -> editor on a developed picture; same item again | pass (DAP DNG developed 3024x4032, 1.3 s); second pick logs `picked ...: already in library`, toast text not caught in the element tree | pass (`IMG_0059.png` 2436x1125 developed 0.9 s); second pick: toast "这张已经在照片里" seen 1.3 s after tapping 添加 |
| d film map: swatches, press-slide-release, modern | swatches complete at the first look (0.6 s, warm); press-slide-release selected stocks (C50D, N400A/C, N200B) and the map closed each time (Switch 1 -> 0); modern 鲜明 and 柔和 chosen by pressing on the circle | cold start of the app: most swatches by 2.2 s, last one at about 14 s; press-slide-release selects and closes (N200B, N400C); modern not re-tried (the 2.4x magnification moves the targets) |
| e export of a public sample | pass: IMG_1105.dng 1440x1080 JPEG, EXIF Make Apple / Model iPhone XS / exposure / ISO / lens / DateTimeOriginal / Software "Dapple 0.1.0", ICC "sRGB" (464 bytes), no GPS | pass: sample.jpg 1440x960, Software "Dapple 0.1.0", ColorSpace 1, ICC sRGB (the source JPEG carries no camera EXIF). The DNG export crashed the app (below) |
| f camera RAW+JPEG, `save:false` | pass (DNG 9.56 MB + JPEG, 0.6 s, editor opened) | pass (developed 3024x4032 in 3.5 s) |

**Crash found (both phones, reproducible):** `EXC_BAD_ACCESS` in `__kmp_acquire_ticket_lock` <- `__kmpc_critical_with_hint` <- `libraw_memmgr::mem_ptr` <- `LibRaw::calloc` <- `parse_tiff_ifd` <- `LibRaw::open_datastream` <- `dapple::extractPreviewImpl` on a `std::async` worker of `decodePreviewFromMosaic`. A LibRaw `omp critical` entered from a thread that libomp does not know (the static libomp of commit eec7b7e) while another LibRaw is decoding. XR at 23:45:59 (leaving the editor of IMG_1105.dng), X at 23:58:06 (DNG export). Not fixed (core/).
Evidence: `build/ios/capture/` (xr_filmmap.png, xr_tab_imaging.png of a public sample, exported JPEGs under exp_xr / exp_x, capability JSON of the first round). The two test files `DAP_20261113_224008.jpg/.dng` in the XR's Photos album "Dapple" cannot be deleted from the app (no delete): the owner has to remove them. Both apps were reinstalled at the end (clean storage, permissions reset). The wireless-data prompt appeared only in round 2 (answered "仅限无线局域网"); it did not appear this round.

## Round 7 (2026-11-14): crash / livelock around LibRaw's OpenMP critical sections

Evidence: every `#pragma omp critical` in LibRaw (`libraw_alloc.h` mem_ptr / forget_ptr = every allocation, `raw2image.cpp critical(dataupdate)` once per row, `fuji_fill_buffer`, `crx.cpp`) is a process-global lock, one per name, shared by all LibRaw instances and threads, and libomp turns it into a spin lock (queuing lock on Apple arm64). The allocation lock is taken from threads that are not OpenMP team members (the embedded-preview read on a `std::async` thread, any other session's opening thread): libomp registers each of them as a root. The device crash (`__kmp_acquire_ticket_lock` from `LibRaw::calloc` on the std::async thread) is that lock.
Reproduction on the M1 (new `dapple_omp_stress_tests`: N threads open / close preview, region, full sessions of the public DNG / RAF / ARW through the C API in a loop): with the unpatched LibRaw the run hangs at about 700 % CPU in `__kmp_acquire_queuing_lock` (samples: all team threads in `__kmpc_critical_with_hint`, from `xtrans_decode_block` / `fuji_fill_buffer` and from `copy_bayer` `critical(dataupdate)`; waiters spin and starve the holder, 64 threads on 8 cores); one of three 60 s runs, about one of six 12 s runs; baseline `DAPPLE_LIBRAW_LOCKPATCH=OFF` hung again in run 3/3. A crash like the phones' was not obtained on the Mac or on the XR (4 and 8 workers, three runs: no crash; the 8-worker runs were jetsam-killed once or twice by memory, not by this lock), so the iOS crash is attributed from its stack (calloc -> mem_ptr -> critical on a foreign thread) plus the livelock reproduction, not reproduced.
Fix: the patch above (all critical sections -> scoped std::mutex, no libomp involvement outside real parallel regions; the preview read / mem_ptr no longer touch the OpenMP runtime) and, in `omp_apple.cpp`, `kmp_set_blocktime(0)` (idle workers sleep). Android: same LibRaw and the NDK's libomp, so the same lock semantics apply there (not reproduced; no unpatched Android build was run); the patch is portable and the patched arm64 build (NDK 28, static libomp) passes the stress test on the Redmi.
After the fix: M1 3 x 90 s with 8 threads: 1311 / 1250 / 1311 opens, no hang; 24 consecutive 12 s runs without a hang; Redmi (6 threads, `/data/local/tmp/dapple`, removed afterwards) 4 x 60 s + 1 x 25 s: 406 / 361 / 369 / 369 opens, 0 failures; XR 3 x 90 s (4 workers through the autotest `stress` hook): 530 / 573 / 525 opens, 0 failed, maxRss 0.8 - 0.9 GB; X 3 x 90 s: 651 / 605 / 484 opens, 0 failed. Open ms (autotest, XR / X): X-T50 RAF 1559 / 991, X-S10 RAF 838 / 826, DNG 639 / 676, ARW 1202 / 1216 (round 6: 1772 / 1596, 1237 / 1264). M1 stage bench RAF preview open 529 ms (round 6: 583). golden selfcheck 0 differences, demosaic.* all pass. ipa 11,573,831 bytes (+322 bytes over the unpatched one).
Files: cmake/Dependencies.cmake, core/src/omp_apple.cpp, scripts/ios/build-engine.sh (DAPPLE_CMAKE_EXTRA), app/lib/platform/ios_autotest.dart (`stress` config), tests/engine/omp_stress_tests.cpp (+ build/omp_stress_cmake.patch for tests/CMakeLists.txt, left to the owner of that file), docs/legal/libomp.md.

## Round 4: HEAD 339ce4d on both iPhones (2026-11-14, clean `git worktree`, junctions removed with `rmdir` and verified before `git worktree remove`)

Mac (M1, tree = HEAD): xcframework device + simulator slices, device ipa and simulator app build. `dapple_omp_stress_tests 60 6` x 3 (IMG_1105.dng, X-S10 RAF, A7 ARW): 1611 / 1574 / 1561 opens, 0 failed, no crash. ctest demosaic.*, engine.selfcheck, video_session.*, motion.*: 40 of 40 pass, 14 skipped (golden and the real-sample tests need the files). Profiles: XR and X both **expire 2026-11-20 19:51 UTC**.

| check | XR (iOS 18.7.10) | X (iOS 16.7.16) |
|---|---|---|
| a Photos prompt -> Allow Full Access -> scan | pass: `scan: 2 new`, both developed (94 - 222 ms) | pass: `scan: 0 new` (nothing from the last 7 days) |
| b crash check: open + leave 5x for IMG_1105.dng and xs10.RAF; export through 发布 -> 分享… -> 系统分享… 5x each | pass: 10 opens, 10 exports, no Runner crash report | pass: 10 opens, 10 exports, no Runner crash report |
| c film map: icons, press-slide-release, modern | pass: film canister icons and round modern icons, no swatch rendering; the map is complete at the first screenshot (0.5 - 0.6 s after the tap on the stock button, about 2 s after tapping 胶片); press-slide-release selected C50D and closed; 鲜明 and 柔和 chosen | pass: complete at 0.5 s; selected N200B; 通透 and 鲜明 chosen; map closed every time |
| d colour pad look swatches | pass: round look swatches (the sample picture) on the pad | pass |
| e first-ever film frames on a fresh install (autotest, interactive 1080 px): portra 160 / ektar 100 / c200 | IMG_1105.dng 316 / 277 / 379 ms (settled median 119 - 143 ms); xs10.RAF 352 / 168 / 184 ms (109 - 136 ms); open 1.1 s and 4.7 s | IMG_1105.dng 391 / 286 / 297 ms (settled 165 ms); xs10.RAF 314 / 202 / 200 ms (148 - 151 ms) |
| f camera RAW+JPEG, `save:false` | pass (DNG 9.5 MB + JPEG, editor opened) | pass (DNG 9.7 MB + JPEG, editor opened) |

Not passing: the autotest's FULL-resolution export of xs10.RAF (6246x4170) never finished on either phone (no `export` line, no `done`); on the XR a JetsamEvent at the time lists Runner at about 0.76 GB resident (lifetime max about 1.0 GB): the 3 GB / 2 GB phones kill the app during a full-size RAF export. The full-size DNG export works (XR 8.7 s cold, X 3.2 s). The share-sheet exports above are the reduced publish sizes.
Evidence under `build/ios/pkg/` (film map, colour pad and 成像 tab of the XR; the film map and colour pad show a public sample, the 成像 tab shows the bundled style picture). Both apps were reinstalled at the end (clean storage). The 2 files `DAP_20261113_224008.jpg/.dng` in the XR's Photos album "Dapple" are still there (no delete in the app). One signing attempt failed on a network error of the signing tool (anisette server); retried successfully.

## Round 5 (2026-11-14): HEAD 1b528da on both iPhones (UI round: capsule tab bar, single pad, publish page)

Mac (M1, tree = PC working tree synced with `sync-worktree.sh`; the tracked files equal HEAD, plus the untracked `scripts/film/lut_eval*` which the build does not use): `build-engine.sh macos on` builds (386 steps). `ctest` (-j3): 223 of 226 run tests pass, 3 fail, 14+ skipped (real-sample files):
`core_tests` (`tests/core_tests.cpp:1440 / 1444 / 1447`, "scheduling policy": `poolCpus(sd680) == {4,5,6,7}`, `poolCpus(g85).size() == 8`, `!elite.hetero() && poolCpus(elite).size() == 8` - fails alone as well; the Android topology table against `sysperf` on Apple, not an iOS-app problem),
`film.c_api` (SEGFAULT in the parallel run, passes alone) and `ai_masks.export` (`ai_masks_tests.cpp:580 s != nullptr` in the parallel run, passes alone): the last two are flaky under `-j3` (shared temp / cache path), not under `-j1`.
Phones: no source change was needed, no build-fix patch (no `build/ios-r5/*.patch`). ipa 12,229,196 bytes (unsigned, autotest flag, `build/ios-r5/Dapple-r5-unsigned.ipa`). The over-install hung on both phones (240 s / 100 s, installd), so Dapple's two own bundle ids were uninstalled and installed (nothing else touched).
New profiles: **XR expires 2026-11-21 05:36 UTC, X 2026-11-21 05:41 UTC** (signed 2026-11-14 05:36 / 05:41 UTC).
Test content: public samples only (IMG_1105.dng, X-S10 / X-T50 RAF, A7R IV ARW, sample.jpg, the repo's synthetic `p3.heic`, 7 CC0 portraits of `ai-test-images.md`), brought in with `apps push` + `Documents/DAPPLE_OPEN` (the autotest build's document ingest); the phones' libraries and other apps were never opened.
Evidence: `build/ios-r5/x_*.png` (iPhone X, 375x812 pt) and `xr_*.png` (XR, 414x896 pt); `x_autotest_log.txt`; scripts `w2.py`, `zoomtime.py`, `pinchtime.py`, `push.sh`, `sign_install.sh`.

| check | iPhone X (iOS 16.7.16) | iPhone XR (iOS 18.7.10) |
|---|---|---|
| home, floating capsule tab bar, tabs switch (照片 / 成像 / 设置) | pass: `x_01_home`, `x_04_tab_imaging`, `x_05_tab_settings` | pass: `xr_01_home`, `xr_03_tab_imaging`, `xr_04_tab_settings` |
| last grid row reachable | pass: 13 / 11 photos, last row above the capsule after scrolling (`x_03_home_scrolled`) | pass (`xr_02_home_scrolled`) |
| selection capsule (存相册 / 发布) | pass (`x_06_selection`) | pass (`xr_05_selection`) |
| "+" | on iOS it opens the system picker directly (`importMenu`: no files source, no merge / panorama / video tools on iOS, so no sheet and nothing listed that does not work). The camera and picker icons are the only entries | same |
| editor, panel heights follow content | 成像: header + chip + pad (`x_08`), 局部 list view is the tallest (336 pt; a gap between the "+" row and the tool row, `x_12_local`), pad panels 317 pt | same layout (`xr_06`, `xr_08`) |
| 成像 header readout | complete: "成像  [选成像 v]  强度 100%", after a stock "N400C v" / "通透 v" with a reset button (`x_08`, `x_09`) | same |
| press-slide-release 选成像 -> film map | pass: modern look 通透 chosen and the map closed (`x_09`); map with film canisters + 2 round modern looks (`x_10`); a canister tap chose N400C (`x_11`) | pass (`xr_07_filmmap`: 通透, N400C) |
| 局部: single pad + 光线 / 颜色 / 质地 switch | pass, gradient (`x_13`, `x_14`, `x_15`: the pad axes change with the switch) and circle (`x_16`) | pass (`xr_09`, `xr_10`) |
| tool strip directly under the photo (brush) | pass: 加上 / 去掉 + 4 sizes right under the photo (`x_17`) | pass (`xr_11`) |
| hold to compare | pass: "原片" chip, unedited look (`x_18`), edit back on release (`x_19`) | pass (`xr_12`) |
| AI masks / portrait retouch | hidden, no dead buttons: 局部 offers only 渐变 / 圆形 / 画笔 / 明暗 / 颜色 on a portrait; no 主体 / 天空 / 人像 / 美肤 anywhere in the 光线 / 颜色 / 质地 detail views or the 更多 menu | same (checked on `p6_asian_woman_grey`) |
| publish page labels | 广色域（P3）: pass, offered for `p3.heic` (`x_26_publish_p3`); HDR 输出: pass, offered for the A7R IV ARW after a brightening edit (`x_29_publish_p3_hdr`; not offered for the unedited public samples, there is no highlight headroom, nor with a film look). Size sheet 原图大小 / 大 / 适合社交平台 (`x_24_publish_size`) | both labels pass (`xr_13_publish`, `xr_13_publish_hdr`) |
| save to the app's album "Dapple" | pass: selection 存相册 on the A7R IV file gave the toast "已存入相册「Dapple」· 1 张" (add-only prompt answered 允许 through WDA). The file itself in the Photos album could not be looked at (the phone's library stays closed), only the app's toast | **fail, not product**: `PHPhotosErrorDomain 3311` (access denied): the add-only prompt was answered but the follow-up "额外访问" prompt hung WDA, the app was killed during the prompt, and Photos then kept add access denied; both 存相册 and the editor button give "保存失败" / "0 张". Needs a fresh install (permissions reset) to retest |

Timings (iPhone X, autotest build, same harness as round 4, kodak_portra_160, 12 drags): sample.jpg open 109 ms, first film frame 396 ms, final 139 ms, strength drag 35 ms, exposure drag 44 ms; IMG_1105.dng open 590 ms, first 297, final 175, strength drag 35, exposure drag 46; xs10.RAF open 721, first 243, final 165, strength 29, exposure 44; maxRss 304 MB. Round 4 on the X: strength 20 - 33 / exposure 67 - 76 (exposure -35 %), RAF open 2105 (now 721, libomp + threads). UI first 1:1 view (pinch out on IMG_1105 with film, WDA screenshots polled, +-0.35 s): gesture over at 0.96 s, layout settled 1.4 s, grain-sharp 1:1 tiles at 3.0 s (about 2.1 s after the fingers lift). Film drag by hand was not timed (WDA latency); the autotest strength drag above is the regression number.

Findings (not fixed, product code untouched):
* The 返回 button of the editor often needs several taps: `PopScope` first closes transient states (menu, panel, enlarged photo, brush tool), a plain tap after a 1:1 zoom or a pad edit did nothing for about 15 s once; the edge swipe also did not always leave. Worth a look (two taps seen as "dead back button").
* A double tap in the editor enlarges the photo and collapses the panel (not 1:1); pinch gives 1:1.
* 局部 list view: a large empty block between the area row and the tool tiles.
* `存相册` in the selection bar saves at the export-size setting, not at the publish preset size; the "publish size" JPEG was therefore not produced through 分享…. (系统分享… opens the system share sheet, not driven here.) The "存入相册，再打开微信" row of the share sheet was not used (it would leave Dapple).
* WDA hangs (HTTP calls never return) while a Photos permission prompt of the app is on screen or after answering a limited / full access prompt; recovery: `ios_setup --stop-wda`, `ios_setup`, relaunch the app. The first "额外访问" (read) prompt after an add-only grant is not harmless on the XR (see table).
* Two screenshots of the system photo picker (with the owner's library) were taken by mistake while navigating (the "导入照片…" button is the "+" on iOS) and were deleted at once; the picker was cancelled each time.
Cleanup: Documents/Photos, `DAPPLE_OPEN`, `autotest*`, `previews/`, `library2.json` of both apps removed; no exports other than one JPEG in the X's album "Dapple" (1 file, the app has no delete; the owner may remove it; the two `DAP_20261113_224008.*` files on the XR were left); WDA stopped on both phones, USB forwards killed, nothing running on the Mac. No settings changed, no reboot.

## Round 6 (2026-11-14): working tree with AI masks, portrait retouch, OpenCV tools, Live Photo plumbing (uncommitted), both iPhones

Build: tree synced to the Mac (`sync-worktree.sh`), `build-engine.sh ios-device on` + xcframework + `flutter build ios --release --no-codesign --dart-define=DAPPLE_IOS_AUTOTEST=true`, ipa 111.7 MB (`build/ios-r6/Dapple-r6-unsigned.ipa`; the engine framework is 56 MB with OpenCV, 7 AI models bundled, no depth model). Signed with plumesign, Dapple's own two bundle ids uninstalled (XR first) and installed; both profiles signed 2026-11-14 ~08:45 UTC, expiry about 2026-11-21 (not read from the mobileprovision). Driven through WDA, public CC0 test files only (p2, p3, p6, p8 portraits, `lake.jpg`), brought in with `apps push` + `Documents/DAPPLE_OPEN`. Evidence `build/ios-r6/` (`xr_*`, `x_*` png, `x_autotest_log.txt`, scripts `seq.py`, `timemask.py`, `ai_time.py`, `sky_time.py`, `pinch.py`).

| check | iPhone XR (iOS 18.7.10) | iPhone X (iOS 16.7.16) |
|---|---|---|
| 局部 on iOS now offers 美颜 / 人物 / 背景 / 点选 / 天空（粗略） | pass (`xr_01`); no 远近 (depth not bundled) | pass (`x_panel_scrolled`) |
| 人物 on p6 | pass: chips 整个人 / 脸部皮肤 / 身体皮肤 / 头发 / 衣服 + 脸 row (眼睛 ... 牙齿); 加上 gives a person mask overlay (`xr_added_0`) | pass (`x_add_5`), overlay within about 1.0-1.4 s of tapping 加上 |
| 人物 on p8 (3 faces) | pass: 脸 1 / 2 / 3 rows (`xr_tmwhole_0`) | not run |
| 天空 on lake.jpg | pass: sky mask on the clouds, not the bright band at the horizon, label says 粗略 (`xr_sky_3`) | pass, first use after launch: overlay between 1.4 and 1.9 s after the tap, WDA latency 0.4 s (`x_sky_5`) |
| 点选 (tap on the face) | pass: person mask in about 0.3 s after the tap, + / - strip under the photo (`xr_tapsel_2`) | not run |
| 美颜 chips and default recipe | pass: 肤质 / 眼下 / 油光 / 补光, 眼睛 / 嘴唇 / 牙齿, badge 8 (`xr_beauty_1`); mean RGB of cheeks, forehead and background before / after the default recipe differs by 0.0-0.3 levels (L, R cheek 170.7/117.6/101.4 -> 170.7/117.4/101.1), texture std down about 1 level: complexion unchanged | chips shown (`x_beauty_b`, badge 8); complexion not measured |
| analysis times | the person / face / sky analyses are finished before the tile row is reached (chips are there 0.8 s after the 人物 tap, mask 0.2-0.3 s after 加上); no log line in release (`local ai:` print is `kDebugMode` only), so the numbers are UI-observed, resolution about 0.4 s | see above |
| save to the app album "Dapple" | pass after answering the second prompt with "允许完全访问": toast 已存入相册「Dapple」, green save icon (`xr_s8_0`) | pass after "好" then "允许访问所有照片": icon changes to 已保存到相册 (`x_saved_3`) |
| editor back: fresh open, no panel | one tap leaves; a pinch zoom is not a layer, one tap leaves from the 1:1 view (`xr_zoomed`, tree after) | same, one tap after zoom |
| editor back with 局部 panel open | first tap closes the panel, second leaves (XR); with a 点选 strip, 点选 mode first, still two taps | three taps (strip, panel, leave) |

Timings, iPhone X, autotest (round 5 in brackets): sample.jpg open 119 (109), first film frame 414 (396), final 140 (139), strength drag 33.5 (35), exposure drag 41 (44); IMG_1105.dng open 612 (590), first 257 (297), final 175 (175), strength 29 (35), exposure 49 (46); xs10.RAF open 956 (721; round 7 measured 826 - 991), first 237 (243), final 166 (165), strength 29 (29), exposure 46 (44); maxRss 348 MB (304). No regression beyond noise; RAF open is in round 7's range.

Not run, and why:
* HDR merge, panorama: there are no bracket or panorama samples under `build/rawsamples` (none in `MANIFEST.md`). The "+" sheet shows the entries (HDR 合成… / 全景拼接… / 视频 → 动态照片…, `xr_plus_sheet.png`), but every tool starts with the system photo picker, which shows the owner's library; it was opened once to read its element tree (no screenshot) and cancelled. Merge on iOS is therefore only verified by the Mac run in the tools report.
* Live Photo export: needs a video from the photo library (none that is public) and was not run; the PhotoKit pair write was not exercised.
* Foldable work: the phone layouts look like round 5 (capsule bar, header, pad, 成像 readout unchanged; the 局部 list view is taller by the new 按内容选 rows). Not pixel-compared with the round 5 screenshots.

Defects found (product code untouched):
* On both phones the 局部 list view is clipped at the panel bottom: the 按内容选 row (美颜 / 人物 / 背景 / 点选 / 天空) is cut through the middle (X, landscape photo: `xr`-less `x_panel_scrolled` was taken after a scroll; before it the tiles show only their top half) and on a portrait-shaped photo (p2 on the XR) even the 明暗 / 颜色 tiles are cut and the AI row is off screen. It scrolls, but nothing says so. Panel height should fit the content or the AI row should come first.
* XR: with Photos "仅添加" kept (the default answer of WDA's `alert accept` picked it, we believe) 存相册 reports 保存失败 for every photo, edited or not (`xr_save4_5`, deleted). Add-only access should be enough to save; the app seems to need the read grant. After "允许完全访问" the save works.
* On the X a stale "限制访问" grant survived the uninstall / install and the first launch asked "选择更多照片… / 保留当前所选内容" instead of the full prompt.
* A tap on the photo in the editor hides all chrome including 返回 (a second tap shows it again); a tester who taps the photo sees a screen with no way out. Existing behaviour, not new.
* After full access the home screen lists two library photos from yesterday under 昨天 · 手机相册 (the 7-day PhotoKit scan; not looked at). My own saved exports also come back into the app library as new photos.

Cleanup: both apps uninstalled and reinstalled clean (all test imports and autotest files gone); WDA stopped on both phones, USB forwards and helper processes killed. Files left in the phones' Photos album "Dapple" (the app has no delete; the owner may remove them): XR 2 JPEGs of p3_curly_glasses_outdoor (one save, one accidental second save), X 1 JPEG of p6_asian_woman_grey with the default 美颜 recipe. The earlier `DAP_20261113_224008.*` files of round 3 are still on the XR.

## Round 7 (2026-11-14): HDR merge, panorama, video -> Live Photo on both iPhones (working tree of round 6, already installed, nothing rebuilt)

Test material (public, synthetic; evidence and sources in `build/ios-r7/`, `src/`): bracket = `IMG_1105.dng` (iPhone XS, public) developed with the desktop CLI at -2 / 0 / +2 EV (`--no-auto --params {"exposureEv":..}`, 2016x1512, mean luma 54 / 115 / 185): a **synthetic bracket**, perfectly aligned, no motion, no EXIF exposure differences. Panorama = three 840x1400 crops of the same developed image, x step 588 (30 % overlap), y offsets 0 / 30 / 15 px (synthetic: no parallax, no lens distortion). Video = 5 s 1280x720 MP4 (ffmpeg zoom/pan over `lake.jpg`), made but **not usable**: there is no way to put a video into the phones' Photos from here (the app has no video save, no Files source; I did not write into `/var/mobile/Media/DCIM` through AFC: Photos does not index it and the owner could not remove it). Photos reached the app through `Documents/Photos` + `DAPPLE_OPEN` (debug ingest). The tools' in-app pick sheet shows only the app's own library (no owner photos); the system picker was opened once on the X (only "相簿" and the "Dapple" album with my 4 items were opened; no screenshot of the owner's library).

| check | iPhone XR (iOS 18.7.10) | iPhone X (iOS 16.7.16) |
|---|---|---|
| HDR 合成: opens, accepts 3 items, preview, progress, result | pass: tool page with live preview (`xr_06`), progress dialog "对齐 → 合并 → 处理", 10 % at 0.7 s (`xr_07a`), result tile in the grid about 2 - 3 s after the tap; looks right: exposure balanced, no ghosting, no misalignment (`xr_08_hdr_editor`) | pass: 58 % at about 3 s (`x_05`), done in about 4 s; result tile with HDR badge; looks right (`x_22`, the chrome was hidden by a stray tap on the photo). The first try used the pano tiles by mistake: the merged result is a ghost mess, as expected for non-bracket input; the app gave no warning that the frames are not a bracket |
| HDR result edited and saved | edit pass (光线 pad drag, 光线 badge 亮 42 / 反差 43, `xr_09`, `xr_15`); save **fail** (see defect 2) | save pass for the pano result (icon 已保存到相册, `x_14`) and 3 selection saves ("已存入相册「Dapple」· 3 张", `x_15`) |
| 全景拼接: opens, 3 items, projection choice, progress, result | pass: 球形 / 柱形 / 平面, 自动裁切, 补全空白边缘 (`xr_18`), result in under 4 s (`xr_20_pano_editor`): no visible seam or step, columns straight, cropped to about 1.47:1 | pass, same (`x_12_pano_editor`) |
| system picker ("+" -> 从相册选…) | not tested (add-only state, nothing of mine in Photos) | pass: picker opens, album "Dapple" with 4 items, 3 picked, 添加 -> 3 new tiles `*_dapple` in the grid after about 4 s; they merge to a good HDR (`x_19`..`x_22`, 4 s) |
| 视频 -> 动态照片 | tool opens, empty state "相册里没有视频" (`xr_21`), 选择视频 only reopens the same empty list | not run |
| peak memory | no number: the release build writes none and `dvt` was not attached; no jetsam or crash seen | same |

Live Photo: **not tested** (no video available; PhotoKit pair write and the "one asset, Photos treats as Live Photo" check remain open). Peak memory of the merge: not measured.

Re-check of the round 6 defects:
1. 局部 list clipped at the panel bottom: **still reproduces on a portrait-shaped photo** (a pano tile on the XR: 明暗 / 颜色 tiles cut through, 按内容选 row off screen, `xr_23_local_portrait`); on a landscape photo the whole list fits (`xr_22_local`).
2. XR add-only save: **still reproduces**, and it is worse than "needs read": after 允许 (add) the app asks "额外访问", answering 保持"仅添加" gives 保存失败 for every save (`xr_16b`) and the banner becomes "还不能读取照片，已被系统拒绝" for good (no second prompt, only Settings could fix it; I did not touch Settings, so the XR stays in that state until Dapple is reinstalled). On the X the two prompts (添加 -> 允许访问所有照片) work and saving passes. The video tool shows "相册里没有视频" in that state (misleading: the phone has 50 videos, the app just may not read them), as does the home banner.
3. X stale "限制访问" grant after reinstall: **not reproduced** this round (fresh state: normal add prompt, then "选择照片… / 允许访问所有照片 / 不允许"); the state was not stale this time, so this is not a fix proof.
4. Tap on the photo hides all chrome including 返回: **still reproduces** (XR `xr_24` deleted, X again by a stray tap; a second tap on the photo brings it back).
Also: WDA hung again after the XR's "额外访问" prompt (recovery as in round 5).

Items left in the phones' Photos (the app cannot delete them): iPhone X, album "Dapple": 4 new items from this round, 3 JPEGs of the synthetic bracket (brk_m2 / brk_z / brk_p2) and the JPEG of the pano result (PANO_20261114_163542); plus the older ones of rounds 5 and 6. iPhone XR: nothing from this round (all saves failed). No video was placed anywhere.
Cleanup: `Documents/Photos`, `merged`, `inbox`, `live`, `previews`, `library2.json`, `detected1.json`, `DAPPLE_OPEN` removed from both apps (only `film_swatches.json` remains); WDA stopped on both phones, the two USB forwards of this round killed, nothing running on the Mac; no settings changed, no reboot, nothing uninstalled, no Mac sync, product code untouched.
