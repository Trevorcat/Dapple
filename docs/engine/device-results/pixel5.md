# Pixel 5 (redfin) device results, 2026-11-12

First measurement of the render engine on a real phone (borrowed, temporary). Raw logs: `build/pixel5/logs/`, helper scripts and the
Main10 test: `build/pixel5/`, builds: `build/pixel5-arm64-gpu` (NDK r28, android-24, Release, `DAPPLE_GPU=ON`),
`build/pixel5-desktop-gpu` (desktop + SwiftShader parity). Everything here is *measured* unless marked *estimate*.
Test image: synthetic 6000x4000 JPEG (`build/pixel5/mk_synth.py`: gradients, noise texture, shapes), no private photos on the phone.

## 1. Device facts

| | |
|---|---|
| Model / OS | Google Pixel 5 (redfin), Android 14 (UP1A.231105.001.B2), security patch 2023-11-05 |
| SoC | Qualcomm SM7250 (Snapdragon 765G), board `lito`; 7.6 GB RAM (3.7 GB available at start), 2 GB swap |
| CPU | 8 cores, 3 clusters: cpu0-5 Cortex-A55 (part 0x805), 300-1804 MHz; cpu6 Cortex-A76 (0x804) 2208 MHz; cpu7 A76 2400 MHz; governor schedutil |
| GPU | Adreno 620, vendor 0x5143, driver version 0x801ea000, Vulkan 1.1.128 (`ro.hardware.vulkan=adreno`, feature `vulkan.level=1`, `vulkan.version=4198400`), GLES 3.2 (`ro.opengles.version=196610`) |
| GPU limits (self-test output) | maxImage2D 16384, maxImage3D 2048, shared 32 KB, maxInvocations 1024, subgroup 64 (ops 0x3f), fp16 arithmetic 1, storage16 1, **linear filter on RGBA32F 0**, timestamps 1 |
| Memory types | one 7462 MB device-local heap; types: 0 DEVICE_LOCAL, 1 +HOST_VISIBLE+HOST_CACHED, 2 +HOST_VISIBLE+HOST_COHERENT+HOST_CACHED, 3 DEVICE_LOCAL, 4 +HOST_VISIBLE+HOST_COHERENT, 5 (256 MB heap). **Buffers may only use types 0 and 2** (`memoryTypeBits = 0x5`), so the only host-readable type is cached+coherent. |
| Thermal | `Thermal Status` 0 before and after every run (incl. the 70 s run); the temperatures in `dumpsys thermalservice` are cached and did not update (44-48 C), so only the status and the frame times are evidence. Battery was 10 % -> 9 % (USB 450 mA): the phone was never charged, throttling from battery state cannot be excluded. |
| Self-test | `point+geometry: mean dE 0.040 p99 0.75 max 1.46 ok; spatial: mean dE 0.076 p99 0.86 max 1.45 ok`. Process with self-test 333-356 ms, without 46 ms: **self-test ~300 ms warm** (target <= 300 ms, met just barely; the first ever run also compiles pipelines). |

## 2. Correctness

* `dapple_gpu_tests` (`gpu_parity`) on the Pixel: **PASSED (0 failures)** before and after all changes; the per-stage dE2000 tables are
  identical to the desktop RTX 4060 Ti and to SwiftShader to the printed digits (point 640x448: mean 0.0400 p99 0.80 max 1.66; all
  spatial + geometry: mean 0.0725 p99 0.87 max 1.74-1.85; fp16 source budget 0.0206 / 0.533 / 1.43). Desktop (RTX) and SwiftShader
  `gpu_parity` / `gpu_parity_swiftshader` also pass after the changes (`build/pixel5/logs/gpu_tests_desktop_after.log`). Tile seams: 0/255 differing pixels.
* **No fp16 rounding problem on Adreno 620**: the explicit round-to-nearest-even `h16` store and `pack8` hold; GPU vs CPU baked path at
  1600 px: point mean 0.0134 / p99 0.368 / max 1.44, interactive 0.0307 / 0.456 / 1.25, full 0.0353 / 0.472 / 1.53 (budget 0.1 / 1.0).
