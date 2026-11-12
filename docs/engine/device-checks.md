# On-device checks: one place (assembled 2026-11-13 by the engine-gaps engineer)

Rules for every check: `adb -s <serial>` always explicit (never the owner's other phones), no system settings changes, remove what you push. Sources of the lists
below are named so that the original text (numbers, expected log lines) stays the authority; this file is the index plus the checks of the capture / anchor work.

## A. Capture, exposure anchor, DNG decode (engine-gaps, this wave)

Phone: Galaxy Note10+ (debug or release APK built from this tree).
1. **RAW-only capture embeds its preview.** In the capture screen choose a lens whose mode is RAW only (or force it: the mode fallback is remembered per lens). Take a shot; pull the DNG from the app's inbox (`Documents/.../inbox/DAP_*.dng`). `dapple info --anchor DAP_*.dng` on the PC must print `tier 1 embedded preview ... source embedded-preview`, confidence above 0.3. Without the embed (older build) it prints tier 3.
   Also check the thumbnail itself: `dapple thumb DAP_*.dng -o t.jpg` must be a recognisable, upright (or sensor-oriented) 720 px picture of the scene. If it is rotated by 90 degrees, report the DNG Orientation (`dapple info`) and the phone orientation at the shot.
2. **RAW+JPEG capture uses the paired JPEG.** Shoot in RAW+JPEG; both files sit next to each other in the inbox with the same stem. `dapple info --anchor DAP_*.dng` must show `source reference-image` (the sibling `.jpg`; the DNG has no preview of its own). The editor's first frame of the DNG should have a mid-grey level like the JPEG (mean L* within about 5).
3. **AE settle.** Open the capture screen and press the shutter at once, repeat after 2 s of waiting, same dark surface. Compare the DNG exposure (`EXIF ExposureTime` / `ISO`, e.g. by `dapple info` or exiftool): the first must not be darker than the second; logcat tag `CaptureActivity` shows the 700 ms wait (`CaptureSettle`). If the early shot is still darker, the cause is not the settle delay (then look at CameraX's own precapture and the HAL).
4. **Dark scenes.** A very dark scene (hand over the lens, indoors at night): the engine's render of the DNG must be dark and neutral (no magenta, `suppressShadowChroma` is gated to frames with a 99.5th percentile luma below 0.03 of white), the JPEG of the same shot will be brighter (ISP tone mapping): this is expected.
5. **Monochrome files** (only if a mono phone / camera file is at hand): the preview comparison is median based; `dapple info --anchor` shows `monochrome: median luminance match ... rank correlation`.
6. **JPEG XL DNG (Samsung S22 and newer):** copy such a file into the library; it must open, show colours like the camera's own preview (`dapple thumb`), and decode in about 1 - 3 s on the phone. A DNG that fails with "JPEG XL DNG with 4 sample(s) per pixel (CFA mosaic)" is the not-yet-supported CFA variant (Adobe's converter): note the file, do not retry.
7. **X-Trans decode time** (Fujifilm RAF 40 MP in the library, release build): time from tap to the first correct picture (proxy, expected about 1 s) and the time of a full-quality export decode (`dapple_engine` / `decode-timing` lines are desktop only: use the wall time of "save full resolution"); report both. The OpenMP demosaic is on for Android (static libomp): compare with a build `-DDAPPLE_LIBRAW_OPENMP=OFF` if the number looks too high.
8. **Licences screen** (settings -> about -> open source licences): entries LibRaw 0.23 (CDDL-1.0), libjxl + Highway (BSD 3-Clause), LLVM OpenMP runtime (Apache-2.0 with LLVM exception) are present below the FFmpeg entry.

## B. Motion Photo, hardware Main10 (motion-photo engineer)

Full list: `docs/engine/pipeline-architecture.md`, section 30.13 "On-device checklist (updated, hardware Main10, edited clips)" (six steps: the codec page rows, edited export with film, HLG / PQ export without film, unedited export, thermal and time, EGL / dec10 failure reports) and the "On-device check (Note10+ ...)" paragraph of `docs/engine/device-results/find-n5.md` Wave 4 (codec page, `dec10`, `enc10` lines and the Main10 muxed-track check).

## C. Present path and GPU view (present engineer)

Full list: `docs/engine/present.md`, "On-device checklist (Pixel 5 / N5)" (seven steps: `debug.dapple.gpu 1` and the `dapple_present` / `dapple_view` log lines, Adreno / Mali presentation extensions, 24 MP drag and release, N5 fold / rotate / split screen, trim-memory, killing the GPU context, comparison with the readback path `DAPPLE_PRESENT_ADOPT=0` / `DAPPLE_GPU_VIEW=0`).

## D. GPU probe and film frames on the phone (device-perf engineer)

Full list and numbers: `docs/engine/device-results/find-n5.md` (section 8 "Not measured, and what is left on the phone", the wave 7 and wave 8 sections: film pad drags, first-frame budgets, warm-up, the soak) and `pixel5.md` / `redmi-garnet.md` for the other phones. Start with the GPU probe line (`GpuProbe`, logcat tag `dapple_engine`: api, device, `selftest`) before any timing: a `cpu` api there means every GPU number below is meaningless.

## E. iOS (iOS engineer)

`docs/ios/README.md` ("How to build", "Device install") and the wave 8 additions checklist at its end (libjxl / Highway / no-OpenMP X-Trans); Metal numbers in `docs/ios/metal-backend.md`.
