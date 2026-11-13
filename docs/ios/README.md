# Dapple on iOS

Status 2026-11-13 (round 2): the app builds for the iOS simulator and for devices (arm64, iOS 16+) and runs on the simulator, an iPhone XR (A12, iOS 18.7.10) and an iPhone X (A11, iOS 16.7.16). The engine runs on **Metal on both phones**
(317 GPU frames in a measurement run, 0 failures), RAW (ARW, RAF, DNG) is developed on the phones, the film pads / strength / map editor works on the iPhone X screenshot, the library has a PhotoKit implementation, the picture can be shown
through a zero-copy Flutter texture. Not verified: PhotoKit end to end (needs a touch on the permission prompt), the editor's own use of the texture on screen, full-resolution export on the phones.

Details: `device-results.md` (numbers, A11 vs A12), `metal-backend.md` (HAL, timestamps, presenter, M1 profile, hunks for the executor files), `platform-interface.md` (Dart seam), `swift-designs.md` (capture, wireless transfer, codec layer),
`neon-candidates.md` (CPU kernels).

## What the owner has to do by hand

1. **Answer the Photos permission prompt on both iPhones** (a freshly installed app asks on first launch; nobody can tap it from the PC). 选择照片 / 限制访问 tests limited access, 允许完全访问 full access. Until then the library shows the "允许访问" banner; Files import and the
   photo picker (`PHPicker`, needs no permission) work regardless. The prompt on the XR shows thumbnails of the real library (the screenshot taken with it on screen was deleted).
