# Samsung Galaxy Note10+ (SM-N9760) device results, 2026-11-12

Borrowed owner phone, temporary permission. Same tools and methodology as `pixel5.md` (`build/pixel5-arm64-gpu` CLI + `dapple_gpu_tests`, `build/gles-android-arm64/dapple_gles_check`, `build/pixel5/main10/m10`, sustain script `build/note10/sustain.sh`). Raw logs: `build/note10/logs/`, screenshots `build/note10/s*.png`. Fixtures: synthetic only. Find N5 numbers come from `find-n5.md` / `build/n5/logs/`.

**Caveat for every timing: Android Battery Saver was ON (`settings get global low_power` = 1, "Battery Saver is currently: ON", sticky), battery 40-42 % on USB.** Settings may not be changed in this session, so all timings are "battery saver on" and they are bimodal (4.1b). Re-run with Battery Saver off before final conclusions.

## 1. Device facts

| | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| OS | Android 12, One UI 4.1 (`oneui=40100`), SP1A.210812.016.N9760ZCU5HVF2, patch 2022-04-01 | Android 14 | Android 17, ColorOS 17 |
| SoC | SM8150 (Snapdragon 855), board msmnile | SM7250 | SM8850 (8 Elite Gen 5) |
| CPU | 4x A55 (1.79 GHz) + 3x A76 (2.42) + 1x A76 prime (2.84), schedutil | 6x A55 + 2x A76 | 7 Oryon-class cores |
| RAM | 11.6 GB (7.7 avail), 4 GB swap | 7.6 GB | 15.5 GB |
| GPU | Adreno 640, Vulkan 1.1.128, driver 0x801f6000, deviceKey `5143:6040001:801f6000` | Adreno 620, 0x801ea000 | Adreno 840, Vulkan 1.4.295 |
| GLES | ES 3.2 V@0502.0 (Date 10/01/21), AEP, `ro.opengles.version=196610` | 3.2 | 3.2 |
| GPU limits | maxImage2D 16384, 3D 2048, shared 32 KB, subgroup 64 (0x3f), fp16 arith 1, **storage16 0**, linear RGBA32F 0, timestamps 1 | storage16 1 | storage16 1, linear RGBA32F 1 |
| Memory | one 11361 MB device-local heap; type 1 HOST_VISIBLE+CACHED, 2 +COHERENT+CACHED, 4 HOST_VISIBLE+COHERENT, 5 = 256 MB heap (same layout as Pixel 5) | same | several cached+coherent types |
| Display | 1440x3040 physical (override 1080x2280), 60 Hz only, HDR types 2,3,4 | | 120 Hz |
| Thermal | status 0 before/after every run; AP 35-37 C, BAT 30-37 C | status 0 | status 0 |

## 2. Vulkan correctness

* `dapple_gpu_tests`: **PASSED (0 failures)**. Self-test `point+geometry mean dE 0.040 p99 0.75 max 1.46 ok; spatial 0.076 / 0.86 / 1.45 ok`, identical to Pixel 5 / desktop. Per-stage tables, tile seams (tile 128..2048, 0/255 differing), steady state (pool 1023.8 MB, 47.6 ms frame), C API hook and blocklist all pass. **No driver bug found on Adreno 640** (the Pixel 5 shared-array bug is worked around and does not reappear; fp16 rounding fine). GPU vs CPU baked at 1600 px: point 0.0134/0.368/1.44, interactive 0.0307/0.456/1.25, full 0.0353/0.472/1.53; 24 MP tiled 0.0357/0.474/1.63 (same digits as Pixel 5).
* Difference: `storage16 = 0` (no 16-bit storage) on this driver; the engine passes anyway. Keep it in the persisted capability key.
* No kgsl hang at 24 MP (submit splitting works); 60 MP export fine.

## 3. GLES 3.1 backend

