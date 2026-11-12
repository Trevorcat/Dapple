# OPPO Find N5 (PLP110) device results, 2026-11-12 (partial: the owner unplugged the phone mid-session)

Method as in `pixel5.md`. Raw logs: `build/n5/logs/`, Main10 logs: `build/n5/main10/`. Binaries are the **existing** `build/pixel5-arm64-gpu` ones (built 11:08, not rebuilt, so they do not include other engineers' later work and have no GPU film path). Test image: the synthetic 6000x4000 JPEG from `build/pixel5/mk_synth.py` plus a synthetic 9504x6336 one (`build/n5/mk_synth60.py`). Everything is *measured* unless marked *estimate*.

**Not done** (phone unplugged): Main10 runs after the first two, ffprobe/decode-back of any Main10 output, the app part (APK install, codec diagnostics screenshots, Motion Photo generation), cleanup. See section 8.

## 1. Device facts

| | |
|---|---|
| Model / OS | OPPO Find N5 (PLP110), Android 17 (SDK 37), ColorOS V17.0.0 (`PLP110_17.0.0.109(CN01)`), security patch 2026-09-01 |
| SoC | `ro.soc.model=SM8850`, board `canoe`: this is the Snapdragon **8 Elite Gen 5**, not the 8 Elite assumed in the targets |
| RAM | 15.5 GB total (4.4 GB available at start, other apps resident), 12 GB swap |
| CPU | **7 cores present and online (cpu0-6)**, all ARM part 0x002 (Oryon-class): cpu0-4 max 3.63 GHz, cpu5-6 max 4.61 GHz, governor `walt`. `hardware_concurrency()` = 7, so the engine runs 7 threads (cpu7 is absent on this unit/firmware) |
| GPU | Adreno **840**, vendor 0x5143, Qualcomm driver build 5bf0b7317d (2026-07-31), **Vulkan 1.4.295** (feature `vulkan.version=4210688`, `vulkan.level=1`, deqp 132711169), GLES 3.2 (`ro.opengles.version=196610`, AEP) |
| GPU limits | maxImage2D 16384, maxImage3D 2048, shared 32 KB, maxInvocations 1024, subgroup 64 (ops 0x6ff), fp16 arithmetic 1, storage16 1, **linear filter on RGBA32F 1** (Pixel: 0), timestamps 1 |
| Memory types | one 15139 MB device-local heap (+4096 MB, +14 MB lazy). Host-visible types: 3 `HOST_CACHED`, 4 and 5 `HOST_COHERENT`, **6 and 7 `HOST_COHERENT|HOST_CACHED`**. Cached+coherent exists in several types, readback is much cheaper than on the Pixel (section 3) |
| Display | inner 2248x2480 (modes 120 / 90 / 60 Hz; was running 90 Hz), outer 1140x2616 (120 / 90 / 60 Hz, was 120 Hz). Supported rates incl. 45, 40, 30, 24, 22.5, 20. HDR types 1-4, peak 2200 nits. density 520. `presDeadline` 14.1 ms (inner) / 11.3 ms (outer) |
| Thermal | `Thermal Status` 0 throughout all runs. Battery 82 %, USB powered (not charging fast), skin 30 -> 33.6 C during the sustained run |
| Self-test | `point+geometry: mean dE 0.040 p99 0.75 max 1.46 ok; spatial: mean dE 0.076 p99 0.86 max 1.45 ok` (identical to the Pixel to the printed digits). `gpu-bench --info`: 0.62 s with the self-test, 0.31 s without: **self-test ~310 ms** warm (target <= 300 ms, borderline, same as Pixel) |

## 2. GPU correctness

* `dapple_gpu_tests`: **PASSED (0 failures)**, 9.05 s, RSS 439 MB. Per-stage dE2000 tables identical to the Pixel 5 / RTX / SwiftShader (point 640x448 mean 0.0400 p99 0.804 max 1.66; all spatial + geometry 0.0725 / 0.87 / 1.74-1.85; fp16 source budget 0.0206 / 0.533 / 1.43). Tile seams (tile 128-1024, resident and streamed): 0/255 differing pixels.
* **No driver problem seen** (the Adreno 620 shared-array spec-constant bug does not appear: spatial self-test is correct). GPU vs CPU baked path at 1600 px: point mean 0.0134 / p99 0.368 / max 1.44, interactive 0.0307 / 0.456 / 1.25, full 0.0353 / 0.472 / 1.53; 24 MP interactive 0.0305 / 0.455 / 1.61, full and tiled 0.0357 / 0.474 / 1.63 (budget mean 0.1, p99 1.0).
* Caveat: the per-pass table shows `point 0.01 ms`, which is impossible for 1.7 MP (would be 2.7 TB/s). Per-pass timestamp attribution on the 840 looks wrong (the point pass time seems folded into neighbouring entries); the per-frame GPU totals are plausible (point-only frame 0.39 ms = ~140 GB/s over a 27 MB read+write). Do not use the per-pass split on this device.

## 3. GPU vs CPU timings (`gpu-bench`, median of 5-20, source resident)

### 3.1 1600 px (1600x1067, 1.7 MP)

| variant | CPU baked path (7 thr) | GPU wall | GPU time | readback copy-out | speed-up | Pixel 5 GPU wall (GPU time) |
|---|---|---|---|---|---|---|
| point-wise only | 15.3-16.3 ms | **2.7-3.1 ms** | 0.39 | 1.7-2.0 | 4.9-6.2x | 19.4 (8.1) |
| interactive (no NR/grain) | 35.5-43.9 | **5.7-8.2 ms** | **3.33** | 1.7-4.0 | 4.3-7.7x | 58.0 (45.3) |
| full quality | 65.9-69.1 | **10.7-10.8 ms** | **5.8-5.9** | 3.8-4.1 | 6.1-6.5x | 86.2 (72.7) |

The copy-out varies 1.7 ms vs 4.0 ms between runs (core placement of the waiting thread, as on the Pixel). First frame (source + lattice upload + pipeline build): point 41-44 ms, interactive 19-22 ms, full 18 ms (pipelines already compiled after the first stage). CPU lattice bake 41^3: 9-15 ms (Pixel 22-27).

### 3.2 24 MP (6000x4000)

| variant | CPU | GPU wall | GPU time | copy-out |
|---|---|---|---|---|
| point-wise only | 209 ms | 41.6-44.9 | 5.8 | 35-39 (plain memcpy of the same 96 MB: 3.8 ms) |
| interactive | 835 | 114.6-118.9 | 74.5-74.6 | 39-44 |
| full quality | 1092 | 160.8-161.6 | 119.7-120.7 | 39.5-40 |
| preview 1600 from resident 24 MP source | | first 60 ms, later 8.5 ms | | |

Tiled export, 24 MP, all spatial stages: tile 2048: streamed 382 ms (62.9 MP/s), resident 309 ms (77.7 MP/s), pool 988 MB; tile 1024: streamed 643 ms (37.3 MP/s), resident 324 ms (74.2 MP/s), pool 955 MB. CPU full-frame reference 1.0-1.2 s. Pixel 5: 2.7-2.8 s (8.5-8.9 MP/s). Bench process VmHWM 2.0 GB (float source + CPU outputs; not the engine pool).

**60 MP (9504x6336, synthetic) tiled export, streamed source**: tile 1024: **1.106 s** (54.4 MP/s), GPU 769 ms, pool **311 MB**; tile 2048: 1.004 s (60.0 MP/s), pool 553 MB. Process peak RSS 937 MB (dominated by the 722 MB float source). The "<= 10 s" target is met by 9x. The 500 MB export-memory target holds for tile 1024 only if the float source is not counted (needs `SourceStore`, section 1.3).

Observation: the pool printed for tile 1024 at 24 MP is 955 MB (it grows to the 1 GB budget when the source is resident), at 60 MP 311 MB, so "pool" is not simply proportional to tile size.

### 3.3 Sustained (12 chunks of 400 full-quality 1600 px GPU frames, ~3 min; each chunk also runs the point/interactive variants and the CPU reference path, so the CPUs were loaded too; `build/n5/logs/sustain.log`)

| chunk | time | GPU wall | GPU time |
|---|---|---|---|
| 1-4 | 13:01:45 - 13:02:25 | 10.67, 10.74, 10.82, 10.87 | 5.78-5.80 |
| 5 | 13:02:37 | 16.04 | 10.71 |
| 6-12 | 13:02:52 - 13:04:31 | 14.8-16.2 | 9.69-9.84 |

**The GPU throttles after about 50-60 s of continuous load: GPU time steps from 5.8 to 9.7 ms (-40 % throughput, +68 % time) and stays there for the remaining two minutes**, while `Thermal Status` stays 0 and GPU/CPU sensors read 47-56 C (peak 56 C at the step, then 47-48 C: it is a vendor DVFS cap, not the Android thermal status; skin 31.4 -> 33.6 C). Even the throttled state (14.9 ms wall) is 5.8x faster than the Pixel 5 (88 ms). The 120 Hz budget therefore cannot rely on the cold numbers for continuous full-quality use. Not isolated: whether the CPU load in the same process contributed to the cap.

### 3.4 Comparison with the targets (section 1.3) and the Pixel

| item | target / estimate | Pixel 5 | Find N5 | verdict |
|---|---|---|---|---|
| GPU time, 1600 px interactive graph | <= 4 ms (est. 4-9 ms phone) | 45 ms | **3.3 ms** (cold), 5-6 ms if throttled *(estimate from the full-quality ratio)* | met cold |
| GPU time, 1600 px full quality | 2.35 ms desktop | 72.7 | 5.8 (9.7 throttled) | 2.5x desktop (RTX 4060 Ti 2.35 ms) |
| Engine frame <= 8.3 ms (120 Hz), interactive incl. readback | 8.3 ms | 58 | **5.7-8.2 ms** (borderline, depends on copy-out 1.7 vs 4.0 ms) | met only with the fast copy-out; point-only drag 2.7-3.1 ms |
| Engine frame, full quality incl. readback | | 86 | 10.7 ms | misses 8.3 ms (fine for the final frame) |
| CPU critical path <= 2 ms | 2 ms | | not measured (no app run); the CPU lattice bake alone is 9-15 ms, so it must be cached/background-baked for 120 Hz | not met by the CPU bake |
| Readback cost | | 10-14 ms @1.7 MP, 120 ms @24 MP | **1.7-4.0 ms @1.7 MP, 35-44 ms @24 MP** (3-8x better; cached+coherent types 6/7 exist) | still not free; zero-copy path still worth it |
| Export 60 MP GPU | <= 10 s | ~7 s *est.* | **1.0-1.1 s** | met |
| Self-test | <= 300 ms | ~300 | ~310 | borderline |
| First frame | | 62-141 ms | 18-44 ms | |

## 4. CPU baselines (7 threads; `build/n5/logs/edit_bench.log`, `film_render_bench.log`)

`edit-bench --scenario 1 --quick` (synthetic 24 MP JPEG, session 2048 long edge), ms, Pixel 5 in brackets:

| stage | N5 | (Pixel) |
|---|---|---|
| interactive 1080, exposure drag (re-bake 41^3) | 16.4 | (23.7) |
| interactive 1080, saturation drag | 20.4 | (32.6) |
| interactive 1080, clarity drag (lattice cached) | 16.0 | (27.1) |
| interactive 1600, exposure drag | 30.1 | (36.5) |
| final 1600, params unchanged | 19.7 | (28.6) |
| final 1600 after point change (bake 65^3) | 32.1 | (38.5) |
| reference (exact) 1600 + analysis | 94.2 | (112.5) |
| bake 41^3 / 49^3 / 65^3 | 9.7 / 8.8 / 12.9 | (20.2 / 12.9 / 18.8) |
| lattice pass 1067x1600 / interactive spatial 1080 / 1080 only | 19.2 / 8.9 / 8.9 | (28.7 / 13.8 / 13.3) |

The CPU is only 1.1-1.5x faster than the Pixel 5 in this engine despite Oryon cores at 3.6-4.6 GHz (memory-bound passes and thread scaling; *not investigated*). With only 7 online cores and warm DVFS state after the sustained run the numbers are a lower bound of what a cold device would give.

`film-render-bench --pack build/film/dapple_new.dfp --runs 3 --mp 2` (Portra 400, 1732x1154 source), ms:

| | normal | film cold | film cached | (Pixel film cached) |
|---|---|---|---|---|
| "26 MP" no spatial | 39.4 | 42.9 | 34.8 | (56.6) |
| "26 MP" all effects | 80.0 | 174.6 | 163.7 | (318.5) |
| 1600 preview, no spatial | 46.8 | 44.1 | 36.0 | (65.7) |
| 1600 preview, all effects | 73.5 | 139.4 | 133.4 | (287.0) |
| 1600 preview, interactive quality | 72.2 | 108.3 | 99.9 | (205.6) |
| 1600 interactive frame dragging print warmth (bake + render) | 123.3 | | | (243.5) |

**Film on the N5 is CPU-only in this build and costs 100-133 ms per 1600 px frame, 15x the 8.3 ms budget.** Film is the item that most needs a GPU path on this class: the GPU colour/spatial graph without film is 3-6 ms, so film would dominate the frame by two orders of magnitude. Not measured: `film-bench`, GPU film.

## 5. Where the N5 stands relative to the emulator profiles

The Pixel 5 sat near the `mid` emulator profile. The N5 CPU is faster than the `n5` throttle proxy in per-core terms but the engine is memory-bound, so the CPU rows above are the ground truth: 16-20 ms for a 1080 colour drag on the CPU (TR n5 v3 reported p50 39 ms for a whole drag update incl. UI). The GPU removes 4-8x of the engine time on this device (versus 1.7x on the Pixel 5): the Adreno 840 changes the cost-benefit of the GPU path from "marginal" to "decisive".

## 6. HEVC Main10 encode (partial)

Capability (from `dumpsys media.player`, `build/n5/main10/media_player_full.txt`): HEVC encoders `c2.qti.hevc.encoder` (alias `OMX.qcom.video.encoder.hevc`, HW, size 96x96-8192x8192, up to 1080p 240 fps / 4K 120 fps points), `.encoder.cq` (HW, constant quality, 128x128-512x512), `.encoder.hdr` (HW, 128x128-4096x4096, `feature-hdr-editing`), `c2.android.hevc.encoder` (software, Main/MainStill only). **All three HW encoders advertise Main, MainStill, Main10, Main10HDR10, Main10HDR10Plus, Main10Still (High 6.2)** and colour formats Surface, YUV420Flexible/Planar/SemiPlanar/PackedPlanar/PackedSemiPlanar, **32bitABGR2101010 (0x7f00aaa2)** and **YUVP010 (0x36)**. (The Pixel 5 advertised Main only.) Also `c2.qti.mvhevc.encoder(.hdr)` for MV-HEVC.

Runs with the `m10` tool (1920x1080, 30 fps, 90 frames, 10-bit gradient in P010, `profile=2`, 20 Mb/s), default encoder `c2.qti.hevc.encoder`:

| run | result |
|---|---|
| BT.709 SDR, P010 buffer input | `configure` OK, input format shows colour-format 54, stride 3840, slice-height 1080; **`FAIL timeout sent=17 got=0`**: the encoder accepts 17 input buffers and produces **no output** (same failure signature as the Pixel 5) |
| BT.2020 HLG, P010 buffer input | **same: `FAIL timeout sent=17 got=0`** |
| BT.709 surface input, then `c2.qti.hevc.encoder.hdr` with buffer input (709 and HLG) | **started but the session was cut before results were read**; unknown |

So on the N5 the declared Main10 profile on the main HW encoder did **not** deliver a stream with P010 buffer input in 2 of 2 runs; the ffprobe / `yuv420p10le` / luma-level check was never possible. The listed profile therefore cannot be trusted: the app's `findMain10Encoder` (profile + colour format check) would select this encoder, produce nothing, and wait for the full deadline. This confirms the Pixel review items (output-profile check after `INFO_OUTPUT_FORMAT_CHANGED`, a short no-output watchdog of ~3 s / 8 queued frames, probe-encode 2 frames per `deviceKey`) as **required** rather than optional. logcat was cleared before the runs but not read (phone unplugged), so the driver message for the failure is unknown. Still to try on this device: `c2.qti.hevc.encoder.hdr` (the likelier path for 10-bit), the Surface/ABGR2101010 route, Main10HDR10 profile value 0x1000.

## 7. Recommendations for this class (flagship Adreno 8xx)

* **GPU default policy**: GPU on by default after the self-test passes (4-8x faster than the CPU path here; every correctness check matches the desktop). Keep the CPU fallback and persist the `deviceKey` with the self-test result.
* **Interactive tier**: the 1600 px GPU graph is 3.3 ms of GPU time and 5.7-8.2 ms wall, so T0 can be 1600 px (or ~2 MP, viewport-sized) on this class instead of 1080; a 1080 tier would be < 3 ms. Keep the 1080/810/640/480 ladder for lower-class GPUs, chosen by the measured GPU time of the self-test, not by model.
* **Final/full quality frame**: 10.7 ms wall cold, 15 ms once the GPU is capped; fine for finger-up, not for 120 Hz drags: drag with the interactive graph, resolve with the full one.
* **Sustained work**: budget for the ~40 % GPU throttle after about one minute of continuous load (a long editing session with constant drags is not continuous load, but export + preview together might be). Re-measure with a GPU-only process (this soak also loaded the CPUs).
* **Export tile size**: 2048 is fastest (60 MP in 1.0 s, pool 553 MB), 1024 gives pool 311 MB for 1.1 s: with 15 GB RAM either works; choose 1024 to stay inside the 500 MB export budget, the cost is 10 %.
* **Readback**: 1.7-4 ms at 1.7 MP is acceptable at 5.7-8 ms frames but makes up to half of the interactive wall time; the zero-copy display path still pays (frame 3.3 ms instead of 5.7-8.2). The host-visible cached types 6/7 are used; pinning to the prime cores keeps copy-out near 1.7 ms.
* **Film**: needs the GPU film node; 100-133 ms CPU per frame is the largest remaining item on this device.
* **CPU critical path**: the 41^3 bake (10-15 ms) exceeds the 2 ms budget; cache/background-bake as planned (item 2 of the risks).
* **Video**: do not trust `profileLevels` for Main10 on Qualcomm encoders; probe-encode at runtime with a short watchdog and fall back to H.264/Main.

## 8. Not measured, and what is left on the phone

Not measured: Main10 results beyond the first two runs (no ffprobe, no decode-back, no luma-level count), app install / codec diagnostics screenshots / 全部重测 / Motion Photo generation from the 10-bit fixtures, display-path frame pacing at 120 Hz, power, dedicated thermal soak with a GPU-only process, GPU film, the CPU-clock/thread-scaling reason for the small CPU gain, simpleperf.

**Left on the device `3B163700HQ200000` for cleanup on reconnect** (nothing was pushed to `/sdcard`, no MediaStore rows, no Motion Photos were generated, the app was NOT installed or touched by this session):

* `/data/local/tmp/dapple/` : `dapple`, `libdapple.so`, `dapple_gpu_tests`, `synth_24mp.jpg`, `synth_60mp.jpg`, `dapple_new.dfp`, and `main10/` with `m10` plus whatever the interrupted m10 runs wrote there (`m709_buf.mp4`, `mhlg_buf.mp4`, possibly partial `m709_surf.mp4`, `m709_hdrenc.mp4`, `mhlg_hdrenc.mp4`). Remove with `adb -s 3B163700HQ200000 shell rm -rf /data/local/tmp/dapple`.
* A background `m10` process loop may still have been running when the cable was pulled; it is killed by the device or finishes with a timeout (`pkill m10` if it persists).
* `/sdcard/Download/dapple_test/` : does not exist (never created).
* Before/after state: before = no `/data/local/tmp/dapple`; after = directory above present. App state unchanged. Nothing else modified (no settings, no rotation changes).

## Wave 4 (2026-11-12, tree at HEAD dc5b7a1 + other engineers' uncommitted edits; arm64 NDK r28, android-29, Release, DAPPLE_GPU=ON, DAPPLE_GLES=ON)

Logs: `build/n5/logs/w4_*.log`; APK `build/n5/app-release-arm64-wave4.apk` (release, `-Pdapple.gpu=true`; gradle has no GLES switch, so the APK has Vulkan only). Binaries in `build/n5/bin-arm64/bin`. The device was in the folded state (outer 1140x2616 active), **screen locked (secure keyguard)**.

**Blocked: all UI steps (4, 6, and the app part of 5).** The phone is secure-locked; I did not and cannot unlock it. The new APK also could not be installed: `INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match` (installed build, versionName 1.0.0, updated 2026-11-11 23:25, was signed with another key). Fixing it needs an uninstall, which would delete the owner's data, so I left the installed app untouched. Not run: present checklist, drag/rotation/trim/kill tests, DAPPLE_PRESENT_ADOPT / GPU_VIEW A/B, engine A/B exports, codec diagnostics, Main10 / .hdr encode, in-app camera. Nothing was created on the phone's storage by these.

### Native results (Adreno 840, Vulkan unless noted)

| item | result |
|---|---|
| Vulkan self-test | point+geometry mean dE 0.040 p99 0.75; spatial 0.076 / 0.86; box scan 6.8e-06: ok |
| `dapple_gpu_tests --quick` | PASSED, 0 failures, 9.5 s (also with `DAPPLE_GPU_WG=128`: PASSED) |
| `film_gpu.parity` (GPU vs CPU, arm64/NEON) | ok, 13.4 s; point chain mean 0.015 p99 0.045 max 0.105; final tier grain off 0.036 / 0.11 / 0.33; grain per pixel 0.033 / 0.10 / 0.28; interactive grain drag 0.026 / 0.08 / 0.23. Same numbers under `DAPPLE_GPU_API=gles` |
| `film_gpu.frames` (whole pipeline vs CPU) | ok; 1536x1024 all effects mean 0.071 p99 0.87 max 1.85; interactive 0.050 / 0.84 / 1.73; grain drag 0.070 / 0.84 / 1.79. Same under GLES |
| `dapple_gpu_engine_tests` hal / box / sched / select / external | all PASSED. select: unsupported, transient x2, device lost (rebuild), second loss each give a CPU frame for that frame; afterwards the engine is on `gles31` (Vulkan -> GLES demotion works; the CPU-only end state is by frame fallback, not a permanent state). external: 0 differing bytes |

### Film frame times (`dapple_film_gpu_tests bench`, median of 10)

| size / tier | Vulkan GPU ms | wall incl. readback | GLES forced (GPU ms / wall) | WG=128 (GPU / wall) |
|---|---|---|---|---|
| 1600 final | 37.8 | 93.7 (run 2: n/a) | 37.0 / 61.6 | 43.2 / 69.5 |
| 1600 interactive | 27.5 (run 2: 34.8) | 63.3 (run 2: 92.3) | 26.5 / 55.6 | 31.5 / 55.2 |
| 1600 interactive, grain drag | 29.6 (run 2: 29.6) | 66.5 (run 2: 64.4) | 29.2 / 68.0 | 33.3 / 56.3 |
| 1080 interactive | 15.2 (run 2: 14.7) | 43.2 (run 2: 29.8) | 14.6 / 35.0 | 16.6 / 29.9 |
| 3600 (8.6 MP) final | 163 | 334 | 161 / 294 | 188 / 317 |

**Film frame misses the 8.3 ms (120 Hz) budget by 3x on GPU time and 7x on wall time at 1600 px interactive (27-37 ms GPU, 55-92 ms wall); even 1080 px is 15 ms GPU.** A later run after 60 s idle was slower again (1600 interactive 37 ms GPU, wall 150 ms; screen was off, other apps resident), so the run-to-run spread on this phone is large (+-30 %). GLES is as fast as Vulkan for film (GPU timestamps are not available on GLES, so its "GPU ms" is not comparable). WG=128 is 10-15 % slower than the default 16x16. Compared with the earlier CPU film numbers (100-133 ms per 1600 frame) the GPU is 2-4x faster but not near real time.

### Colour/spatial graph without film (`gpu-bench`, 24 MP JPEG source)

| | CPU baked | GPU wall | GPU time | copy-out |
|---|---|---|---|---|
| 1600 px interactive | 105 ms | 11.6 | 6.3 | 3.9 |
| 1600 px full | 84 | 14.9 | 9.7 | 3.8 |
| 24 MP interactive | 5688 (outlier) / 1330 | 189 | 138 | 46.6 |
| 24 MP full | 1625 / 2779 | 263-305 | 213-260 | 43-46 |
| 24 MP tiled export, all stages, tile 1024 | CPU ref 1417 | streamed 690 / resident 480 (34.8 / 50.0 MP/s), pool 467 MB | | |
| tile 2048 | CPU ref 1449 | streamed 607 / resident 453 (39.5 / 53.0 MP/s), pool 501 MB | | |

These are 30-60 % slower than the earlier wave (1600 full 10.7 ms, 24 MP tiled 309-382 ms): the phone was already in its capped state when the run started (full-quality GPU time 9.7 ms from the first chunk).

### Thermal (8 chunks x 400 full-quality frames, 15:18:55 - 15:21, `w4_sustain.log`)
GPU time 9.57-10.86 ms in every chunk, wall 14.4-16.7 ms, no further step over ~2.5 min. `Thermal Status` 0, GPU0 sensor 41.7 C at the end. So the "throttle step after ~55 s" was not reproduced as a step, because the device started already in the slow state; the cap (9.7 vs 5.8 ms) is the sustained value.

### Bugs / observations for the lead
1. Film GPU frame is 3-7x over the 120 Hz budget at 1600 px (see above); needs a pass-count / fusion reduction (14-20 passes at 640x448 interactive, 36-53 for all effects).
2. GLES timestamps report 0 (`timestamps 0`), so per-frame GPU ms are not available under GLES.
3. `INSTALL_FAILED_UPDATE_INCOMPATIBLE`: the release APK is signed with a different key than the build on the phone; owner must uninstall (losing app data) or we need the original keystore.
4. Per-pass table on the 840 still unreliable (point pass 0.01-0.02 ms).

Cleanup done: `/data/local/tmp/dapple` removed, no `debug.dapple.*` props were set, app left installed (the old build).

## N5 wave 4: app (2026-11-12)

APK `build/n5/app-w4-arm64.apk` (release, arm64, `-Pdapple.gpu=true`, package `com.dapple.dapple`, installed by the lead after the owner removed the old build; first `adb install` while locked failed with `Failure [-99]`). Folded outer display 1140x2616 (inner not tested). Gradle: `-Pdapple.idSuffix=.dev` added (default empty, label via manifest `${appLabel}`), not used in the end. Logs `build/n5/logs/w4app_logcat_*.log`, screenshots `build/n5/w4app_*.png`. No bundled sample photo exists; the editor was tested on a photo shot with the in-app camera. The owner took over the phone after about 15:56, so the rest was not run.

| item | result |
|---|---|
| GPU off (default: no `debug.dapple.gpu`), exposure/contrast drag, 810x1080 frames | `dapple_view: shown 90 ext 0 host 90 hostBytes 323289600 tooSmall 0 refused 89`, render 19-34 ms, hostBytes grows 3.5 MB per frame |
| `debug.dapple.gpu=1`, app restarted, same drag | `dapple_present: device Adreno (TM) 840 type 1 api 4210983 family 0 (shared with the engine)`; no "private device" line (queue family 0 presents); `swapchain 1200x1600 fmt 37 (R8G8B8A8_UNORM) images 10 transform 1`; `shown 60 ext 59 host 1 hostBytes 3499200 tooSmall 0 refused 0`, hostBytes flat; render 17.8 ms (exposure/contrast), present 0.6-0.8 ms, latency 28 ms |
| grain drag (effects tab, no film) | `shown 180-210 ext 179-209 host 1`, render 1.9-3.2 ms, latency 13-14 ms, picture correct |
| style 暖阳 + style strength drag to 84 % | `shown 240 ext 239 host 1`, render 31.7 ms, latency 42 ms (screen: warm look, correct) |
| film N200A (负片) selected, film strength drag to 84 % | render 28.3 ms (810x1080), ema 34.6 ms, `ext 269 host 1`. Next counter line (film, `last 360x480`): `render 119.77 ms present 0.28 ms latency 164.17 ms ... ema render 110.38`, `ext 299 host 1`. No log line says GPU or CPU executor for the frame; frames are counted `ext` (zero-copy present) either way. Film frames are 28-120 ms, far over the 8.3 ms budget. Picture looks like film (contrast, warm/dark) |
| gfxinfo (Flutter UI), 114 frames | janky 4 (3.5 %), p50 5 ms, p90 7 ms, p99 30 ms; gpu p99 19 ms. meminfo TOTAL 407 MB PSS, graphics 205 MB |
| in-app camera, one shot | saved `Pictures/Dapple/DAP_20261112_154612.dng` 25 195 852 B and `.jpg` 3 036 174 B (RAW + JPEG; the badge text was not screenshotted); editor opened on it; edge darkening visible in the first editor screenshot (`w4app_03_capture.png`), not judged as lens shading vs app vignette. Shots NOT deleted: the owner is editing them |
| 2c background / rotate / trim, 2d A/B props (engine off vs full, exports, GPU view, adopt), 2e codec diagnostics | not run (owner took the phone; no `DappleCodec` lines appear in logcat without opening the diagnostics page) |

Notes: `DAPPLE_GPU_VIEW` is a `--dart-define` (build-time) and `DAPPLE_PRESENT_ADOPT` an env var read by `vk_presenter.cpp`; neither can be flipped on the phone with a property. The only runtime props are `debug.dapple.gpu`, `debug.dapple.gpu_api` and `debug.dapple.engine`. The engine logs no per-frame executor line. `debug.dapple.gpu` is reset to empty, so the app runs GPU-off after its next restart. One extra tap/swipe sequence from a command that was stopped late may have reached the app at about 15:56. Early in the session the adb link to the N5 dropped three times (`offline`, recovered with `adb reconnect offline`; one `adb kill-server` was run on this PC).


---

## 9. Film GPU frame, wave 5 perf session (2026-11-12, film perf engineer; binaries `build/w5-perf-arm64`, arm64 NDK r28, Release; analysis in `pipeline-architecture.md` section 25)

Only native binaries pushed to `/data/local/tmp/dapple` and run through `adb shell` (no input, no app, no settings). State: Adreno 840, Vulkan 1.4.295, `Thermal Status` 0 before each run, `taskset 60` (CPUs 5-6), phone in a faster state than in the earlier sessions (see 25.2). The phone went off adb after the runs below: **nothing later was measured on it**, and **`/data/local/tmp/dapple/` (`dapple_film_gpu_tests`, `libdapple.so`, `dapple_gpu_tests`, `film/dapple.dfp`, ~120 MB) is still on the device** because it could not be reached to clean up (`adb -s 3B163700HQ200000 shell rm -r /data/local/tmp/dapple`).

### 9.1 The timestamp problem and its fix

Plain per-pass timestamps (`DAPPLE_GPU_PROFILE=1`) and even one with a full barrier before each timestamp (`DAPPLE_GPU_PROF_SERIAL=1`) are wrong on the 840: 810x1080 interactive, serial only: `film_point` 0.006 ms, `film_gnoise` 0.30, `film_h3` 0.06 / 1.18, `film_v3` 0.40 / **4.44**, second `film_point` 1.51, `film_pd` 0.12, sum 8.49 ms. With every dispatch repeated 20 times inside its timestamp interval (`DAPPLE_GPU_PROF_REPEAT=20`, ms per single run) the table is consistent with the un-instrumented frame (8.54 ms against 8.48 ms):

| # | pass | ms (staged `film_v3`) | ms (direct `film_v3`) | nominal MB read / written |
|---|---|---|---|---|
| 0 | film_point (exposure) | 0.281 | 0.281 | 13.9 / 7.3 |
| 1 | film_gnoise | 0.076 | 0.077 | 0 / 3.7 |
| 2 | film_h3 (4 comps, writes 4 RGBA16F planes) | 1.122 | 1.122 | 7.3 / 29.3 |
| 3 | film_h3 (3 comps) | 0.440 | 0.440 | 7.3 / 22.0 |
| 4 | film_v3 (4 comps) | 4.236 | **1.897** | 43.9 / 7.3 |
| 5 | film_v3 (3 comps) | 1.654 | **0.720** | 36.6 / 7.3 |
| 6 | film_point (develop) | 0.176 | 0.136 | 7.9 / 7.3 |
| 7 | film_pd (print, decode, glare) | 0.189 | 0.189 | 11.6 / 7.3 |
| 8 | film_enc | 0.099 | 0.099 | 14.6 / 7.3 |
| 9 | readback copy | 0.269 | 0.263 | |
| | sum | 8.54 | 5.22 | 143 / 99 (all passes) |

Desktop (RTX 4060 Ti), same case, serial timestamps: 1.6 ms total, `film_v3` 0.34 + 0.14 ms (3-5 % slower with direct taps).

### 9.2 Frame GPU time, non-instrumented (`bench`, median of 10, timestamps only, no serial / repeat; `GPU` includes the readback copy; "wall" = record + submit + wait + half-float readback conversion in the test harness)

| case (Portra 400) | staged `film_v3` GPU / wall (ms) | direct `film_v3` GPU / wall (ms) |
|---|---|---|
| 1600 px final, all effects | 19.85 / 53.6 | **14.98** / 48.2 |
| 1600 px final + highlight boost | 20.42 / 56.5 | 15.56 / 48.4 |
| 1600 px final + enlarger diffusion | 40.60 / 74.9 | 27.92 / 61.2 |
| 1600 px interactive | 14.12 / 48.1 | 9.78 / 38.6 |
| 1600 px interactive, grain drag | 15.24 / 48.6 | 10.91 / 41.0 |
| 1080x720 interactive | 7.48 / 23.9 | 4.57 / 20.1 |
| 810x1080 interactive | 8.48 / 26.1 | **5.16** / 21.9 |
| 810x1080 final | 13.33 / 34.4 | 9.07 / 27.1 |
| 3600x2400 final | 97.02 / 193 | 81.49 / 186 |

The "wall" column is the test harness (it converts the RGBA16F readback to float on one core), not an app number.

### 9.3 Not measured on the phone (phone off adb)

The develop + print fusion, the interactive component merge (pipeline-architecture 25.3), the `ensureFilm` table-cache fix (desktop `--film-interactive`: still frame 14.5 -> 3.9 ms), the app frame time, and a re-run of `film_gpu.parity` / `film_gpu.frames` / `gpu_tests --film-tiling` on the device. Rerun on reconnect: `adb -s 3B163700HQ200000 push build/w5-perf-arm64/bin/{dapple_film_gpu_tests,dapple_gpu_tests,libdapple.so} /data/local/tmp/dapple/`, then `DAPPLE_FILM_DATA=/data/local/tmp/dapple/film DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PASSLOG=1 DAPPLE_GPU_PROF_REPEAT=20 taskset 60 ./dapple_film_gpu_tests bench` and `./dapple_gpu_tests --film-interactive 1080`.
