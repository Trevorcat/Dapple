# Xiaomi Redmi (2312DRA50C "garnet") device results, wave 8, 2026-11-12

Native only. The app could not be installed: `adb install -r` fails with `INSTALL_FAILED_USER_RESTRICTED: Install canceled by user` (MIUI `AdbInstallActivity` of `com.miui.securitycenter` waits for a tap on the phone; the screenshot of the prompt is black, a secure window). No setting was changed and no input was injected. Nothing was installed, so **no in-app number (logcat `dapple_view`, cadence, first photo, rotation, background/return, 30 s app session) exists**. The APK built from this tree is `build/redmi/app-w8-arm64.apk` (not installed).

## 1. Device facts (getprop / dumpsys)

| | |
|---|---|
| Model / device | 2312DRA50C / garnet, Android 16 (SDK 36), HyperOS `ro.miui.ui.version.name` V816 |
| SoC | `ro.soc.model` SM7435, board `parrot` (Snapdragon 7s Gen 2): 4 x 1.958 GHz (cpu0-3) + 4 x 2.4 GHz (cpu4-7) per `cpuinfo_max_freq` |
| GPU | Adreno (TM) 710, Vulkan 1.1.128 (driver 2150002787), GLES 3.2 V@0615.99; subgroup 64, shared 32 KB, fp16 arithmetic and 16-bit storage yes, linear filter RGBA32F no, timestamps yes, unified memory |
| Panel | 1220 x 2712, modes 60 / 90 / 120 Hz, active 120 Hz (`mActiveSfDisplayMode` id 1), display id 4630946545580055170 |
| State | keyguard not showing, thermal status 0 before and after a 90 s sustained load, Battery Saver off (`low_power` 0); GPU clock / busy nodes under `/sys/class/kgsl` are `Permission denied` (no root): GPU clock was NOT logged |
| Other | Dapple not installed before or after. Native runs use `taskset f0` (cpu4-7); `DAPPLE_GPU_NO_PIN=1` made no difference to the GPU passes |

## 2. Native baseline (tree HEAD 1088f2d + working tree before my changes), Vulkan, `dapple_film_gpu_tests bench`

Per pass with `DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PASSLOG=1 DAPPLE_GPU_PROF_REPEAT=10` (each pass repeated 10 times back to back = clock hot), ms:

| pass | 810x1080 interactive before | after | 1080x720 after | 1600x1067 before | 1600 after |
|---|---|---|---|---|---|
| film_point | 2.45 | 2.44 | 2.17 | 9.88 | 10.0 |
| film_gnoise | 0.66 | 0.66 | 0.59 | 1.28 | 1.28 |
| film_d3 (new, half-resolution levels) | | 0.71 | 0.63 | | 1.39 |
| film_h3 | 7.63 | 1.63 + 0.73 | 1.45 + 0.64 | 33.75 | 7.24 + 2.17 |
| film_v3 | 5.25 | 0.66 + 2.97 | 0.58 + 2.65 | 20.37 | 1.75 + 8.65 |
| film_pd | 2.31 | 2.15 | 1.90 | 5.36 | 4.47 |
| readback (bench only) | 2.85 | 2.82 | 2.49 | 5.45 | 5.45 |
| **total** | **21.2** | **14.75** (11.9 without readback) | 13.1 | **76.1** | **42.4** |