* `dapple_gles_check` (arm64, NDK r28, new dir `build/gles-android-arm64`, configured like the x64 one; built cleanly, 45 steps): "Adreno (TM) 640" GLES 3.2, key `5143:d5ef:0b92d5ef`, **self-test OK in 21.3 ms**, numeric case 0 of 3108 values off, buffer copy chain ok; EGL + setup ~780-830 ms. Reports: subgroup 0, fp16 arith 0, timestamps 0, linear RGBA32F 1.
* Selection: `--auto` tries Vulkan first (not linked into this tool: "Vulkan not linked into this tool"), then GLES31 succeeds. The tool always reports `forced: gles` and ignores `DAPPLE_GPU_API=vulkan|cpu`, so the full Vulkan -> GLES -> CPU order could not be observed. The shipped `libdapple.so` / `dapple_gpu_tests` do not contain the new HAL, so **GLES parity vs the CPU reference and a forced-gles engine run could not be run** (`DAPPLE_GPU_API=gles dapple_gpu_tests` just reruns the Vulkan suite).

## 4. Timings (battery saver ON)

### 4.1 1600 px (1.7 MP), source resident: GPU wall ms (GPU time)

| variant | CPU baked: Note10+ / P5 / N5 | Note10+ GPU wall (GPU / copy-out) | Pixel 5 | Find N5 |
|---|---|---|---|---|
| point-wise | 33-40 / 26.6 / 15.5 | **15.4-18** (3.5 / 9-12) | 19.4 (8.1) | 3-4.6 (0.4) |
| interactive | 87 / 101.7 / 39.3 | best 36-42 (20-25 / 13); typical 62-69 (47 / 19); worst 182 (47.8 / 127) | 58.0 (45.3) | 5.7-8 (3.3) |
| full quality | 139 / 154.0 / 68.1 | 48 (33 / 12.5); worst 110-160 (33-75 / 73-76) | 86.2 (72.7) | 10.8 (5.9) |

First frame: point 54-59, interactive 78-85, full 97-105 ms (Pixel 5: 62 / 117 / 141). CPU lattice bake 41^3: 21-25 ms. Peak RSS 451 MB.

### 4.1b Bimodal behaviour (retest without Battery Saver)
Same binary, same graph, screen on or off, different numbers between runs:
* Interactive GPU time 20.5 / 24.8 ms (best) vs 47-48 ms; full 33 ms vs 46.6 / 74.8 ms. In the slow state interactive costs as much as full, so it is not work-dependent: GPU/CPU frequency state (ramp after the short point variant, battery-saver caps).
* Copy-out of the same 6.8 MB: 10-19 ms normally, but 73-128 ms in some runs (6-10x). The engine's own pin to big cores did not prevent it. `taskset f0` / `70` were mostly good, `taskset 0f` (little cores) always bad (~120 ms), unpinned (`DAPPLE_GPU_NO_PIN=1`) 76-83 ms. The readback copy is more sensitive to core and clock placement than on the Pixel 5. Recommendation stays: avoid readback (display path).
* 150-frame run (42 s): interactive 182 ms (GPU 47.8, copy-out 127), full 63 ms.

### 4.2 24 MP (6000x4000), tile 1024

| | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| point-wise GPU wall / CPU | 187 (GPU 60, copy-out 118) / 447 ms | 239 / 355 | 41.6-44.9 / 209 |
| interactive GPU / CPU | 601 (GPU 483) / 1546 | 1042 / 1670 | 115-119 / 835 |
| full GPU / CPU | **831** (GPU 710) / 2096 | 1499 / 2350 | 162 |
| tiled export 24 MP, streamed / resident | tile 1024: 1.80 s (13.4 MP/s) / 1.61 s (14.9), pool 951 MB; tile 2048: 1.56 / 1.37 s (15.4 / 17.5 MP/s), pool 460 MB | (2048) 2.82 / 2.69 s | 0.64 / 0.32 s |
| 60 MP tiled, tile 1024, streamed | **5.56 s** (10.8 MP/s), pool 310 MB, RSS 930 MB | not measured | 1.1 s |
| preview 1600 from resident 24 MP | first 199, later 132 ms | 302 / 59 | |
| bench RSS (24 MP with CPU refs) | 2.0 GB, 36.7 s wall | 1.99 GB, 44.8 s | 2.0 GB |