2. **Re-sign and reinstall every 7 days** (free Apple ID: both profiles expire 2026-11-19, `docs` below: `scripts/ios/device-test.sh`), until the paid programme exists.
3. **App Store / TestFlight needs a decision from the owner**: the paid Apple Developer Program, and a Mac on macOS 15.6 or later with Xcode 26 (this Mac is on 14.7.6 / Xcode 16.2): Apple requires uploads since 2026-04-28 to be built with Xcode 26 and the iOS 26 SDK
   (https://developer.apple.com/news/upcoming-requirements/, read 2026-11-13; Xcode 26 needs macOS Sequoia 15.6+ per https://developer.apple.com/xcode/system-requirements/). An OS upgrade of the shared Mac is his call.

## Build and run (everything on the Mac under `~/dapple`, driven from this PC)

```
scripts/ios/bootstrap.sh              # on the Mac once: cmake, ninja, Flutter 3.47.8, LunarG SDK (glslc), SPIRV-Cross / Vulkan header sources (versions: ~/dapple/tools/VERSIONS.txt)
scripts/ios/sync-worktree.sh          # PC -> Mac: the CURRENT working tree (tracked + untracked, ignored files excluded) + tools/shaderc, files extracted with fresh mtimes; sync.sh is the older HEAD-snapshot variant
scripts/ios/mac.sh '<cmd>'            # run a command on the Mac with the toolchain on PATH
scripts/ios/build-engine.sh macos|ios-sim|ios-device [on|off]    # CMake builds (GPU on = Metal); adds HWY_DISABLED_TARGETS (no SVE on Darwin)
scripts/ios/build-xcframework.sh      # device + simulator -> app/native_ios/dapple_engine/ios/dapple_engine/Frameworks/dapple.xcframework
cd app && flutter build ios --simulator --debug --no-codesign         # simulator app
cd app && flutter build ios --release --no-codesign [--dart-define=DAPPLE_IOS_AUTOTEST=true]; scripts/ios/make-ipa.sh   # device ipa (unsigned)
scripts/ios/sim-run.sh | device-run.sh <xr|x> <ipa> <prefix> | device-editor.sh <xr|x> <name> <stock>      # simulator / phone test cycles
```
Packaging: Swift Package Manager (no CocoaPods), the engine as a dynamic `dapple.xcframework` (arm64 device + arm64 simulator, no x86_64 simulator slice), the local plugin `app/native_ios/dapple_engine` (Swift: GPU start-up, PhotoKit, zero-copy view),
FFI through `DynamicLibrary.process()` with a fallback to `dapple.framework/dapple`. Deployment target 16.0. Mac CPU suites passed 59/59 (engine, film, golden skipped by design, selfcheck 0 differences) and the Metal GPU suites 69/69 (round 1 tree);
the round 2 tree could not be rebuilt on macOS because of an in-flight edit of the DNG engineer (`core/src/decoders/libraw_mosaic.inc`: `raw.COLOR()` on a const `LibRaw`, `S.fuji_width` missing from the pinned LibRaw snapshot) — see "Blockers".
Dart tests on the Mac (current tree, `DAPPLE_LIB` = host engine): 216 pass, 1 skipped, 1 fails (`tools_test.dart` Motion Photo tool window, not iOS related).

## Device install (free Apple ID, testing only; owner's own signing)

Build the unsigned ipa on the Mac, copy it to `build/ios/`, sign on the PC with the autotest framework's `plumesign` and its saved session (`APPDATA=%LOCALAPPDATA%\\autotest-framework\\ios\\plume`, `sign -p x.ipa --apple-id --custom-identifier <id> -o signed.ipa`), install with its
`pymobiledevice3` (`apps install`; **uninstall first**: an install over an existing app hung the installd coordinator), launch with `developer dvt launch`, screenshot with `developer dvt screenshot`, push files with `apps push <bundle> <local> Documents/...`.
Bundle ids `com.lumi.dapple.00008020` (XR) and `com.lumi.dapple.26d2a524` (X); free-account limits: profiles and device registrations expire after 7 days (2026-11-19), 10 App IDs per 7 days (**4 are in use** now: 2 for the autotest framework's WDA, 2 for Dapple), 3 sideloaded apps per device.

## Stubbed / missing

Wireless camera transfer, codec layer (VideoToolbox), Live Photo tools: designs with the API mapping in `swift-designs.md`. The Dart stubs (`app/lib/platform/ios_services.dart`) fail gracefully. Haptics go through Flutter.

## Camera capture (done 2026-11-13)

`DappleCapturePlugin.swift` (channel `dapple/capture`, result maps like Android: `capture` -> `{jpg?, dng?, thumb?, source:"avfoundation"}`; extra calls `available`, `permission`, `openSettings`, `capabilities`, `autoShutter` for test runs) + `app/lib/platform/ios_capture.dart` (`IosCaptureBackend`, wired by `IosPlatformServices` and by `AppState.captureBackend = PlatformServices.current.capture`). The capture screen is a native full-screen view controller (AVCaptureVideoPreviewLayer, portrait), `capture_flow.dart` / `ingestCapture` are shared with Android. Ladder RAW+JPEG -> RAW -> JPEG per lens, remembered per lens id + OS build (`UserDefaults`), ProRAW preferred over Bayer when the output supports it, flash off, orientation from the device, tap to focus / expose, vertical drag = exposure compensation, GPS only from an already granted location permission, settle = exposure / focus / white balance stop adjusting (min 0.7 s after start, 2.5 s timeout), interruptions and runtime errors handled; camera denied / restricted -> toast with Settings. The simulator reports `available=false` (no button). Capabilities per phone: `build/ios/capture/*_capture_caps.json`.
Test drive: build with `DAPPLE_IOS_AUTOTEST=true`, push `Documents/DAPPLE_OPEN` = `{"capture":true,"documents":false,"noopen":true,"autoMs":6000,"save":false,"mode":"raw"}` (mode optional, one capture per app launch, `save:false` keeps the shot out of Photos); the shot log is `Documents/capture_log.txt`, capabilities `Documents/capture_caps.json`. Real shots through the button save the DNG + JPEG into the Photos album "Dapple" (Android parity); `queryNew` now skips that album so the shots are not ingested twice.

## Driving the UI from the PC (WebDriverAgent)

`python -m src.devices.ios_setup <udid>` (autotest framework) starts WDA; `pymobiledevice3 usbmux forward --serial <udid> 8101 8100`; `scripts/ios/wda.py 8101 launch|source|tap|alert|shot` (set `PYTHONUTF8=1`). Answer only Dapple's own prompts (Camera, Photos, and the China "wireless data" prompt). Stop with `ios_setup <udid> --stop-wda` and kill the forward. Over-installing Dapple hangs on both phones: uninstall the app's own bundle id first.

## Blockers and things not verified

* macOS rebuild of the round 2 tree (above); the Mac Metal results of round 1 stand. The film-kernel test `film.develop_film` failed on arm64 in the round 2 tree (grain knob 150 / 200 saturates at the 100 value): probably the in-flight film work, not checked on x86.
* `engine.gpu_external` is disabled on Apple (Vulkan swapchain handles); the Metal equivalents are `metal.external` and the `GpuViewBackend` probe.
* Editor integration of the texture: `EditorEngine._maybeStartGpu` starts the view after its first readback frame; no `GpuViewBackend` log line appeared in the simulator editor session while the probe worked in the same app.
* PhotoKit listing / copy / save / picker: compiled and the permission prompt is the real one; the listing could not be exercised because the simulator ignores `simctl privacy grant photos` for PhotoKit and the phones need a touch.
* Thermal / sustained load on A11 / A12; 24 - 33 MP export on the phones; A11 / A12 `MTLBinaryArchive` warm start.

## Plan for the Swift side (remaining)

1. Codec layer on VideoToolbox + AVFoundation (probe + report first, then Live Photo), 2. capture (ProRAW / Bayer RAW), 3. ImageCaptureCore (USB tethering) then a foreground FTP listener, 4. `PresentExt` polish (resize / trim under memory pressure, editor use),
5. A11 / A12 tuning (`DAPPLE_FILM_V3_DIRECT=0` rule for Apple, half-precision accumulators behind a spec constant, binary archive), 6. NEON versions of the three hot CPU kernels (`neon-candidates.md`), 7. device build in CI (Mac) with the unsigned ipa as the artefact.

## Risks

* A11 is 2.3x slower than A12 on film frames (235 ms final at 1600 px): the interactive tier must keep the A11 inside the frame budget with `AdaptiveQuality`; 3 GB RAM on both phones (peak 620 MB for a 33 MP RAW session, measured).
* iOS background limits (no server in the background; exports must finish in the foreground or a `BGTaskScheduler` task). * Free-account signing breaks every 7 days and depends on a third-party tool (`plumesign`). * The `gpu_context.cpp` /
  `engine_session.cpp` status strings are correct for Metal now; `Core::create` and the envMode default still need the hunks in `metal-backend.md`.
* PhotoKit has no "date added": the 7-day first-run window applies to the creation date; older photos come in through the picker.

## Shared files touched (list)

New: `core/engine/hal/metal/*`, `core/engine/present/metal_presenter.mm`, `cmake/apple_framework*.{cmake,plist.in}`, `scripts/ios/*`, `docs/ios/*`, `app/native_ios/dapple_engine/**`, `app/lib/platform/*`, `scripts/ios/wda.py`.
Hunks in existing files: `core/engine/hal/hal.cpp`, `select.cpp` (Metal first on Apple), `core/gpu/CMakeLists.txt` (`if(APPLE)`), `core/engine/present/null_presenter.cpp` (`!__APPLE__`), `present.cmake` (Apple presenter sources + frameworks), `tools/shaderc/main.cpp` (MSL table),
`app/pubspec.yaml` (plugin dependency), `app/lib/main.dart` (two hook lines), `app/lib/native/dapple_bindings.dart` (iOS library lookup), `app/lib/services/ingest.dart`, `media_permission.dart`, `gpu_view_backend.dart` (iOS branches),
`app/lib/state/app_state.dart` (`captureBackend` from `PlatformServices.current.capture`; three gates: `refreshPermission`, `requestPermission`, `scan`), `app/ios/Runner/Info.plist`, `Runner.xcodeproj` (deployment target), `app/ios/Flutter/*.xcconfig`.

## Demosaic kernels: what to check on the next Mac build (demosaic engineer, 2026-11-13)

The Demosaic node (docs/engine/pipeline-architecture.md section 32) adds shaders `core/engine/shaders/demosaic_{scale,bayer,xtrans,xfast}.comp` (pipelines `dm_*` in `core/engine/nodes/demosaic/demosaic_gpu.cmake`, appended to the Vulkan list and to `hal/gles/shader_list.cmake`). Nothing was compiled for Metal (no Mac). They are written to translate through `dapple_shaderc --msl` without surprises: no dynamic swizzle / vector indexing (explicit selects, `comp(vec4, int)`), no constant arrays in the default kernels (`demosaic_bayer.comp`, `demosaic_xtrans.comp`; the CPU-parity "fast" X-Trans tier `demosaic_xfast.comp` still has `int[4]` direction tables and a local array in its median, which MSL accepts but ANGLE's HLSL output does not), uniform members read as plain `uint` / `float` components (the CFA table is one `uvec4`, selected by `.x/.y/.z`), storage buffers only (`float`, `uint`, `vec4` arrays) plus one write-only `rgba16f` / `rgba32f` image, spec constants 0 / 16 / 17.
Checklist: (1) `dm_scale`, `dm_bg`, `dm_bo16`, `dm_x0 ... dm_x4_16` compile and load (`DAPPLE_GPU_API` is irrelevant here: `GpuDemosaic` uses the `hal::Device` directly); (2) `dapple_demosaic_tests gpu_parity gpu_tiles gpu_real wire_gpu` pass (mean dE2000 <= 0.1, tiles 0 differing); (3) the shader with five buffers does not exist: the largest has 4 storage buffers (`dm_x2`, `dm_x3`), under Metal's and GLES' limits; (4) bound image `rgba16f` write-only tile: the backend of the decoder already picks tiles of 512 on Android and Apple devices (the desktop default of 1024 uses 70 MB of buffers for the candidate planes); (5) `demosaic_xtrans.comp` `MODE == 3` reads and overwrites its own pixel of the same buffer (`Dv`): if MSL's `device` buffer qualifiers complain about `readonly` aliasing, split it into two buffers.
