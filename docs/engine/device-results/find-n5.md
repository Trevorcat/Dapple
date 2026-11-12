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