Tiled GPU vs CPU full frame: mean 0.0357 / p99 0.474 / max 1.63.

### 4.3 CPU baselines (`edit-bench --scenario 1 --quick`, `film-render-bench --pack dapple_new.dfp --runs 3 --mp 2`)

| ms | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| interactive 1080 exposure drag | 33.3 | 23.7 | 16.4 |
| interactive 1080 saturation drag | 48.9 | 32.6 | 20.4 |
| interactive 1080 clarity drag | 28.0 | 27.1 | |
| interactive 1600 exposure drag | 61.6 | 36.5 | 30.1 |
| final 1600 unchanged / after bake 65^3 | 41.3 / 66.3 | 28.6 / 38.5 | |
| reference exact 1600 | 159.8 | 112.5 | 94.2 |
| bake 41^3 / 49^3 / 65^3 | 14.1 / 17.3 / 25.6 | 20.2 / 12.9 / 18.8 | 9.7 |

| film-render-bench (Portra 400) | normal | film cold | film cached |
|---|---|---|---|
| "26 MP" no spatial | 63.7 | 75.3 | 61.5 |
| "26 MP" all effects | 117.1 | 291.1 | 276.9 |
| 1600 preview no spatial | 77.5 | 84.4 | 72.5 |
| 1600 preview all effects | 117.7 | 270.0 | 254.2 |
| 1600 preview interactive | 117.2 | 199.6 | 187.8 |
| print-warmth drag frame | **228.9** (Pixel 5 243.5, N5 123.3) | | |

CPU is about 1.4-1.7x slower than the Pixel 5 in the edit-bench items (A55x4 + A76x4 plus battery saver), faster only in bake and the print-warmth drag.

### 4.4 Sustained run
A 600-frame chunk takes more than 2 min in the slow state, so one-variant (full quality) load was run for ~4.6 min (2 chunks, 13:20-13:24), then stopped on purpose: frame 152.3 and 152.4 ms (GPU 74.6, copy-out 74.5), flat, **no throttling trend**, `Thermal Status` 0, AP 35.2 -> 35.8 C, BAT 35.6 -> 37.1 C. This is the slow state (GPU 74.6 vs 33 ms best case), likely battery saver, not heat.

## 5. Codecs

* `dumpsys media.player`: HEVC encoders `OMX.qcom.video.encoder.hevc` (HW, Main + **Main10HDR10** advertised, no P010 `0x36` in its colour list: Surface, Flexible, SemiPlanar, Qualcomm 0x7fa30c0x; max 4096x2304) and `c2.android.hevc.encoder` (sw, Main/MainStill, max 512x512). AVC: `OMX.qcom.video.encoder.avc` (HW, up to High 5.2), `c2.android.avc.encoder`, `OMX.google.h264.encoder`. Decoders: `OMX.qcom.video.decoder.hevc` lists Main, **Main10**, Main10HDR10 (8192x8192), `...avc` Baseline-High.
* `m10` (Main10 P010, buffer mode): `configure` returns -10000 for the HW encoder with profile 2, 0x1000 and also the Main control (tool hard-codes colour-format 54, not listed). logcat: "Unsupported eColorFormat", "Reset to OMX_VIDEO_HEVCProfileMain10HDR10 profile instead of HEVCProfileMain10", "Failed to update the profile_level", "cannot encode HDR static metadata. Ignoring". Surface mode (profile 2 / 1 / 0x1000, 8 s watchdog): no output, including the Main control, so the tool's surface path is inconclusive here. Software encoder: no P010/Main10. **No Main10 stream could be produced, so the decode-back luma level count was not measurable.** Same conclusion as the Pixel 5: Main10 encode unavailable, app falls back to 8-bit.
* App (设置 -> 关于 -> long-press 版本 -> 编解码器 -> 全部重测; `s03`..`s05_*.png`, all viewed):
  * Self-tests: `enc8avc OMX.qcom.video.encoder.avc` PASS 229 ms (8 frames, decoded by OMX.qcom.video.decoder.avc, 220 distinct luma levels); `enc8hevc OMX.qcom.video.encoder.hevc` PASS 429 ms (221 levels); `enc8avc c2.android.avc.encoder` PASS 259 ms (219 levels).
  * Ladders `encodeLadder1080p30`, `...Hdr`, `...8Bit`: 1. HEVC Main 8-bit (OMX.qcom), 2. H.264 High HW, 3. H.264 sw. No 10-bit rung (correct).
  * `decodeLadderHevc10`: **software only** although the HW decoder advertises Main10, so HW Main10 decode is not used (worth checking why); `decodeLadderHevc8`: HW then sw; `decodeLadderAvc8`: HW then sw; `decodeLadderAvc422`: software.