Un-profiled bench (GPU idle between frames, the harness' "GPU ms" is the submit-to-wait wall): 810x1080 interactive 50.7 ms alone against 18.6 ms hot, 1600 px interactive 121.6 (hot 71.5 before / 42 after), 1600 final 196, 810x1080 final 89 (hot 87), 3600 px final 776. The ratio cold / hot of 2.4-2.7 is the clock ramp (or per-submission overhead): GB/s of the passes in the table are 2-8 (film_h3 1600: 2.1), i.e. the film passes are nowhere near memory bandwidth; they are ALU / register bound on this GPU (section 3).

1600 px is super-linear against 810x1080 (2x the pixels, 3-4x the time in film_point / film_h3 / film_v3): not explained (cache capacity is the suspect, not isolated).

## 3. Causes found and what each fix bought (810x1080 interactive, hot, ms; every change keeps film_gpu.parity / frames within budget)

| cause | fix | before -> after |
|---|---|---|
| integer division in `reflectIdx` evaluated for every tap even for in-range indices (Catmull-Rom upsample: 8 calls per pixel) | early return `if (i >= 0 && i < n)` (`film_mix3.comp`) | level-0 film_v3 6.0 -> 4.5 |
| dynamically indexed `float wE[4] / wO[4]` and a conditional pick inside the 4x4 loop | two `vec4` weights chosen once per pixel, column indices hoisted | film_v3 4.5 -> 3.0 |
| `vec3 acc[4]` indexed by the slot loop in film_h3 | explicit `a0..a3`, taps read once per k | film_h3 1.88 -> 1.63 |
| wide tail components (sigma 2..3, r 8) at full resolution in the interactive tier | `Mix3::fastLow`: sigma >= 2 goes to pyramid level >= 1 (`kMinLowSigma` 1.0, `DAPPLE_FILM_FASTLOW=0` off) | film_h3 7.63 -> 2.36 (two passes incl. level 1), film_v3 5.25 -> 3.6 |
| total of the three | | **21.2 -> 14.75** (incl. 2.8 readback of the bench) |

Measured and rejected (slower or no gain, reverted): 4 output pixels per invocation in film_h3 (22.0 vs 7.6 ms: spill), shared-memory tile for the upsample (v3 6.3 vs 4.5), staged film_v3 (`DAPPLE_FILM_V3_DIRECT=0`, 18.6 vs 5.3), workgroup 64 (film_point 7.7 vs 2.4), explicit-array-free tetrahedral lookup in film_point (no change), ADPF from a native process (no session is granted to a shell binary: `adpf_session:false`), a concurrent GPU load while the session-path bench ran (17.5 -> 18.0 ms: not a clock issue in steady state).

## 4. Session path (`dapple_drag_bench`, C API, INTERACTIVE, host readback, big cores, Vulkan; one parameter moving; wall ms)

Steady `film_strength` frames by long edge: 120 px 1.0, 240 px 2.1, 480 px 7.5, 810 px 17.8, 1080 px 34.8 ms (all of it `exec` in the `cpu_ms` split; match / tables / upload 0). **The engine cannot hold 8.3 ms above about 400 px long edge on this phone**, and the number includes the host readback of a native bench; the zero-readback display path of the app saves part of it (not measured).

Drag bench at 480 px (320x480 frames), medians (first frame), after the changes, with the app's start-up probe + warm-up (`DAPPLE_BENCH_PROBE`):

| drag | median | first frame | notes |
|---|---|---|---|
| film_strength | 7.4 | 6.9 | one 150-250 ms outlier per run (max) not attributed |
| film_grain | 9.7 | 104-145 | first frame: CPU grain tables 103 ms + pipelines (`cpu_ms ... tables 103.5 exec 112.9`) |
| film_shoot | 21-35 | 73-627 | before the warm-up change 191 median; first frame = cold print-constant buckets when the drag starts < 1.3 s after the stock |
| film_print | 22 | 68-108 | |
| film_black | 23 | 21-24 | CPU table bake + upload (`upload 17-60 ms`) |
| film_latitude / film_wb / film_exp | 11.6 / 11.4 / 11.5 | 13-16 | |
| film_warmth / film_tint | 23 | 23-38 | table bake |
| exposure (film on) | 19-40 | 32-50 | |

30 s scripted session (all 12 drag cases, 120 frames each, 1440 interactive frames, `DAPPLE_FRAME_PROFILE=1` logcat `cpu_ms` = CPU side + GPU wait): 480 px: p50 20.0, p95 27.5, p99 43.2, **max 1406** ms (the cold first film frame of the process); 640 px: p50 35.3, p95 60.9, p99 64.9, max 1572. So **no budget of the brief is met on this phone**: 8.3 ms interactive (810x1080: 14.7 hot, 17.5 session), first drag frame <= 16 ms (grain 104-145, print 68-108, shoot 73-627), no frame over 33 ms (p99 43 at 480 px), first film frame after choosing a stock <= 50 ms (cold process 379-506 ms: upload 190-240 = tables + plan + display, record 136-146 incl. pipeline creation, wait 34-97; a warm number was not isolated). Thermal status stayed 0 over the ~90 s of continuous load (no 60 s app soak was possible).

Floor reached and what adaptive quality should do: at the measured cost the interactive long edge that fits 8.3 ms is about 360-400 px (`film_strength` 2.1 ms at 240, 7.5 at 480); the Dart controller now takes the budget from the panel refresh (`budgetForRefresh(120) = 8.33`) and has a 360 px rung, so on this phone it would settle at 360-480 px in a film drag; the 1220 px wide picture is then upscaled for the duration of the drag and replaced by the exact settled frame. The settled frame is unchanged (final tier). The visual cost (dE of interactive vs settled at screen size) was **not measured**. The calibration / micro-benchmark that would seed the rung before the first drag was not built.

## 5. Gates on the Redmi (this tree)

| gate | Vulkan | GLES 3.1 (`DAPPLE_GPU_API=gles`) |
|---|---|---|
| `dapple_film_gpu_tests` (film_gpu.parity, 40 cases) | ok, 0 failures, e.g. halation interactive mean 0.017 p99 0.053, final grain off 0.030 / 0.098 | ok, identical dE values, 0 failures |
| `dapple_film_gpu_tests frames` | ok (1536x1024 mean <= 0.078, p99 <= 0.85, max <= 1.87) | ok |
| `dapple_gpu_tests --film-tiling` | first run FAILED: `exact look LUT + spatial: mean 0.109` (budget 0.1), second run PASSED (mean 0.090). That case has no film and runs none of the shaders I changed; Adreno 710 results move between runs (self-test spatial mean 0.076 / 0.112, point+geometry 0.040 / 0.054): a device numeric noise near the budget, not a regression of this wave | PASSED |
| `dapple_gpu_tests --coverage` | PASSED, 192 / 192 on the GPU executor | PASSED |
| `dapple_golden selfcheck` | arm64: 148 frames, 0 mismatches, SELFCHECK OK | |
| `dapple_golden seqcheck` (engine.match_cache_seq) | 60 settled frames, 17 sequences, 0 mismatches | |
| `dapple_match_cache_tests` | ok (now takes the pack from `DAPPLE_FILM_DATA`) | |
| `dapple_exec_tests` | ok | |
| GLES film correctness (the Adreno 620 / 640 wrong-colour report) | | **not reproduced on the Adreno 710**: all film rows pass with the same dE as Vulkan; the Pixel 5 was not touched (battery) |

Desktop (RTX 4060 Ti, `build/w8a`): `ctest -R "film_gpu|golden|match_cache|gpu_parity|engine\.|exec"` 35 / 35 passed (includes film_gpu.parity, film_gpu.frames, gpu_parity_swiftshader, engine.golden_*, engine.match_cache_seq). Full `check-all` was not run (other engineers' files do not build: `tests/video/video_session_tests.cpp` was mid-edit during my arm64 build).

## 6. Warm-up on this phone

`gpu probe: api=vulkan ... self-test cached, probe total 47 ms`; `gpu warm-up: film pipelines + default stock 790 ms (film check: mean dE 0.044 p99 0.75 max 1.67)` (GLES: 52 ms / 658 ms, same dE). Frames before the probe is ready run on the CPU with the reason "GPU self-test running" (nothing waits on it; first photo opening in the app was not observed). Fresh first film frame of a process (session path): 379-609 ms.

## 7. Not done / not verified

App install and everything in the app; GPU clock / busy logs (not readable); a per-device micro-benchmark and a cross-device ratio table (Pixel 5 not used); ADPF and cadence effect in the app; Vulkan-vs-GLES choice by measurement (on this phone GLES drag frames were not slower: film_strength 480 px median 12.5 vs 14.9 ms Vulkan, 5 frames each, noisy); first-drag-frame targets; the remaining per-frame CPU cost of table drags (17-60 ms, bake + upload) on this SoC; the Pixel 5 `cpu_ms 234 (match 110 upload 124)` pad-drag case; the 60 s soak in the app; rotation, background/return.

End state: `/data/local/tmp/dapple` removed from the Redmi at the end, no `debug.dapple.*` property was set, no setting was changed.

# Round 2 (same day, owner direction: find where the hardware is not used; nothing per device in code)

## 8. Findings

* **Core selection is capability-driven already.** `exec/big_core.h` picks the cores whose `cpuinfo_max_freq` is >= 80 % of the top one (no indices, no names). The only fixed mask was mine: `taskset f0` in the bench scripts. Dropped: without it the engine's own pinning gives the same numbers (film_strength 480 px median 7.3 ms with and without). Nothing else in `core/` names a model, GPU or core.
* **Precision (mediump / RelaxedPrecision): no gain, reverted.** `precision mediump float;` on `film_mix3.comp` does reach the SPIR-V (`glslc -O` output has 71 `RelaxedPrecision` decorations, checked with `spirv-dis`), and the Adreno 710 per-pass times did not move (film_h3 1.635 -> 1.635 ms, film_v3 2.97 -> 2.90, total 14.75 -> 14.84). So these passes are not limited by fp32 ALU rate; the driver either already runs them as fp16-eligible or the limit is elsewhere (load/store latency, occupancy). Not applied to other shaders and the GLES path was not tried. On the GLES wrong-colour bug of the Adreno 620 / 640: from the shaders alone nothing supports a precision cause (the film shaders use the same highp ESSL path as on the Adreno 710, where GLES matches Vulkan to the last digit, and the only mediump-sensitive operations, the `h16` rounding helper and `floatBitsToUint`, are precision independent); a driver bug in image load/store or `texelFetch` of RGBA16F planes is as likely. The new warm-up film check demotes such a device by behaviour.
* **Cadence (item 5), native session bench, 480 px film_strength, median ms:** back-to-back 7.3, paced at 8.33 ms 7.5, 11.1 ms 8.3, 16.7 ms 8.5. At 640 px: 14.7 back-to-back, 14.7 at 16.7 ms, 18.3 at 33 ms. At 810 px the numbers were noisy and device-state dependent (24.5 back-to-back in this run, 17.8 in the earlier one; 35.2 paced at 16.7 ms, 25.3 at 33 ms). So vsync-paced frames cost 0 to 15 % more at sizes that fit the period and up to 1.4x in a noisy case at a size that does not; no clear "cold clock" signature at the sizes that matter, **no keep-warm was implemented** (nothing measurable to justify the battery cost). ADPF could not be measured natively (no session for a shell binary: `adpf_session:false`); the app already creates the session in `configurePerf` (`core/src/sysperf.h`), untested on this phone.
* **Superlinear cost (item 4): a cliff between 15.2 and 17.8 MB, not a slope.** Size sweep of the interactive film frame (film_point, hot, ms): 600x800 1.35, 810x1080 2.43, 1100x780 2.64, **1200x800 2.65 (source 15.4 MB, 0.96 MP)**, **1300x867 7.5-8.3 (source 18.0 MB, 1.13 MP)**, 1400x933 8.9, 1500x1000 9.4, 1600x1067 10.0. Same pixel rate below the step (2.7 ns/px) and 7-8 ns/px above it; film_h3 shows the same step (3.3 ms at 1200x800 for the level-0 pass, 7.2 at 1600). The step sits where the RGBA32F source (and the 8 B/px RGBA16F planes) cross about 16 MiB. Not isolated further (no GPU counters without root): candidates are a driver limit on large linear images (no compression, TLB reach) or the system cache. Consequence: every interactive rung of the Dart ladder (<= 810x1080, 14 MB) is below the step, the 1600 px still / detail frames are above it; **a banded (windowed) record of frames above the step is the fix** (the film window record exists for tiled export), not done.
* **Ratios to the best simple pass (Redmi, hot):** the fastest pass in the 1600 px grain-drag frame is `film_enc` at 30 GB/s (reference "copy-like" pass), film_point 4.1 (0.13), film_h3 2.1 (0.07), film_v3 4.9 (0.16), film_pd 6.8 (0.22); at 810x1080 film_point 8.5 (0.28), film_h3 13 (0.43), film_v3 10 (0.33), film_pd 8 (0.27). The N5 records (wave 6, `find-n5.md`): simple passes over 100 GB/s, film_v3 10-26 GB/s (0.1-0.26). So relative to its own simple passes the Redmi is no worse than the N5 on the film passes, and its simple passes are 3-4x slower in absolute terms: the gap to the N5 is mostly raw throughput (memory system and ALU) plus the 16 MiB step above 1.2 MP, not a pass that is pathological here. The measurement was done by hand from the bench tables; the in-engine micro-benchmark that would produce it at warm-up and seed adaptive quality was **not built**.
* **Per-drag costs (item 6) and the Pixel 5 log, from code:** `cpu_ms 234 (match 110 ... upload 123.8)` on a film pad drag is the table-moving drag path (film_print / film_shoot / film_black / warmth / tint): each frame re-bakes the 17^3 coarse print table on the CPU and re-uploads it (17-60 ms here on an A78, 124 ms on the Pixel 5's A76/A55 mix is the same mechanism at a slower CPU), and `match` is the first print solve / cold calibration bucket (110 ms there, 440 here for a cold bucket). Neither is keyed by the interactive size: the table key is film + paper + parameters, the match key is the lean key; changing the interactive size mid-drag was not reproduced. Not fixed: the bake needs to move to worker threads outside the frame pool with interpolated tables, or onto the GPU; also not done: grain tables cached on disk, bake-at-selection. The warm-up workers share the pool with the frame thread (19 -> 23 ms on table drags while they run).
* **Visual cost of the interactive size (item 7):** not measured. Adaptive quality: budget = one refresh and a 360 px rung (round 1); the calibration seed and the oscillation guard beyond the existing up-after-12-frames hysteresis were not added.
* **Compute vs raster (item 2):** not prototyped (a render-pass path in the Vulkan HAL was out of reach in this round). No numbers.

## 9. APK (item 8)

`:app:lintVitalAnalyzeRelease` crashes inside Kotlin lint ("Unexpected failure during lint analysis of **CodecSelfTest.kt**", `KotlinIllegalArgumentExceptionWithAttachments` from the K2 FIR symbol code): the offender is `app/android/app/src/main/kotlin/com/dapple/dapple/CodecSelfTest.kt` (modified in the working tree, not mine; not edited). Workaround for this build only: a temporary Gradle init script in `tools/gradle-home/init.d/` disabling `lintVital*` (removed again), then `flutter build apk --release --target-platform android-arm64`: **`build/redmi/app-w8-arm64.apk`** (36 MB, debug key, same as the earlier APKs) for the owner to install by hand. **Untested**: it was never installed or launched (MIUI install prompt). It carries this tree's engine changes (shader fixes, warm-up film check, two prewarm workers) and the Dart adaptive-quality change.

## 10. Gates added in round 2

Size sweep and cadence benches are test-side only (`DAPPLE_BENCH_SIZE`, `DAPPLE_BENCH_PERIOD_MS`, `DAPPLE_BENCH_ADPF`). The shader state at the end of round 2 equals round 1 (the mediump experiment was reverted), so the round 1 gates stand. `tools/check-all.sh`: see the final report.

# Round 3 (print tables off the frame thread, Metal case, film log)

## 11. Per-drag CPU cost: background print tables

Cause (same on the Pixel 5): every drag frame of film_print / shoot / black / warmth / tint / wb had a new print-table key, so the frame thread baked the 17^3 coarse table (`bakePrintScanCoarse`, 17-60 ms on an A78) and uploaded it; the `match` / calibration part is a separate cost (section 8).

Fix (`film_tables.cpp`, `parallel.h`): an interactive drag frame never bakes a print table on the frame thread any more. It uses the coarse table of its own values when cached, otherwise the most recent table of the same stock / paper / mapping / split path (the previous step of the drag, or the settled table the drag started from) and hands the wanted table to one worker thread (latest request wins) that bakes it on a second thread pool (`ThreadPool::background()`: half of the capability-chosen cores, never the frame pool, normal priority). The first table of a stock (nothing to fall back to) still bakes on the frame thread. The settled frame is not coarse and bakes / fetches the exact table exactly as before; `DAPPLE_FILM_ASYNC_TABLES=0` restores the synchronous path. The calibration warm-up workers also run on the background pool now (they no longer take the frame pool).

Not done from the brief: moving the dependency out of the table (print exposure, warmth and tint act before the paper curve, the paper layers cross-talk spectrally: no exact pre / post transform, and `film_black` was not isolated as a post-mapping), GPU interpolation between bracketing tables, sub-data table upload (the upload of a 17^3 RGBA32F table is 79 KB; the measured cost was the bake), grain tables and calibration persisted on disk, bake-at-selection of grain tables.

Redmi, native session bench, 480 px (320x480 frames), medians (first frame), after warm-up, `DAPPLE_BENCH_SETTLE_MS`:

| drag | before (round 1) | after |
|---|---|---|
| film_shoot | 21-35 (73-627) | 7.0-11 (7-19; 757 when started within 1.3 s of the stock) |
| film_print | 22 (68-108) | 8.6-9.5 (64-212: first print solve) |
| film_black | 23 (21-24) | 9.4-10 (8.5-11.7) |
| film_warmth / film_tint | 23 (23-38) | 10.1-10.3 (10-23) in a fast run, 24.9 in a slow-state run |
| film_latitude (GPU only, no table) | 11.6 | 11.9 (22 in the slow-state run) |
| film_grain | 9.7 | 10-16 (first frame 88-143: CPU grain plan / table 103 ms, not fixed) |

The device alternates between a fast and a slow state (the GPU-only latitude drag moves between 11 and 22 ms); table drags now sit on the GPU-only floor in either state. 8.3 ms median is **not** reached at 480 px (floor 7.3 ms film_strength in the fast state, 11 ms for the others). The calibration warm-up (15 buckets, two workers on the background pool) finishes **3.2 s** after the first match of a stock (`DAPPLE_ENGINE_LOG=1` prints it); a drag started earlier meets cold buckets (film_shoot first frame 757 ms in the worst run). The develop-exposure drag with a film on still costs 17-44 ms (the per-frame film match: statistics of the 256 px proxy, 7-34 ms on this CPU), not addressed.

Colour lag: while the worker bakes, frames show the previous step's table (a stale-by-one-or-two-steps colour); `film_tables stale S baked B requested R` in the log line counts it (601 stale frames of 1441 in the scripted session, 527 tables baked). The settled frame after the finger lifts is exact; a finger held still on the last value shows the stale colour until it lifts or the next frame (the engine has no repaint request).

30 s scripted session (12 drag cases, 120 frames each, 1441 interactive frames, 480 px, `cpu_ms` incl. GPU wait, Vulkan, settled warm-up): **p50 15.1, p95 24.8, p99 27.0, max 1317 ms (the cold first film frame of the process); 9 frames over 33 ms** (cold first frames of the cases: grain 111, print 65, strength outlier 78, black 122). Round 1 for the same session at 480 px: p50 20.0, p95 27.5, p99 43.2.

## 12. Other round 3 items

* **Film log line (item 5):** film frames already go through the same `noteFrame` as every frame (the view path calls `dapple_session_render_frame` -> `EngineSession::renderFrame`; the log is always on on Android); the Pixel app log has the film line. The throttle (one line per 30 frames, plus every frame over 25 ms, plus every frame with `setprop debug.dapple.frame_profile 1`) is why a short drag shows one line. Not reproduced for the Note10+ (its only log on record, `app_start1_logcat.log`, ends at start-up). The line now ends with `film_tables stale S baked B requested R` for film frames.
* **Metal (item 7):** `gpu_context.cpp` / `engine_session.cpp` know `Api::Metal`: the JSON status takes `hal::apiName` ("metal" / "vulkan" / "gles31"), the rebuild path re-selects the same API, Apple tries `metal` then `gles` instead of the Vulkan alias, `exec == "metal"` counts as GPU, `FrameResult::executor` can be "metal". Desktop ctest (film, golden, match_cache, gpu_parity, engine.exec, engine.cache: 50 / 50) and the arm64 build / device gates are unaffected; **not compiled on Apple** (no Mac here).
* **Not done:** item 2 (bands under the 16 MiB step), item 3 (raster prototype), item 4 (micro-benchmark seed and the interactive-vs-settled dE table), item 6 (re-pointing film_match / film_match_cache proxies to the engine).

Gates on the Redmi after round 3: `dapple_match_cache_tests` ok, `golden seqcheck` 60 frames 0 mismatches, `selfcheck` 148 frames 0 mismatches, `film_gpu_tests frames` ok, `--coverage` 192 / 192, `exec_tests` ok.

# Round 4 (repaint, device table updates)

## 13. Item 1: repaint when a queued table lands

`film_async.h` (`addTableLandedListener`), `film_tables.cpp`, `view.cpp`, `dapple_c.h` (`dapple_set_repaint_callback`). A frame that used a previous-step table sets a flag *before* it queues the wanted table; when the worker finishes the newest request (no newer one pending) and the flag is set, the listeners are called on the worker thread. `dapple_view` registers itself (live views tracked under a lock) and resubmits its newest snapshot, so the picture converges without input; a host that renders frames itself registers `dapple_set_repaint_callback` and renders the last frame again. **Untested in the app** (no install); tested natively with `dapple_drag_bench ... converge` (a film_black drag that stops on a value, no further input), Redmi, 480 px:

| run (stop value) | table landed after the start of the last drag frame | last drag frame vs converged frame (mean abs, /255) | converged frame vs the same frame 500 ms later |
|---|---|---|---|
| 0.25 | 7.0 ms | 0.019 | 0.000 |
| 0.45 | 7.2 ms | 0.034 | 0.000 |
| 0.65 | 7.2 ms | 0.052 | 0.000 |

So staleness at the end of a drag lasts about 7 ms (one table bake on the background pool) plus the repaint frame (one vsync); the repainted frame is already the exact coarse table of the stop value. The difference between the stale frame and the converged one was 0.02-0.05 /255 for an 11-step drag (larger for a faster drag). Over a whole scripted session (1441 frames) 601 frames showed a previous-step table.

## 14. Item 2 (partly): the device-side table update

Finding: with the bake off the frame thread, the remaining spikes of a table drag were `uploadGpuTables`: every changed print table re-created the raw / LUT / curve / decode resources and re-uploaded all of them (including the 600 KB raw table) with a full `waitAll()` (26-61 ms on this phone). New `updateGpuPrintLut` replaces only the print LUT texels in place when only the print table changed (same raw / curves, same LUT size, no split path), `DAPPLE_FILM_LUT_INPLACE_OFF=1` disables it. Session bench (fast device state), 480 px medians: film_shoot 9.0, film_print 8.8, film_black 8.5, film_warmth 7.9 ms (GPU-only drag film_latitude 8.0), against 10-12 before the in-place update in the same state; spikes of 26-61 ms remain in some landing frames (the `waitAll` and the staging copy: not isolated).

Frames over 33 ms in the 721-frame scripted session (12 cases x 60 frames, settled warm-up): 32 (p50 10.3, p95 30.9, p99 53.3, max 1240 ms). By cause: the cold first film frame (1240 ms: match 468 + exec 772 + upload 356), the first grain frame (76 ms: tables 12 + plan upload), the first print frame (62 ms: first print solve 43 ms of `match`), a handful of table landings (26-61 ms upload), and **about 25 frames of the develop-exposure drag with a film on (33-58 ms: the per-frame film match 7-43 ms, rest GPU wait)**. The synchronous-slow-state run of the same session gave p50 17.3, p99 36.2 with 13 frames over 33 ms: the device state moves these numbers by 2x.

Not done in item 2: baking the print table at the current values, the grain plan / tables and the first print solve at stock selection on the background pool; disk persistence (calibration results, grain tables) with a size cap; the exposure-drag match (async result + repaint, or analytic: not checked whether the statistics scale with the gain; the exposure drag recomputes the develop render of the 256 px proxy per frame, so they do not scale exactly through the tone curves).

## 15. Items 3-6 of round 4

Not started (bands under the 16 MiB step, micro-benchmark and visual-cost table, raster prototype, re-pointing `film_match*.cpp`). Targets of the brief are therefore not met: median 8.3 ms at the size adaptive quality picks was reached only in the fast device state for drags that do not touch the film match (7.3-9.0 ms at 480 px), and the exposure drag with a film stays at 17-44 ms.

# Round 5 (match refresh off the frame thread, warm-up tiers, library size)

## 16. Develop-exposure / print match: previous result + background refresh + repaint

`core/src/c_api.cpp` (`withFilmKey`, `computeFilmMatch`, `queueFilmMatch`): an interactive frame whose film match is not cached uses the previous result of the same stock and queues the recompute on the background pool (latest request wins, own worker, `ThreadPool::background()`); when it lands the repaint listeners fire (same hook as the tables, `markMatchStale` / `notifyRepaintIfMatchStale`). A non-interactive (settled) frame computes the exact match itself and waits for a running refresh at most one match long (`matchMtx`); the first film frame of a stock (no previous result) still computes synchronously. `DAPPLE_FILM_MATCH_ASYNC=0` restores the synchronous path. I did **not** show the scene statistics are a pure function of the gain: the proxy statistics go through the develop tone curve, which is not scale-invariant, so the match stays a render of the 256 px proxy and only moves off the frame thread. Colour lag: the film EV compensation of an exposure drag follows the slider by one match (7-43 ms plus a repaint), the settled frame is exact.

Redmi, native session bench, 480 px, medians (first frame), warm-up settled:

| drag | round 4 | round 5 |
|---|---|---|
| exposure (film on) | 17-44 (33-58 over 33 ms: ~25 frames) | **7.8-7.9** (10.1-10.7; max 13-14) |
| film_print | 8.6-9.5 (62-212) | 9.1-9.9 (**8.7-9.6**) |

## 17. First frames: warm-up tiers, LUT image size change, app-like measurement

* The bench with `DAPPLE_GPU=1` creates the device lazily on the calling thread and never runs the probe / warm-up (the app runs it in `Auto` mode); the earlier first-frame numbers were therefore **cold pipelines**. The app-like measurement is `DAPPLE_BENCH_PROBE=<dir> DAPPLE_BENCH_PROBE_WAIT_MS=6000` without `DAPPLE_GPU`.
* `gpu_context.cpp warmFilm` now renders five tiers (interactive, full, interactive kept grain, **film_strength 60 (the Blend pipelines)**, **film_black (the coarse print table path)**), 1199 ms on this phone in the background (was ~790 ms). With it, first frames of a drag (480 px, after the warm-up): film_grain **15.4 ms (was 88-143)**, film_strength 13.2, film_print 8.7, film_black 9.4, film_shoot 12.7, film_warmth 18.8 (slow-state phase), film_tint 17.2. Everything at or under 16 ms except warmth / tint / wb / exp in a slow-state stretch (their medians were 18-22 ms in that stretch too).
* `updateGpuPrintLut` now also handles the size change exact 33^3 <-> coarse 17^3 (only the LUT image is re-created); remaining landing spikes: 15-28 ms (one 48.6 ms in the earlier run): the `waitAll()` and staging copy are still on the frame thread, **the fence-and-swap version was not built**.
* Cold: the first film frame of a process on this phone is a CPU frame (65 ms, "GPU self-test running") followed by the first GPU film frames (35-47 ms) and one 528 ms frame (first match 286 ms + first table upload 228 ms) while the probe (107 ms) and the warm-up (1.2 s) run; choosing a stock inside an open photo (warm process): the first film frame is the GPU frame with the stock's tables (match 0-286 ms, tables upload 84-228 ms): **not hidden**, the default stock only is warmed.
* Persisting calibration results and grain tables on disk, baking the print table / grain plan at stock selection (other than the default stock): **not done**.

## 18. Scripted session distributions, fast and slow device state

How the state is told apart: the GPU-only drag `film_latitude` (no tables, no match, 480 px, the same frame as `film_strength`) is **7.5-8.0 ms median in the fast state and 11.5-12 ms in the slow state** (the `still` case: 6.9 / 11-12 ms). The phone changes state within a run (the cause is not known: GPU clock governor or background load), so each number below carries the latitude median of its own run, and a run whose per-case GPU-only baseline differs inside the run is marked mixed.

| run (480 px, 721-728 frames, warm-up per case as stated) | state | p50 | p95 | p99 | max | frames > 33 ms |
|---|---|---|---|---|---|---|
| `DAPPLE_GPU=1`, no warm-up (cold pipelines), settle 8 s | fast (latitude 7.8-7.9) | 9.8 | 16.5 | 27.7 | 1505 (cold start) | 4: cold start, first strength 80, first grain 102, one landing 49 |
| app-like (probe + warm-up 6 s), settle 8 s | mixed: fast for still..black, slow for wb..exp (latitude 11.9, wb / warmth / tint / exp 18-22) | 14.7 | 22.5 | 35.2 | 528 | 10 (cold start and the CPU frames before the probe: 327, 127, 65, 63, 52, 48, 35; one strength 42; one landing 35) |
| `DAPPLE_GPU=1`, settle 8 s (round 4, for reference) | slow | 10.3-17.3 | 30.9 | 36-53 | 1240 | 13-32 |

No frame over 33 ms inside a drag in the fast state apart from the documented cold start and one table-landing / strength spike per run. Median <= 8.3 ms holds in the fast state for shoot (9.0-9.3: not quite), print 9.1, black 8.1-8.7, latitude / wb 7.8-7.9, warmth 8.1-8.6, tint 8.0-8.7, exposure 7.8: **the 8.3 ms median is reached by latitude, wb, exposure, blacks and tints in the fast state, not by shoot / print / grain (16.5-17.2 ms: the kept-grain frame) / strength (13.2-13.8: the Blend passes)**; in the slow state every case is about 1.5x those numbers.

## 19. Size of libdapple.so

The 58 -> 89 MB growth of `build/w8a-arm64/bin/libdapple.so` is debug info: the arm64 configure (`tools/check-all.sh` flags) builds with `-g` and `.debug_info` + `.debug_loc` + `.debug_str` + `.debug_ranges` + `.debug_line` are 80.5 MB of the 87.9 MB; `.text` is 4.4 MB (1.9 MB compressed) and `.rodata` 1.06 MB. The wave 8 code (DNG opcodes, video session, Metal glue, async tables / match) added ~30 MB of debug info and a little code. What the APK carries is the stripped library (Gradle strips): `lib/arm64-v8a/libdapple.so` in the release APKs, raw bytes (stored, not compressed): wave 4 7,658,992; wave 6 8,241,928; wave 7 8,296,632; **wave 8 8,410,392** (+114 KB, +1.4 % over wave 7); `libdapple_video.so` 82 KB in wave 8. Stripping the CMake output by hand (`llvm-strip --strip-unneeded`) gives 6.8 MB, the APK build links with different flags and carries 8.4 MB. Only the arm64-v8a ABI was built in this wave (x86_64 APKs were not rebuilt: the last one on record is `build/app-release-x64.apk`).

## 20. Not done in round 5

Fence-and-swap table upload, disk persistence, grain / print tables at stock selection for stocks other than the default, items 3 to 6 (bands under the 16 MiB step, micro-benchmark and visual-cost table, raster prototype, re-pointing `film_match*.cpp`).

# Round 6 (golden stability, stock selection)

## 21. The unexplained golden failure: understood and fixed

Reproduced under load: `dapple_golden selfcheck` failed 1 run of 24 (four at a time, twelve CPU hogs) with `MISMATCH lut/interactive: off c3840b67... full 5afa1ddc...`, always the same frame, never with one process alone. Cause: **`writeCube()` in `golden_tests.cpp` (and `engine_tests.cpp`) wrote one fixed file, `<temp>/dapple_golden.cube`, in every process; `ctest -j` runs `engine.golden_off`, `golden_full` and `selfcheck` (separate processes of the same binary) at the same time, so one process truncated and rewrote the file while another read the look LUT: a torn LUT gives a different `lut/interactive` hash.** (The `final` and `exact` LUT frames read the file too but only the interactive one hit the window in the observed runs.) Fixes: per-process cube names (removed at exit) in both files, the same per-process tags for every other fixed temp name in the tests (`tests/test_tmp.h`: `film_tests`, `core_tests`, `dng_opcode_tests`), and one shared deterministic mode: `core/src/determinism.h` (`DAPPLE_DETERMINISTIC=1`) switches off the timing dependent interactive paths (async print tables, async film match, the background median seeding, the stock fallback of section 22), pinned by `tests/deterministic.h`, which `golden_tests`, `engine_tests`, `exec_tests`, `film_gpu_tests`, `match_cache_tests` and `gpu_tests` include. My earlier per-binary `setenv` in `golden_tests.cpp` is gone. The earlier `engine.golden_off` failure of round 5 (one run, log lost) is most likely this race; the async-match mismatch of round 5 was a second, separate cause (fixed then).

Stress, desktop `build/check-desktop-redmi`: `ctest -R "golden|selfcheck|match_cache" -j4 --repeat until-fail:30` with twelve busy-loop CPU hogs: **30 repetitions, 0 failures (404 s)**; `-R golden` 20 repetitions at -j4 under the hogs: 0 failures. One `SEGFAULT` of golden_off + golden_full appeared in a run started right after a rebuild whose linker output I did not see complete (the executable was probably half linked); it did not recur in 70 repetitions. Full `check-all` in own dirs after the fix: see the final report.

## 22. Stock selection: no frame-thread bake, previous stock until the new one landed

Baseline (Redmi, native bench `dapple_drag_bench ... stockslide`: 10 stocks selected one every 300 ms = 3 s, frames in between, 480 px, warm-up settled, interactive frames): **cold in process: stock tap -> first frame mean 376-506 ms, max 613-1245 ms; warm in process: mean 86-95 ms, max 310-430 ms; 10 frames over 33 ms per pass.** Causes: (1) the table LRUs (raw / curves / print) held 6 entries, so ten stocks thrashed them (the warm pass re-baked everything); (2) the first use of a stock bakes tables + plan + match on the frame thread.

Fixes: (1) LRU size 6 -> 24 (`film_tables.cpp`, about 1.5 MB per stock). Alone it takes the warm pass to **mean 10 ms, max 15.6, 0 frames over 33 ms**. (2) `GpuExecutor::filmResolve`: an interactive frame of a stock whose raw / curve / print tables are not cached renders with the previous stock's resolved state (frame fields from this frame, table fields from the last frame, so nothing is baked) and queues the new stock's tables and plan on a latest-wins worker on the background pool (`film_tables.cpp requestStockPrep`, `tablesCachedFor`); the repaint hook fires when it landed (view / `dapple_set_repaint_callback`). The film match follows the same rule across stocks (`c_api.cpp`: previous result of any stock while the refresh runs). The settled frame is not interactive and never substitutes. `DAPPLE_FILM_STOCK_FALLBACK=0` and `DAPPLE_DETERMINISTIC=1` turn it off.

Result (three runs, fast and slow state mixed):

| | stock tap -> first frame (mean / max) | frames over 33 ms in the 3 s | frame p50 / p95 | correct stock on screen after (mean / max) |
|---|---|---|---|---|
| cold in process, before | 376-506 / 613-1245 ms | 10 | 6.4 / 520 | (immediately, blocking) |
| cold in process, after | **7.7-11.3 / 16.2-17.2 ms** | 0-1 (one frame at 34-37 ms) | 6.2-6.8 (fast) or 10.8 (slow) / 11-13 | 38-47 / 193-301 ms |
| warm in process, before | 86-95 / 310-430 ms | 10 | 12.7 / 50 | |
| warm in process, after | 6.9-12.6 / 12.8-30.9 ms | 0-1 | 6.3-6.8 or 11.4 / 10.7-13 | 14-37 / 145-289 ms |

"Correct stock on screen after" is the time of the last repaint callback after the selection (the stock tables and, for a stock never seen, the film match, which is the slow part: 193-301 ms cold); while it runs the picture shows the previous stock. **Not done: 'warm from disk'** (no per-stock persistence of tables, calibration results or grain tables, no size-capped cache directory) and **the fence-and-swap upload** (table landings still pay the `waitAll` and staging copy on the frame thread: 15-49 ms in a few frames per drag; the measured slide shows one frame over 33 ms at most). Cold stock tap -> first correct frame therefore is 8 ms (previous stock) then 40-300 ms (new stock), against 376-1245 ms blocking before.

## 23. Distributions of the final tree, fast and slow state separately

How the state is told apart: in the same run, the GPU-only `film_latitude` drag median. With `DAPPLE_GPU=1` (no probe thread) it is 6.9-8.0 ms fast / 11.5-12 ms slow; in the app-like mode (probe and warm-up running, the mode of the table below) it is 11.5-12 ms fast / 21-22 ms slow (the same frame costs about 4-5 ms more while the app-like warm-up thread exists: not explained). A run whose cases show both are marked mixed.

| run (480 px, 728 frames, 12 cases x 60, app-like, settled warm-up) | state | p50 | p95 | p99 | max | frames over 33 ms |
|---|---|---|---|---|---|---|
| fast run 1 (latitude 11.5; shoot 9.2, print 9.4, black 8.7, warmth / tint 8.0, exposure 8.8, strength 12.7, grain 16.9) | fast | 10.8 | 16.8 | 33.4 | 654 (cold start) | 9 |
| three slow runs (latitude 21) | slow | 14.2 / 15.2 / 15.3 | 21.2 / 22.8 / 22.8 | 30.3 / 38.0 / 31.8 | 512-605 | 8-9 |
| fast run 2 (turned slow after the first cases) | mixed | 15.5 | 23.0 | 35.7 | 459 | 10 |

The frames over 33 ms are the cold start (the CPU frames before the probe, the 500+ ms first GPU frame) plus about two table landings per run. Still above 8.3 ms in the fast state at 480 px: strength 12.7-13.2 and the kept-grain frames 16.9-17.2.

## 24. Items 3-7 of round 6, APK, size

Items 3 (strength / grain frames), 4 (bands), 5 (micro-benchmark and visual-cost table), 6 (raster prototype) and 7 (re-pointing `film_match*.cpp`) were **not started**. `build/redmi/app-w8-arm64.apk` was **rebuilt from the final tree** (38.3 MB; `libdapple.so` stripped 10,440,496 bytes, up from 8,410,392 in the previous APK: +2.0 MB from this round's merged tree, not measured per change), with the lint tasks skipped by a temporary Gradle init script (CodecSelfTest.kt lint crash, see round 2), debug key, never installed or launched.

# Round 7 (arm64 grain knob, Apple hunks, measured film_v3 variant, film check)

## 25. Item 1: `film.develop_film`, grain 150 / 200 on arm64: not reproduced on the Redmi

`dapple_film_tests develop_film` (arm64 NDK build, Cortex-A78, the portable kernel set) passes on the Redmi with the 150 / 200 cases: `grain knob: std dev 0 -> 0.00013, 50 -> 0.00078, 100 -> 0.00154, 150 -> 0.00231, 200 -> 0.00308`, `halation energy 100 -> 3.62, 150 -> 4.11, 200 -> 4.45`, 59 checks, 0 failures (the pack comes from `DAPPLE_TEST_PACK`). A read of every grain amount path found no remaining clamp below 200 (`edit_params.cpp` 0..200, `renderer.cpp:751` and `film_tables.cpp:50` clamp to 2.0, `grainPct` is passed unclamped to the plan). So the M1 failure is specific to that build (Apple clang on arm64: same kernels, but not the same binary) or a binary / pack older than the cap lift; I cannot reproduce it without the M1. New rows in `film_gpu.frames` (`film grain 150`, `film grain 200`, `film interactive grain drag 200`, strict budget for the first two): GPU vs CPU mean dE 0.074-0.076, p99 0.82-0.84, Vulkan and GLES on the Adreno 710 and desktop. Those rows cannot catch a path that clamps both CPU and GPU identically; `develop_film` does (it compares std dev against the knob). Needed from the iOS engineer: the exact command, the printed `grain knob:` line and the `git rev-parse HEAD` of the failing binary.

## 26. Item 2: Apple hunks, and film_v3 chosen by measurement

`Core::create` (`DAPPLE_HAL_METAL` -> Metal) and the Apple default `Mode::Auto` in `envMode` are applied as written in `docs/ios/metal-backend.md`. The third hunk (`v3Direct` keyed on the Apple vendor id) is **not** applied: `v3Direct` now follows, in this order, `DAPPLE_FILM_V3_DIRECT`, the warm-up override, the measured verdict, and only before any measurement the old unified-memory rule. The measurement (`gpu_context.cpp measureFilmV3`, called at the end of `warmFilm`): a final-tier film frame of a 1024x683 test image is rendered through the executor with each variant (direct taps, staged column tile; one compile round plus two timed rounds each, minimum), the faster one wins if it is 5 % faster, a tie keeps the old rule, and the verdict is stored in `<gpu cache dir>/film_v3_verdict_<hash of build fingerprint + app version + API + device key>.txt`, so the second start (and every start until an OS / driver / app update) skips the measurement. The log line reads `film_v3 direct X ms vs staged Y ms -> direct|staged` or `film_v3 direct (cached verdict)`. Redmi (Adreno 710, Vulkan): direct 147.8 ms vs staged 213.8 ms at 1280x853 in the first version (1024x683 now) -> direct, matching the earlier hand measurement; warm-up 3.4 s the first time (before shortening the test frame), 0.64 s with the cached verdict. The M1 result (staged) is to be confirmed by the iOS engineer with this build; nothing in the code names a vendor, a platform or a memory model. This is the first use of the warm-up micro-benchmark; the interactive-size seeding is not built.

## 27. Item 3: film self-check

There is a single call site, `warmFilm(c, note, &filmWrong)` in the probe thread, so the check runs for every API (Vulkan, GLES, Metal) that the probe thread selected, and a failing check demotes the API the same way. It does **not** run (and the log shows nothing about it) when: the GPU mode is explicit (`DAPPLE_GPU=1`: the device is created lazily on the calling thread, no probe thread, no warm-up), `DAPPLE_GPU_WARM_OFF` is set, or the film library is not available within 10 s. The line now always says why: `film check skipped: no film library` or `film check skipped: the GPU frame returned no host pixels` appear in the warm-up line instead of nothing. Also fixed: the film_black tier of the warm-up set `coarseFilm` before overwriting the options (the coarse-table path was not warmed); it is set on the options now. The iOS engineer's observation (no line on Metal) is consistent with the library not being set within 10 s of the probe on iOS or with an explicit GPU mode; with `DAPPLE_GPU=auto` and the pack path set at start-up the line appears on Metal.

## 28. Item 4: strength and kept-grain frames; item 5: the 4-5 ms "app-like" penalty explained

Per-pass GPU times of session frames are now available: `DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PASSLOG=1` makes the engine log `gpu passes: name ms; ...` per frame (Redmi, 480 px = 320x480 frame, timestamps skew by about one pass on Adreno). Fast state: film_latitude (no blend) `film_gnoise 1.08, film_h3 .22, film_v3 1.55, film_pd 1.32, film_fin 1.06, readback .18` = wall 7.1 ms; film_strength adds the normal branch `point 0.22` and wall is 7.4-7.6 ms; film_grain adds `film_point 1.32, film_grain .45, film_enc .85` and wall is 10.1-10.3 ms. **So in the fast state the strength frame costs +0.3 ms over the plain film frame and the kept-grain frame +3 ms; the 12.7-13.2 / 16.9 ms of the earlier tables were the same frames in a different device state, not extra work.** The interactive tier now leaves out the zero-mean glare noise (`film_gnoise` + its blur, std dev about 3e-4 of the display range; `DAPPLE_FILM_GLARE_NOISE_INTERACTIVE=1` keeps it): wall 7.1 -> 6.5 ms for the plain frame, no change for strength / grain; interactive parity rows 0.0164 -> 0.0204 mean dE (p99 0.05 -> 0.064), within the budget. A fused blend pass was not built (the blend costs 0.3 ms).

The 4-5 ms every frame seemed to cost in app-like mode is a **measurement artifact of the bench settle**: `DAPPLE_BENCH_SETTLE_MS=3000` makes the bench render one frame and sleep 3 s before the `still` case, and the frames after an idle gap of seconds run at 10.4-11.3 ms median in the 40-60 frame case (`still` with settle 0: 6.7 ms, with settle 3000: 11.0-11.2 ms, same binary, same run order otherwise; film_latitude 6.6 ms either way when something just ran before it). That is the GPU clock after an idle gap (the governor needs time and sustained load to ramp up), not the warm-up thread (`DAPPLE_GPU_PROBE_NICE_OFF=1`, which stops the probe thread from lowering its priority before device creation, changes nothing: still 6.5-7.0 vs 6.7-6.8 ms) and not `DAPPLE_FRAME_PROFILE`. The slow phases (21 ms for several cases in a row) are therefore the clock state of this phone; the distributions of rounds 5 and 6 that used a settle before the cases carry it. A real drag has no 3 s gap between frames, but the first 0.3-0.5 s of a drag after a pause runs at the ramp-up speed: this is the case for the keep-warm idea of round 2 (not implemented: the app's ADPF session is the sanctioned mechanism and could not be measured natively).

## 29. Not done in round 7

Item 5 (fence-and-swap upload, disk persistence of per-stock tables / calibration / grain tables, 'warm from disk'), item 6 (bands under the 16 MiB step), item 7 (interactive-size seeding and the visual-cost table), item 8 (raster prototype: no evidence for Adreno either way, closed only by measurement; the M1 report argues against it), item 9 (re-point `film_match*.cpp`). The demosaic engineer's hunks have not arrived.

# Round 8 (touch keep-warm, interactive-size seed, fence-and-swap LUT)

## 30. Item 1: the idle-gap clock, measured as the user feels it, and a keep-warm that works

Bench `dapple_drag_bench ... idlegap` (Redmi, 480 px interactive frames, a film_latitude drag of 30 frames after a pause; per-frame times of the first 10 printed; app-like probe/warm-up mode). **The slow clock after a pause does not ramp within the drag**: it stays for all 30 frames (0.2-0.6 s) at a lower level and a normal drag never lifts it, because its frames (7-11 ms with host gaps) do not keep the GPU busy enough for the governor.

| gap before the drag | no hint: first frame | frames 1-10 mean | frames 11-30 median |
|---|---|---|---|
| 1 s | 10.7-11.1 ms | 6.9-7.1 | 6.4-6.6 |
| 3 s | 17.8-21.4 | 10.6-12.4 | 10.3-11.1 |
| 10 s | 18.7-21.7 | 11.7-12.4 | 10.1-11.1 (one run of the day: 19.8-21.5, the phone's slower state) |

Keep-warm: **`dapple_session_touch_hint(session, burst_ms)`** (`c_api.cpp`, `dapple_c.h`; burst_ms < 0 = "prepare": one burst-size frame so its plans exist, call it once at idle after the first frame; 0 = no-op). On pointer-down, before the first parameter change, a background thread renders the newest interactive frame request again at 3x the long edge (`DAPPLE_HINT_SCALE`, default 3; the GPU has to be close to saturated, the same-size frame and 2x did not lift the clock: first frame 10.9 -> 10.9 and 11.2 ms medians) for `burst_ms` or until a real frame is requested; the results are dropped. It costs power only while a finger is down, ~80 ms per touch.

| gap | hint 80-120 ms, first movement >= 100 ms after touch-down: first frame | frames 1-10 mean | frames 11-30 median |
|---|---|---|---|
| 1 s | 4.7-11.4 ms | 4.1-4.8 | 3.9-4.1 |
| 3 s | 4.5-5.2 | 4.1-4.2 | 4.0-4.1 |
| 10 s | 4.9 | 4.2 | 4.0 |

So with the hint every frame of the drag is ~4 ms (top clock, below the fast state's 6.5) instead of 10-11 ms; the first frame goes 18-21 -> 4.5-5 ms. If the first movement comes sooner than the burst ends, the real frame waits for the burst frame in flight (touch-down to movement 60 ms, burst 120 ms: first frame 32-43 ms, then 3-4 ms: frames 1-10 mean 6.8-7.9 vs 10.6-12.4 without the hint), so the app should use burst_ms 80 and call the hint on pointer-down, and call the prepare form once per photo. The view API does not wrap it yet (`dapple_view` has the session: one line in the Dart pointer-down handler of the editor via `dapple_session_touch_hint`; **not wired and not run in the app**). ADPF through the app layer (PerformanceHintManager) was not built: the burst works without it and is the same mechanism on every platform; ADPF would be an additional hint where present.

## 31. Item 4: interactive-size seed and the visual cost of the interactive size

The warm-up that decides film_v3 now also times the interactive film frame (960x640 test image, up to 14 rounds until three consecutive rounds agree within 15 %, the minimum is kept) and stores **ms per megapixel** in the same cached verdict file (`film_v3_verdict_*.txt`, `<verdict> <ms/Mpx>`), exported as `"filmMsPerMpx"` in `dapple_gpu_status_json`. `AdaptiveQuality.seedFromMeasurement` (Dart, `adaptive_quality.dart`, called from `editor_engine.dart` at photo open until seeded) starts the drag at the largest rung whose predicted cost (a 3:2 frame) fits 80 % of the refresh budget and never above the display / thermal floor; the normal hysteresis moves it from there. On the Redmi the measurement gave 57.8 / 59.2 ms/Mpx in two runs and 125-127 in one run in a lower clock state (the drag bench measures 44 ms/Mpx in the fast state at 480 px): at 58 ms/Mpx and a 120 Hz budget the seed picks the 360 px rung, one rung more conservative than the fast state allows; the controller then steps up after 12 frames predicted to fit. Unit test added; **not run in the app**.

Visual cost (`dapple_drag_bench ... visualcost`, Redmi, Portra 400 with grain, halation, clarity 20, sharpen 30; the interactive frame at the ladder size upscaled bilinearly to the settled frame's size, 1080x1620 = a 1080-wide panel; CIEDE2000 against the settled frame; sharpness = mean absolute gradient of the green channel, relative to the settled frame):

| interactive long edge | dE2000 mean | p95 | p99 | sharpness vs settled |
|---|---|---|---|---|
| 360 px (240x360) | 3.03 | 9.4 | 19.4 | 47 % |
| 480 px | 2.66 | 7.6 | 16.6 | 52 % |
| 640 px | 2.37 | 6.2 | 13.9 | 57 % |
| 810 px | 2.22 | 5.4 | 12.1 | 59 % |
| 1080 px | 2.05 | 4.6 | 10.1 | 63 % |

The floor of about dE 2 and 63 % sharpness is the tier difference (no grain, no clarity / sharpen detail at the interactive tier), not the resolution: going from 360 to 810 px halves nothing (3.0 -> 2.2 mean), and the p99 of 10-19 sits on edges and grain. Where it becomes visible: a mean dE2000 above ~2 is just noticeable in a flat area, the edges at 360-480 px lose about half their contrast (47-52 %). Between 480 and 640 px the change is small; the settled frame after the finger lifts is exact.

## 32. Item 2: fence-and-swap LUT upload (done), disk persistence (not done)

`startGpuPrintLutUpdate` / `finishGpuPrintLutUpdate` (`film_gpu.cpp`, `gpu_executor.cpp ensureFilm`): an interactive frame that sees a new print table of the same size records the copy into a new LUT image, submits it with a fence and does not wait; the frame that finds the fence done swaps the binding and destroys the old image; the frame meanwhile shows the bound table. Settled frames (non-interactive) still wait and update in place; a size change or the split path still takes the in-place / full path; `DAPPLE_FILM_LUT_SYNC=1` restores the blocking update. Measurement (Redmi, 12 film drags x 60 frames after the first three frames of the run, wall `cpu_ms` incl. GPU wait, twice each): synchronous p50 8.2 / 7.6, p95 12.7 / 12.3, p99 15.8 / 17.4, 5 / 7 frames over 16.7 ms; fence-and-swap p50 8.2 / 7.9, p95 12.8 / 12.1, p99 16.2 / 17.0, 6 / 7 over 16.7 ms: **no measurable difference**: the 15-49 ms landing spikes of earlier rounds came from the LUT image being re-created on the exact 33^3 <-> coarse 17^3 size change (fixed in round 5), not from the wait; the remaining frames over 25 ms are the cold first frames of cases. The mechanism stays because a landing can no longer block the frame thread.

**Disk persistence of per-stock tables, calibration and grain tables ("warm from disk") was not built**: the FilmTables payloads carry about thirty fields and vectors; a versioned (pack hash + build fingerprint), size-capped, corruption-tolerant serializer with the measurement of second-start cost did not fit this round.

## 33. Items 3, 5, 6

Item 3 (bands under the 16 MiB step) and item 5 (re-pointing `film_match*.cpp`) were not started. Item 6 (raster prototype): not built; with the touch hint every drag frame is 4 ms at 480 px (the strength frame +0.3 ms, the kept-grain frame about +3 ms in the fast state), so no frame of the seeded size is above one refresh and the question stays closed as "not needed on this phone".

## 34. Gates

Full `check-all` in own dirs: PASS (build, ctest, golden-selfcheck, flutter analyze / test (219), jvm, video-host). Redmi Vulkan: match_cache ok, seqcheck 60 / 0 mismatches, selfcheck 148 / 0, film_gpu frames ok, coverage PASSED, film-tiling PASSED; GLES frames and film-tiling PASSED. `build/redmi/app-w9-arm64.apk`: see the report.

## 35. APK

`build/redmi/app-w9-arm64.apk` (38.3 MB, `libdapple.so` stripped 10,457,520 bytes, debug key, never installed or launched) is built from a clean `git worktree` at HEAD 7ae3b60 plus only my hunks of this round (the working tree's demosaic edits break the Android build of `libraw_decoder.cpp`: libc++ `false_type` / `true_type` unknown class names, `core/src/decoders/libraw_mosaic.inc`, not mine, and they are excluded from the APK). The release lint tasks were skipped by a temporary Gradle init script (the `CodecSelfTest.kt` lint crash), removed again; the worktree is removed. The 2 MB growth of `libdapple.so` over the wave 7 APK (8.41 MB) is in HEAD itself (wave 8b / 8c code), not in this round's hunks.

# Round 9 (touch hint in the app, film tables on disk)

## 36. Item 1: touch hint

State at the start of the turn: the committed hint is the frame-based one (`dapple_session_touch_hint`: the newest interactive request re-rendered at 3x the long edge for `burst_ms`, results dropped, ends when a real frame is requested; `burst_ms < 0` = prepare). A GPU-only "burn" variant (a separable box blur of a scratch image in 2-3 ms slices, two slices in flight so the GPU never idles) was built and measured to get a real frame pre-empted within a slice: it does not lift the clock like real film frames do (frames 6-7 ms after it, against 4 ms after the frame burst; with the slice sizes small enough to pre-empt, no lift at all) and it did not shorten the first frame, so it was removed. The warm-up burst I had added before the seed measurement (200-300 ms of 3x frames, plus raising the thread's priority and pinning it) was measured to change nothing in the result (47-51 ms/Mpx with and without) and was removed; the seed measurement now times a 480x320 frame (the cost per megapixel is not constant: 960x640 measured 58, 480x320 47-53 on the same phone), which is the only change that stayed.

First-frame times after a 3 s pause, 480 px drag, Redmi, hint burst 80 ms (frame burst, scale 3), per touch-to-move delay (no hint: 18-21 ms at every delay, frames 2-30 at 10-11 ms):

| touch-to-move | first frame with hint | frames 11-30 median |
|---|---|---|
| 0 ms | 20.6 ms (second frame 96 ms: it waited for the burst frame that was already in flight) | 4.0 |
| 30 ms | 62.1 ms | 4.1 |
| 60 ms | 30.2 ms | 4.2 |
| 100 ms | 3.6 ms | 4.1 |

A real frame cannot pre-empt GPU work already submitted (one queue, no preemption in the HAL): the worst case is the burst frame in flight (up to ~40-60 ms at the low clock) and it only happens when the finger moves within the first ~80 ms after touch-down; scales 2 and 2.5 shorten the wait (first frame 22-57 ms at 30 ms) but do not lift the clock (frames 10-11 ms). A true fix needs a lower-priority queue or a cancellable GPU frame in the HAL: not done. **The app calls the hint**: `EditorEngine.beginInteraction` (the single entry of the pad, strength strip, rulers, film map and crop handles: `editor_controller.dart` and `controls.dart` both end there) calls `DappleSession.touchHint(boundSession, 80)` on the first pointer of a drag, and the "prepare" form once per photo at the first finger-up (an interactive frame exists by then). FFI hunks, kept separate from the demosaic engineer's: `dapple_bindings.dart` (the `_TouchHint` typedefs, the `touchHint` lookup, the field), `dapple_session.dart` (`DappleSession.touchHint`), `editor_engine.dart` (`touchHintBurstMs`, `_hintPreparedFor`, the two calls). `app/test/touch_hint_test.dart` (no-op without a session / symbol, burst length range); `flutter analyze` and `flutter test` clean. Not run in the app. ADPF through the Android app layer was not added: it could not be measured here and the burst already lifts the clock.

## 37. Item 2: film tables on disk ("warm from disk")

`film_tables.cpp`: every raw / curves / print (exact and coarse) table baked by `tablesFor` or the stock worker is written to `<gpu cache dir>/film_tables/<key>.bin` from a writer thread (never the frame thread); the next process loads it instead of baking. File = magic, format version, 128-bit key, payload length, FNV-1a checksum, then all fields of the table; key = pack content hash (`film::libraryContentHash`, new: hash of the loaded pack file or profile folder) + build salt (app version + fingerprint, from `configure`) + format version + kind + stock / paper ids, warmth, tint, print EV, black, mapping, split path, adaptation, LUT size. Corrupt, truncated, wrong-version or wrong-key file = deleted and baked as before; writes go to a per-process temporary name and are renamed; directory kept under 64 MB (`DAPPLE_FILM_DISK_CAP_MB`) by deleting the least recently used files (a load touches the file); `DAPPLE_FILM_TABLE_DIR` selects a directory when the app configured none, `DAPPLE_FILM_DISK_OFF=1` disables. The **print-exposure calibrations** (`filmPrintConstant`, ~190 ms per bucket on the phone) are persisted the same way, one 28-byte checksummed file each (`film_match.cpp`). Grain plan tables are not persisted (they are built per frame size, not per stock).

Tests (`tests/engine/film_disk_tests.cpp`, ctest `film_disk.tables`; the test runs itself as child processes so every load starts with empty memory caches; passes on desktop and on the Redmi): cold run writes >= 3 files; second start loads them all (hits >= 3, 0 stores) with a **bit-identical** content checksum; a flipped byte is detected (1 corrupt, rebuilt with the same content, file replaced, next start clean); a truncated file likewise; another build salt finds nothing and writes its own files; with a 1 MB cap two stocks leave the directory under the cap (and larger without it); four processes baking the same stock into one directory at once leave only whole files, no temporary files, identical content.

Time of the first exact (settled, 1080 px) frame of each stock in a fresh process, `dapple_drag_bench ... firsts` (the first *interactive* frame of a second or third stock is a few ms because it shows the previous stock while the new one is prepared, round 6, so it is not a measure of the stock being ready):

| | no disk | cold (empty dir) | second start | third start |
|---|---|---|---|---|
| Redmi portra_400 (first stock: also device, pipelines) | 512 ms | 562 | 512 | 532 |
| Redmi ektar_100 | 722 | 1010 | **157** | **113** |
| Redmi velvia_100 | 273 | 418 | **118** | **95** |
| Redmi first interactive frame of the process (portra) | 1950 | 1932 | 772 | 764 |
| desktop (RTX) portra_400 | | 192 | **25** | |
| desktop ektar_100 | | 518 | **10** | |
| desktop velvia_100 | | 40 | **11** | |

Cold with the disk write costs 0-290 ms more than no disk on the phone (the table copies and the writer thread compete with the frame; not measured separately); the first stock of a process still pays about 500 ms for the device, pipelines and plan upload which the disk does not hold. The directory after three stocks: 30 files, 14 MB.

## 38. Items 3-5 of round 9, and the shared device

Not started: the 16 MiB bands, the re-pointing of `film_match*.cpp`. Seed: re-measured at 480x320: 47-53 ms/Mpx on three runs (at 58 before at 960x640); the seed picks the rung from it as in round 8. `hal::Device* sharedDevice()` for the demosaic node: not added (the provider hands out a Context, not a device; it needs a lease protocol with the presenter's device lease, more than a one-line change).

## 39. Gates and APK (round 9)

Full `check-all` in own dirs: PASS (build, ctest incl. `film_disk.tables`, golden-selfcheck, flutter analyze / test (221), jvm, video-host). Redmi Vulkan: match_cache ok, seqcheck 60 / 0 mismatches, selfcheck 148 / 0, film_gpu frames ok, coverage PASSED, film-tiling PASSED, film_disk ok; GLES frames and film-tiling PASSED. `build/redmi/app-w10-arm64.apk` (36.8 MB, debug key, not installed) = HEAD 42f107b + this round's changes, release lint tasks skipped by a temporary Gradle init script (removed).