* **Driver bug found and worked around**: a shared-memory array of the separable filter sized from specialisation constants
  (`shared T sh[PERP * ROW]`) gives wrong results on this driver (self-test failed: spatial mean dE 15.3, and even the point case varied
  run to run). A fixed-size array (size from a build-time define) is correct. The startup self-test caught it, which is what it is for.
* The C API test "render_frame interactive/full" shows mean 1.19 / 0.74: same value on desktop and emulator (GPU baked vs CPU exact path by design).

## 3. Problems found and fixed (all in `core/gpu/**`)

### 3.1 24 MP frame took 589 s (a GPU hang loop, not slowness)

* Symptom: `gpu-bench` at 24 MP ran > 10 min (first run killed at 589 s, 2.67 M voluntary context switches, 2.0 GB RSS, main thread sleeping in `nanosleep` inside the driver's fence wait at ~6 % CPU, ~80 % of it system time).
* Bisect with the new `gpu-bench --only <stages>`: every stage alone, and all combinations up to ~2.0 s of GPU time per frame, ran fine; adding the texture stage (~3 s GPU in **one command buffer / one `vkQueueSubmit`**) hung.
* Root cause: Adreno's kernel driver (kgsl) treats a submission that keeps the GPU busy for more than ~2-3 s as a hang, resets the GPU and replays the command stream, which hangs again, in a loop. Not memory pressure (RSS 1.4-2.0 GB, swap not growing), not a polling loop of ours. (Inferred from the bisect and the behaviour; kernel logs are not readable without root.)
* Fix: `Engine::submit` now cuts a frame into several submissions of bounded work (`splitGroups_` = 250 000 weighted workgroups, box/gauss/min weighted x2, roughly 0.3-1 s on this GPU; env `DAPPLE_GPU_SPLIT_GROUPS`, 0 = never). Continuation command buffers per slot, a memory barrier opens each piece. Frames up to 1600 px are never split.
* After: the same all-stage 24 MP frame takes 1.5 s; the whole `gpu-bench` at 24 MP (decode, 3 variants x 2 sizes, CPU references, tiled export) takes **44.8 s** (before: > 589 s).
* **Rule**: on Android never let one submission run more than ~1 s. Box passes at 60 MP (R up to 64) must be watched: the splitter cuts between passes, not inside one.

### 3.2 Separable filter ("box"/"gauss"/"min") was 55 % of the frame

Measured per step (1.7 MP interactive frame, GPU ms):

| change | interactive GPU ms | note |
|---|---|---|
| baseline | 106.8 | box 51.5, gauss 24.5, dehaze_apply 12.6, down 7.6, point 4.1, detail 4.0 |
| shared tile sized by radius (R <= 8: 16x16 groups; else 64x4) + float tile for single-channel kinds | 96.7 | small effect: the fixed 12 KB tile was not the main limit |
| input through `texelFetch` (sampled image) instead of `imageLoad` | 79.1 | storage-image loads are 2-3x slower than texture-path loads on Adreno |
| lane-fast thread axis = filter axis in **both** directions | 54.5 | vertical passes were 3x slower than horizontal (10.1 vs 3.5 ms at 1.7 MP, 0.64 vs 0.22 ms on the 1/4 grid): shared-memory bank conflicts when the lane-fast axis is the image column |
| other read-only inputs (dehaze dark channel, blurred NR inputs, `down`, `reduce`, `gf_ab`, `resize`, point source) through `texelFetch`; `dehaze_apply` divides once per pixel | **45.0** | dehaze_apply 12.2 -> 3.5, nr_chroma 11.1 -> 3.9, nr_luma 6.1 -> 3.8, down 7.6 -> 3.3 |

The box itself is still the O(R) sum in shared memory (R is 2-5 in the 1600 px tests, up to 64 at 24-60 MP). It is no longer the bottleneck at 1600 px: the
remaining passes sit on a **memory-bandwidth floor of ~3.5 ms per full-resolution RGBA16F read+write pass at 1.7 MP** (16 B/px = 27 MB in 3.5 ms = 7.8 GB/s,
about half the LPDDR4X peak): `point` (3.7), `down` (3.3), `dehaze_apply` (3.5), `nr_luma` (3.8), `detail` (4.0) and the box passes (3.2-3.5) all land there. Further gains need fewer full-resolution passes
(fuse NR luma with its box, dehaze with detail, read each pixel once), not faster kernels. A true O(1) (prefix-sum / per-thread sliding window) box was **not done**: at the radii used on 1600 px previews it cannot beat the bandwidth floor;
for 24-60 MP exports (R up to 64) it is the next item (24 MP interactive still spends ~415 ms in box).

### 3.3 Readback / copy-out (12-15 ms at 1.7 MP, 120 ms at 24 MP)

* The readback buffer already used a HOST_CACHED type (type 2, cached+coherent); the driver allows no other host-visible type for buffers. The non-coherent write-back type (type 1) is not in `memoryTypeBits`; binding it anyway "worked" in an experiment and was 2-3x faster, but that is invalid Vulkan and was not kept.
* Measured: the memcpy out of the mapped buffer runs at **~1 GB/s** (6.8 MB in 6-14 ms; a second read of the same buffer is as slow, so it is not cache-cold) vs 1.0-1.4 ms for an ordinary heap-to-heap copy of the same size. The mapping is effectively uncached for CPU reads. The output is already RGBA8 (6.8 MB at 1.7 MP); the GPU-side copy takes 1 ms.
* After sleeping in the fence wait the calling thread wakes on a little core at a low clock: copy-out was 12-49 ms depending on placement, 7-13 ms when pinned to the A76 cores. `Session::render` now pins the calling thread to the cores with the highest `cpuinfo_max_freq` (>= 80 % of the top) for the duration of the call (Android only, `DAPPLE_GPU_NO_PIN` disables); copy-out is now a stable 10-14 ms.
* Parallel copy with 2-8 threads did not help reliably.
* **Recommendation**: the ~12 ms per interactive frame cannot be removed by buffer-type choice on this driver. The real fix is not to read frames back at all (SurfaceProducer / swapchain path, gpu-backend.md section 3.2), or an `AHardwareBuffer` with `CPU_READ_OFTEN` imported into Vulkan (a genuinely cached CPU mapping), which was not tried.

### 3.4 Fused point kernel (7.9 -> 7.1 ms with geometry, 4.1 -> 3.7 ms without)

* 3.7 ms for the identity-geometry kernel is at the bandwidth floor above. The "point-wise only" bench variant (7.1 ms) has Catmull-Rom geometry: 16 source taps per pixel on top of the same lattice work.
* Not changed (no gain expected at the floor, and each needs parity re-verification): RGBA16F lattice, 8x8 groups, fp16 arithmetic. Branch divergence between the two lattices is negligible.

## 4. Before / after tables

### 4.1 1600 px (1600x1067, 1.7 MP), source resident, median of 20

| variant | CPU baked path (8 threads) | GPU wall before | GPU wall after | GPU time before | GPU time after | copy-out before | copy-out after |
|---|---|---|---|---|---|---|---|
| point-wise only (geometry) | 26.6 ms | 25.2 | 19.4 | 9.1 | 8.1 | 14.6 | 10.0 |
| interactive (no NR/grain) | 101.7 | 123.2 | **58.0** | 106.8 | **45.3** | 12.5 | 11.5 |
| full quality (all stages) | 154.0 | 186.8 | **86.2** | 171.1 | **72.7** | 13.7 | 10.6 |

Speed-up vs the CPU path: interactive 0.8x -> **1.7x**, full 0.8x -> 1.8x. First frame (source + lattice upload + pipeline creation): point 62 ms, interactive 117 ms, full 141 ms (before 73 / 182 / 241). Peak RSS of the 1600 px run: 446 MB (VmHWM, includes the 24 MP decode).
The CPU lattice bake (41^3) is 22-27 ms on this phone (README low-end emulator: 45 ms).

Per pass after (ms, 1.7 MP, interactive): point 3.7, down 3.3 (twice), box/gauss 0.2-3.5 each (14 passes), dehaze_apply 3.5, detail 4.0, readback copy 1.0, the rest < 0.3.

### 4.2 24 MP (6000x4000), median of 5

| variant | CPU | GPU wall before | GPU wall after | GPU time before | GPU time after | copy-out after |
|---|---|---|---|---|---|---|
| point-wise only | 355 ms | 253 | 239 | 111 | 108 | 124 (before 133) |
| interactive | 1669-1710 | 2479 | **1042** | 2352 | **915** | 121 |
| full quality | 2349-2401 | **hang (589 s, killed)** | **1499** | - | 1370 | 122 |
| tiled export 2048 + halo, all spatial | 2449-2501 | not reached | streamed 2.82 s, resident 2.69 s (8.5-8.9 MP/s), pool 983 MB | | | |
| preview 1600 from the resident 24 MP source (Lanczos-3 on GPU) | | | first 302 ms, later 58.6 ms | | | |

GPU vs CPU full frame at 24 MP: interactive mean 0.0305 / p99 0.455 / max 1.61, full / tiled 0.0357 / 0.474 / 1.63; tile seams 0/255 (gpu_tests). The whole bench took 44.8 s wall with VmHWM 1.99 GB (the bench keeps the float source, a copy and the CPU outputs; the engine pool was 983 MB).
60 MP: **not measured** (the float RGB source alone is 722 MB, plus the bench copies, with 3.7 GB available). *Estimate* from 8.5-8.9 MP/s: ~7 s tiled, within the <= 10 s target; tile 1024 would cut the pool to ~320 MB.

### 4.3 Sustained run (6 x 100 full-quality 1600 px GPU frames, ~70 s, `build/pixel5/logs/thermal.txt`)

GPU frame wall: 88.4, 88.5, 90.2, 90.2, 88.8, 88.9 ms (GPU time 72.6-72.7 ms constant): **no throttling in 70 s** (thermal status 0 throughout; CPU clock readings varied 0.8-1.77 GHz with the governor). A 70 s load is short; the phone started at room temperature and was not charging.

### 4.4 CPU baselines on this phone (8 threads; `build/pixel5/logs/edit_bench.log`, `film_render_bench.log`)

`edit-bench --scenario 1 --quick` (synthetic 24 MP JPEG as the source, session at 2048 long edge, default auto edit without heavy spatial stages):

| stage | ms |
|---|---|
| interactive 1080, exposure drag (re-bake 41^3) | 23.7 |
| interactive 1080, saturation drag (bake 41^3) | 32.6 |
| interactive 1080, clarity drag (lattice cached) | 27.1 |
| interactive 1600, exposure drag | 36.5 |
| final 1600, params unchanged (lattice cached) | 28.6 |
| final 1600 after point-wise change (bake 65^3) | 38.5 |
| reference (exact) 1600 + analysis | 112.5 |
| bake 41^3 incl. analysis / 49^3 / 65^3 | 20.2 / 12.9 / 18.8 |
| lattice pass 1067x1600 no spatial / + all spatial / interactive spatial 1080 / only 1080 | 28.7 / 28.2 / 13.8 / 13.3 |

(The "all spatial" row is cheap because the auto edit enables no spatial stage; the GPU bench rows of 100/154 ms use explicit NR/dehaze/clarity/texture/sharpen/grain.)

`film-render-bench --pack build/film/dapple_new.dfp --runs 3 --mp 2` (Portra 400, 1732x1154 source; row labels are the tool's own):

| | normal | film cold | film cached |
|---|---|---|---|
| "26 MP" no spatial | 57.6 | 70.0 | 56.6 |
| "26 MP" all effects | 116.8 | 331.2 | 318.5 |
| 1600 preview, no spatial | 69.6 | 81.1 | 65.7 |
| 1600 preview, all effects | 116.9 | 304.6 | 287.0 |
| 1600 preview, interactive quality | 120.7 | 217.9 | 205.6 |
| 1600 interactive frame while dragging print warmth (bake + render) | 243.5 | | |

`film-render-bench` with the older `dapple_v1.dfp` failed with "bad magic", and `film-bench` was not run; only the current pack was used. The tool's "26 MP" rows are not a real 26 MP image (the source is 2 MP), treat them as the tool's output.

## 5. Comparison with the estimates (pipeline-architecture.md section 1.3) and the emulator

| item | estimate / emulator | Pixel 5 measured | verdict |
|---|---|---|---|
| Low-end CPU engine proxy "90-100 ms per interactive 1080 exposure frame" (throttle `lowend`, SD680/G85 class) | 90-100 ms | 23.7 ms for the 1080 exposure drag with re-bake (same stage family) | The Pixel 5 (2x A76 + 6x A55, 8 threads) is **~4x faster than the `lowend` profile** and sits near the `mid` profile (f = 0.44) for this stage; it is not an SD680-class stand-in. lowend f 0.139 x ~4 gives ~0.55 host P-core equivalents *(approximate, the bake paths differ)*. A real SD680/Helio G85 phone is still needed for the low-end claim. |
| Colour bake 41^3 = 45 ms on lowend | 45 ms | 20-27 ms | holds in direction; the bake is not the critical item on this class |
| GPU time of the 1600 px interactive graph: "phone 3-6x slower than desktop, 4-9 ms" (desktop 1.47 ms) | 4-9 ms | **45 ms** (33x the desktop 1.36 ms); the point pass alone 3.7 ms (60x) | **does not hold.** Adreno 620 has ~7.8 GB/s effective bandwidth and the graph is bandwidth bound with ~27 full-resolution passes. The 4 ms target is out of reach on this class without fewer passes; a flagship (Adreno 830) is a separate question. |
| "GPU removes the per-pixel and neighbourhood costs" | big win expected | GPU 58 ms wall vs CPU 102 ms: 1.7x with readback, 2.2x GPU-only | partly: faster, not free. With the bake at 20-27 ms the CPU critical path estimate (<= 30 ms) holds. |
| Unified memory makes readback cheap | | readback copy 10-14 ms at 1.7 MP | **does not hold**: unified memory does not make CPU reads of GPU buffers cheap. |
| Export 60 MP GPU <= 10 s | estimate | 24 MP tiled 2.7 s -> ~7 s *estimate* | plausible |
| Self-test <= 300 ms | estimate | ~300 ms | borderline (with a warm pipeline cache) |
| Pool memory: 318 MB at tile 1024 / 565 MB at 2048 (desktop) | | 983 MB pool at tile 2048 with 24 MP (1 GB budget cap) | the 1 GB pool budget is too large for a 7.6 GB phone that must keep the app alive; use tile 1024 on this class |

### Recommendations for this class of device (Pixel 5 / mid-range Adreno 6xx)

* **Tile size**: 1024 (+ halo) for export: pool ~320 MB, each tile in its own submission, per-submit work far below the hang limit.
* **Interactive tier**: keep 1080 px (ladder 1080/810/640/480). At 1600 px the GPU path is 58 ms (17 fps) with all spatial stages, ~20 ms (wall incl. 10-13 ms readback) for colour-only drags; at 1080 about 0.45x of that. On this class the CPU (23-37 ms per 1080-1600 exposure drag) is competitive for point-only edits; the GPU wins when NR/dehaze/clarity/sharpen are on (102 -> 58 ms at 1600) and for exports >= 12 MP. Use the GPU for point-only drags only once the display path avoids the readback.
* Never submit more than ~1 s of work at once; persist the `deviceKey` (`5143:6020000:801ea000`) with the self-test result.
* Mobile GPU rules learned: single-channel float tiles, texture-path reads (`texelFetch`), lane-fast axis along the filter axis, 16x16 groups for small radii, fixed-size shared arrays.

## 6. HEVC Main10 encoding on the Pixel 5

Standalone NDK test (`build/pixel5/main10/m10.cpp`, AMediaCodec + AMediaMuxer; run from `/data/local/tmp/dapple/main10`, removed afterwards), 1920x1080, 30 fps, 90 frames of a smooth 10-bit gradient (P010) or the equivalent surface frames, `profile=2` (HEVCProfileMain10), colour tags BT.709 SDR or BT.2020 HLG.

**Result: the Pixel 5 cannot encode HEVC Main10 by any route (hardware or software).** Capability dump: `build/pixel5/main10/media_player_full.txt` (`dumpsys media.player`).

| encoder | advertised HEVC profiles | colour formats | what happened |
|---|---|---|---|
| `c2.qti.hevc.encoder` (HW, alias `OMX.qcom.video.encoder.hevc`) | **Main only** (1) | Surface, YUV420Flexible/Planar/SemiPlanar/PackedPlanar/PackedSemiPlanar, **YUVP010 (0x36) listed** | `configure` with `color-format=54` + `profile=2` returns OK, `getInputFormat` shows stride 3840 / slice-height 1088 (P010 layout), 2 input buffers are accepted, then **no output ever** (60 s timeout). logcat: `QC2V4l2Driver: failed to set profile/level`, `Failed to set Profile on output port`, `QC2Comp: configureCodec: Failed to set updated param Output::ProfileLevel`, `QC2V4L2PollThread: HW unsupported`. The same with `profile=1` (Main) + P010 input, with `profile=0x1000` (Main10HDR10), and with Surface input (R10G10B10A2 window, 90 frames posted, 0 output). |
| `c2.qti.hevc.encoder.cq` (HW, constant quality) | Main only | as above | same hang with Main10 + P010 (2 buffers accepted, no output) |
| `c2.android.hevc.encoder` (software) | Main, MainStill | flexible 8-bit formats only (no P010) | accepts `profile=2`, its output format reports `profile: 1` (silently downgraded to Main), stalls after 4 input frames |
| control: `c2.qti.hevc.encoder`, NV12 8-bit, `profile=1` | | | **works**: 90/90 frames in 2.08 s = **43 fps encode** at 20 Mb/s; ffprobe: hevc, profile Main, yuv420p, bt709/bt709/tv, 90 frames (`build/pixel5/main10/ctl_nv12.mp4`) |

Because no Main10 stream can be produced, the planned checks (ffprobe `yuv420p10le`, > 256 luma levels after decode, BT.2020/HLG tags in the file) could not be run. P010 buffer layout facts: input buffer = `stride * (sliceHeight + height/2)` bytes with stride 3840 and slice height 1088 (not `w*h*3`).

Decoders: `c2.qti.hevc.decoder` (+ `.low_latency`, `.secure`) is hardware and lists **Main, MainStill, Main10 (High 6.1), Main10HDR10**, colour formats incl. YUVP010: **Main10 hardware decode exists** (playing 10-bit HEVC from other devices works); `c2.android.hevc.decoder` is the software fallback.

### App code review (`app/android/.../SoftwareMedia.kt`, `LiveBridge.kt`; not edited)

* `findMain10Encoder` requires `profileLevels` to contain Main10/Main10HDR10/Main10HDR10Plus **and** `colorFormats` to contain 54 **and** the size to be supported. On the Pixel 5 this correctly returns `null` (the encoders list Main only), so the clip falls back to 8-bit H.264. The capability query matches what the Pixel supports; the 10-bit path **has not run on a real Main10 encoder** and cannot be tested on this phone.
* Weak points for devices where the list is wrong (the software encoder here accepts `profile=2` and silently produces Main, the HW encoder lists P010 but fails at run time):
  1. After `INFO_OUTPUT_FORMAT_CHANGED` check `outputFormat.getInteger(KEY_PROFILE)` is 2 / 0x1000 / 0x2000; otherwise abort and take the H.264 branch (today a downgraded stream would be muxed as 10-bit and reported with `videoProfile = "Main10"`).
  2. The encode loop only has the full `TIMEOUT_MS` deadline; the failure seen here (the encoder accepts input and never produces output) needs a short no-output watchdog (e.g. no output after ~3 s and 8 queued frames: fall back).
  3. Do not assume the input buffer size is `w*h*3` (here `stride*(sliceHeight + h/2)`); the code correctly goes through `getInputImage` planes, keep it that way.
  4. Optionally probe-encode 2 frames at the real size once per `deviceKey` and cache the result instead of trusting `profileLevels`.
* No change is needed for the capability query to be right on this phone.

## 7. Not measured

60 MP tiled export (memory), the app-level run with `debug.dapple.gpu` and the perf probe (needs a `DAPPLE_PERF` build; app/ was being built by someone else), power (no Perfetto/battery stats; battery 9-10 % and not charging), a multi-minute thermal soak (only 70 s), `film-bench`, a true O(1) box, other device classes, `AHardwareBuffer` readback, simpleperf (the cause of the hang was found by bisection; kernel-side behaviour is not observable without root).


## Wave 8 (2026-11-12, HEAD 1088f2d clean worktree build; arm64 NDK r28 Release `DAPPLE_GPU=ON DAPPLE_GLES=ON`, release APK; phone physically in landscape, 90 Hz mode (60/90 available), Battery Saver off, battery 7-9 % on USB, Thermal Status 0 before and after everything)

Logs `build/dev2/pixel5/logs/`, screenshots `build/dev2/pixel5/app_*.png`, `cam_*.png`, `sample.dng`. Native binaries in `/data/local/tmp/dapple` (removed at the end), not pinned.

**Correctness (Vulkan, Adreno 620):** `dapple_gpu_tests --quick` and the full run PASSED; film_gpu.parity ok (the six enlarger rows now mean dE 0.02-0.037, p99 <= 0.12, max <= 0.40: the unaligned-LUT-upload fix holds on Adreno Vulkan); film_gpu.frames ok; `--film-tiling` PASSED; `--coverage` 192/192 on the GPU executor; `dapple_golden selfcheck` OK; `dapple_exec_tests` all ok; `dapple_gpu_engine_tests` hal/box/sched/select/external PASSED. `dapple_match_cache_tests` SKIPs on a phone (pack path hard-coded to `DAPPLE_SOURCE_DIR`): NOT RUN.

**GLES forced (`DAPPLE_GPU_API=gles`) FAILS the film checks.** `dapple_gpu_tests --quick`: 40 failures (all film rows, e.g. `film portra400, all effects: mean dE 3.05 p99 5.75`); film_gpu.parity 23 failed rows (final tier mean dE 2.9-3.3, enlarger 4-14, dye-cloud / micro-structure 21-54; worst pixel `cpu 0.9141 0.4543 0.8473  gpu 0.9438 0.5151 0.8926`); film_gpu.frames 52 failures; `--film-tiling` 3 failures (`exact film portra: mean 3.045`). The point chain rows pass (mean 0.014). Identical counts on the Note10+ (Adreno 640) and seen on a Redmi (Adreno 710): all Adreno 6xx/7xx, passes on the Adreno 840.

**Film GPU frame (Vulkan `dapple_film_gpu_tests bench`, median of 10, GPU ms / wall ms incl. readback, 3 runs, spread < 1 %):**

| case | GPU / wall | vs 8.3 / 11.1 / 16.7 |
|---|---|---|
| 810x1080 interactive | 99 / 226 | missed 12x / 9x / 6x |
| 1080 px interactive | 94 / 210 | missed |
| 1600 px interactive | 249 / 500 | missed |
| 1600 px interactive, grain drag | 271 / 520 | missed |
| 1600 px final | 438 / 690 | missed |
| 810x1080 final | 284 / 408 | |
| 3600 px final | 1988 / 3300 | |

GLES forced (wrong colours): 810x1080 interactive 141 / 185, 1600 interactive 303 / 366. Per pass (`DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PROF_REPEAT=8`, 810x1080): film_point 7.1, gnoise 1.7, **film_h3 18.2, film_v3 21.7**, film_pd 3.8, readback 1.1 = 53.5 ms (h3 / v3 achieve 1.6-2.0 GB/s, 0.9 GB/s at 1600 px). **`DAPPLE_GPU_PROF_REPEAT=20` gives `vkQueueSubmit failed (VkResult -4)` (device lost)**: profiling repeat bypasses the submission splitter (one submission ~5 s); repeat 8 / 3 / 2 is fine. No hang or reset in any normal run.

First film frame in a fresh process (`dapple_gpu_tests --film-interactive`): 1080 px 457 ms (upload 177, record 164, wait 106); 1600 px 656 ms (184 / 166 / 286). Session path wall ms (still / strength / exposure / grain / warmth): 1080 px 61.6 / 66.1 / 63.3 / 79.1 / **104.1** (upload 42.6); 1600 px 245 / 254 / 247 / 279 / 292.
Drag bench (`dapple_drag_bench`, 480 px long edge, `DAPPLE_GPU=1`, median ms): strength 16.2, grain 21.8 (first 141), **film_shoot 166.7**, print 27.3, black 27.3, latitude 16.3, wb 15.5, warmth 27.2, tint 27.8, film_exp 15.5, exposure (film on) 28.7 (CPU path: shoot 189, print 50, exposure 144).
Non-film 1600 px: point 22.5 wall, interactive 48.1 (GPU 33.9, 29 passes), full 75.2 (GPU 61.6). 24 MP whole: point 241, interactive 790, full 1255 ms (readback 118-124). Tiled export 24 MP: tile 2048 2.9 s (8.3-8.8 MP/s), tile 1024 2.9 s; vs the CPU full frame dE mean 0.0357 p99 0.474 (seams ok). 60 s sustained loop (810x1080 interactive bench): GPU 99.1-99.6 ms, Thermal 0, flat.

**App (first install; photo access through the system "limited access" picker with one synthetic test photo):** fresh start `gpu probe: api=vulkan device="Adreno (TM) 620" self-test fresh, probe total 798 ms`; frames before it were `exec=cpu reason=GPU self-test running`; warm-up `film pipelines + default stock 2076 ms` (518 ms on the second start). Zero-copy present works: `dapple_present: device Adreno (TM) 620 type 1 ... (shared with the engine)`, swapchain 1600x1066 / 1118x745, `dapple_view: shown 60 ext 59 host 1 hostBytes 3110400 tooSmall 0 refused 0`, present 0.3-0.4 ms. Exposure drag (film off, 1080x720): cpu_ms 24-29 (tables 8.8, upload 15-20), view render ema 16.7-34 ms. **Film pad drag at 480x320: `cpu_ms 234.3 (match 110.2 ... upload 123.8) max 2397.1`, view `render ema 118.42 ms`** (first drag frames up to 2.4 s). Film grain drag after warm-up: cpu_ms 0.5-0.8, view render ema 5.3 ms, tier 480 -> 640 -> 1080.
Layout (landscape 2340x1080, 440 dpi): the right-hand panel works, but (a) in 风格 the chip grid shows 1.5 rows; the film chip and film pads are below the fold, and a swipe aimed at a ruler scrolled the tone-target grid instead; (b) before a tab is chosen the film strip and the toolbar float over the photo; (c) the last chip of the tab bar is clipped; (d) the `更多` popup's first item `分级` sits under the status bar (tapping its centre does nothing); (e) **Settings: the category list (相机 ... 编辑) is clipped and does not scroll, so 关于 and the codec diagnostics page (long-press on 版本) are unreachable in landscape.** Portrait NOT checked (phone was lying sideways; rotating is a system setting).
Camera: badge `RAW + JPEG`; one shot (phone face down, black picture) saved `Pictures/Dapple/DAP_20261112_233223.dng` 24 415 172 B + `.jpg` 1 444 918 B (MediaStore rows 1000000200 / 1000000201), the editor opened on the JPEG: badge matches the files. `sample.dng` pulled (a black frame). Test shots, `DT_synth24.jpg` and their rows deleted.
NOT verified: codec diagnostics page (unreachable), Main10, portrait layout, film latitude visibility, edit-then-export.