* Motion Photo: not generated through the app (the in-app video picker would show the owner's library, which is not allowed). Instead the CLI `dapple motion` on the device with a synthetic cover: `gradient_hevc_main10_MP.jpg` (VALID, hvc1 640x352, 3 frames) and `sdr_h264_422_10_MP.jpg` (VALID, avc1 64x48, 15 frames); outputs in `build/note10/out/`. ffprobe (desktop ffmpeg 9.0.2) of the extracted clips: `hevc Main 10 yuv420p10le bt709` and `h264 High 4:2:2 Intra yuv422p10le bt709`. The CLI only trims/copies, so these are the source formats; **the app's 10-to-8-bit transcode path was not exercised on this device**.

## 6. Recommendations (older Adreno 6xx flagship)

* GPU is correct (no new driver bug); enable it after the startup self-test. Speed-up vs CPU: point 2.0x, interactive 1.3-2.5x (0.5x in the worst readback state), full 2.9x at 1600 px; 24 MP 2.5-2.6x; export 24 MP 1.6-1.8 s, 60 MP 5.6 s (target <= 10 s met).
* **GPU default ON for this class, gated by the self-test**, mainly for full quality, spatial stages and export. For point-only drags CPU (33-62 ms) and GPU (15-18 ms wall, readback 9-13 ms and up to 127 ms) are close; use the GPU for them only when the display path avoids readback.
* **GLES not needed** on this device (Vulkan 1.1 works); GLES 3.1 also passes its self-test and is a valid fallback, but its on-device parity is unmeasured.
* **Tile size 1024** (60 MP pool 310 MB, RSS 930 MB; 2048 is ~15 % faster at 24 MP, pool 460 MB).
* **Interactive tier 1080 px** (ladder 1080/810/640/480): 1600 px full quality is 48 ms best case, 110-160 ms bad case.
* Never submit > ~1 s per submission; persist `deviceKey` and `storage16` with the self-test result.
* Open: readback copy-out up to 127 ms in some runs; GPU frequency bimodality (retest without battery saver); HW Main10 decode not chosen by the decode ladder; no Main10 encode.

## 7. Not measured
GLES parity vs CPU on device and forced-gles engine run (shipped binaries lack the new HAL); Main10 encode/decode-back level count; in-app Motion Photo generation and the app's 10-bit transcode; a fixed 2-min sustain (ran ~4.6 min, one variant); isolated Vulkan self-test cost (only the GLES self-test, 21 ms, was timed); power; timings with Battery Saver off; 60 MP CPU path; Pixel 5 60 MP.

## 8. Cleanup
Removed `/data/local/tmp/dapple` and `/sdcard/Download/dapple_test`, uninstalled the app (it was not installed before), deleted the MediaStore row of the `dapple_test` folder (id 205095). No Motion Photos were written to the phone's media storage. `build/note10/listing_before.txt` vs `listing_after.txt` (`/data/local/tmp`, Download, DCIM, Pictures, Movies, package list): identical except the mtime of the `/data/local/tmp` directory entry. Screen woken once (KEYCODE_WAKEUP) and Home pressed; no settings changed; battery saver left ON.
