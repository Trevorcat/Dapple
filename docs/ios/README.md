# Dapple on iOS

Status 2026-11-13: the C++ core builds natively on Apple silicon (CPU and Metal), the Flutter app builds for the iOS simulator and for
devices (arm64, iOS 16+), runs on the simulator, an iPhone XR (iOS 18.7.10) and an iPhone X (iOS 16.7.16). On the simulator a synthetic
photo was developed by the engine and shown in the editor. **Not done**: everything native on the Swift side (see "Stubbed").

Details: `metal-backend.md` (GPU, numbers), `platform-interface.md` (Dart seam for Android-only features).

## What works

- Core, native macOS arm64: no source change was needed. `ctest` 59 / 59 (CPU build): engine, film, film_nodes, exec, selfcheck. Golden corpus
  (recorded for x86 AVX2) is skipped by design ("corpus recorded on arch=x86_64"); `dapple_golden selfcheck off full`: 0 differences. NEON: there is
  no hand written NEON or intrinsics anywhere (`film_kernels_base.cpp` is portable scalar code, the AVX2 kernels are x86 only and not built
  here); the base kernels compile to NEON through the auto-vectoriser (about 1330 `.4s` vector instructions in `film_kernels_base.o`, 24 in `film_spatial.o`).
  No NEON specific tuning or measurement was made.
- Metal HAL + GPU suites on the M1: see `metal-backend.md`.
- Engine for iOS: `dapple.framework` (dynamic framework, `cmake/apple_framework.cmake`), built for device and simulator and merged into
  `dapple.xcframework` (arm64 device + arm64 simulator slice; no x86_64 simulator slice, so Intel Macs are not supported for simulator runs).
- Flutter app: `app/ios` (Runner, Swift Package Manager integration, **no CocoaPods needed**), local plugin `app/native_ios/dapple_engine`
  (pubspec path dependency, `Package.swift` with a binary target on the xcframework). FFI: `dapple_bindings.dart` uses `DynamicLibrary.process()` and falls
  back to `DynamicLibrary.open('dapple.framework/dapple')`. The bundled film pack and assets come through Flutter assets. Deployment target 16.0.
- Dart test suite on the Mac (`flutter test`, `DAPPLE_LIB` = host engine): 217 pass, 6 fail. All 6 are in `editor_film_test.dart` /
  `editor_panel_test.dart`, the old editor UI that is being replaced in the working tree: missing golden PNGs (`test/goldens/editor_v2/*` are untracked) and text-overflow
  differences with the Mac's fonts. Not iOS related.
- Simulator: `build/ios/sim_editor.png` (editor on the sample photo, engine render), `build/ios/sim_style.png` (style panel). Devices: `build/ios/device_xr_1.png`,
  `build/ios/device_x_1.png` (library screen after launch of the release build; no photo was opened on the phones).

## How to build (everything on the Mac under `~/dapple`, driven from this PC)

```
scripts/ios/bootstrap.sh            # on the Mac once: cmake, ninja, Flutter 3.47.8, LunarG SDK (glslc only), SPIRV-Cross / Vulkan headers sources  -> ~/dapple/tools, versions in tools/VERSIONS.txt
scripts/ios/sync.sh                 # PC -> Mac: git archive HEAD (core.autocrlf=false) + the files listed in scripts/ios/overlay.txt
scripts/ios/mac.sh '<cmd>'          # run a command on the Mac with the toolchain on PATH
scripts/ios/build-engine.sh macos|ios-sim|ios-device [on|off]     # CMake builds (GPU on = Metal)
scripts/ios/build-xcframework.sh    # device + simulator -> app/native_ios/dapple_engine/ios/dapple_engine/Frameworks/dapple.xcframework
cd app && flutter build ios --simulator --debug --no-codesign     # simulator app
cd app && flutter build ios --release --no-codesign               # device app, then scripts/ios/make-ipa.sh -> unsigned .ipa
scripts/ios/sim-run.sh <prefix> [group]   # headless simulator (own device "dapple-iphone"), installs, opens the sample photo, screenshots in ~/dapple/out
```
Mac tests: `build-engine.sh macos on` then `ctest` in `~/dapple/build/macos` (CPU + Metal suites, 69 / 69).
The Apple build needs `-DDAPPLE_GLSLC=<glslc>` (the Android NDK one is Windows only); `build-engine.sh` passes the LunarG SDK's.

Why SPM + an xcframework of a dynamic framework: Xcode embeds and signs it for device builds, no force-load / strip tricks for `dlsym`, no CocoaPods
installation, App Store accepts embedded dynamic frameworks. The simulator needs `EXCLUDED_ARCHS[sdk=iphonesimulator*] = x86_64` (set in `app/ios/Flutter/*.xcconfig`).

## Shared files touched (list)

New: `core/engine/hal/metal/*`, `cmake/apple_framework*.{cmake,plist.in}`, `scripts/ios/*`, `docs/ios/*`, `app/native_ios/dapple_engine/**`,
`app/lib/platform/*`. Hunks in existing files: `core/engine/hal/hal.cpp` (Api::Metal case, `#if DAPPLE_HAL_METAL`), `core/engine/hal/select.cpp` (`DAPPLE_GPU_API=metal`),
`core/gpu/CMakeLists.txt` (`if(APPLE) include(metal.cmake)`), `tools/shaderc/main.cpp` (MSL table output; untracked tool dir), `app/pubspec.yaml` (one dependency),
`app/lib/main.dart` (one line), `app/lib/native/dapple_bindings.dart` (iOS library lookup), `app/ios/Runner/Info.plist` (usage strings, file sharing), `app/ios/Runner.xcodeproj` (deployment target 16.0), `app/ios/Flutter/*.xcconfig`.

## Stubbed / missing (graceful, see platform-interface.md)

Photos library (PhotoKit), phone camera capture (ProRAW / Bayer RAW), wireless camera transfer, video / codec layer (Live Photo / Motion Photo tools), saving
to Photos, `dapple/view` zero-copy texture (readback path is used), haptics through Flutter. Library on iOS: only the picker import (document picker) works in release
builds; debug builds on the simulator can load `Documents/Photos` (`IosDebugOpen`). `app_state.dart` still gates the ingest on `Platform.isAndroid` (not edited: other engineers are in it).

## Plan for the Swift side

1. Codec layer on VideoToolbox: `VTDecompressionSession` (HEVC / H.264 / 10-bit, P010-like `kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange`), `VTCompressionSession`
   (HEVC Main10 when `VTIsHardwareDecodeSupported` / encoder list says so, else H.264), the same ladder + self-test + per-OS-build cache as `CodecLadder.kt`, reports through `dapple/live`.
2. Capture on AVFoundation: `AVCapturePhotoOutput`, ProRAW where `isAppleProRAWSupported`, else Bayer RAW (`availableRawPhotoPixelFormatTypes`), DNG via
   `fileDataRepresentation()`, camera permission flow, results as `CaptureResult` (jpeg + dng paths).
3. Wireless transfer: Network.framework listener for FTP / PTP-IP while the app is foreground (iOS does not allow a long-lived background server), Sony / Fujifilm app hand-off via
   the Photos library as on Android; tethered USB through ImageCaptureCore.
4. Photos library: PhotoKit read (`PHFetchResult`, change observer = `changes` stream, limited library), export into album "Dapple" (`PHAssetCreationRequest`), Live Photo
   pairing (`PHAssetResourceType.pairedVideo`).
5. Present: `CAMetalLayer` / IOSurface-backed Flutter texture fed by `copyImageToExternal` (HAL side exists), `PresentExt` for the view API, then drop the readback path on iOS.
6. Metal: per-pass timestamps (`MTLCounterSampleBuffer`, stage boundary), `MTLBinaryArchive` pipeline cache, tune for A11 / A12 (`readWriteTextureSupport` tier, threadgroup limits) on the phones.

## Device install (free Apple ID, testing only)

Done here with the owner's existing practice (autotest framework): build the unsigned device `.ipa` on the Mac (`scripts/ios/make-ipa.sh`, minimum iOS 16.0, no entitlements),
copy it to `build/ios/`, sign on the PC with the framework's `plumesign` and its saved Apple session, install with its `pymobiledevice3`:

```
set APPDATA=%LOCALAPPDATA%\autotest-framework\ios\plume                     (the tool's session / certificate store)
plumesign-windows-x86_64.exe sign -p build\ios\Dapple-unsigned.ipa --apple-id --custom-identifier com.lumi.dapple.<udid first 8> -o build\ios\Dapple-<n>-signed.ipa
python -m pymobiledevice3 apps install build\ios\Dapple-<n>-signed.ipa --udid <udid>          (autotest venv, read-only use)
python -m pymobiledevice3 developer dvt launch com.lumi.dapple.<udid8> --udid <udid>
python -m pymobiledevice3 developer dvt screenshot out.png --udid <udid>
```
Bundle ids `com.lumi.dapple.00008020` (XR) and `com.lumi.dapple.26d2a524` (X). Limits of a free account: provisioning profile and device registration expire after 7 days
(the XR build above expires 2026-11-19, the X build 2026-11-19), 10 App IDs per 7 days (this used 2 more), 3 sideloaded apps per device. The iPhone X was not registered in the account any more
(its registration had lapsed), so `plumesign account register-device` was run for it, as the framework does. **This route is for testing only and must be repeated every 7 days.**

App Store / TestFlight needs the paid Apple Developer Program, and a newer toolchain than this Mac has: Apple's upcoming-requirements page
(https://developer.apple.com/news/upcoming-requirements/, read 2026-11-13) says that since April 28, 2026 apps uploaded to App Store Connect must be built with Xcode 26 or later using the iOS 26 SDK; Xcode 26 needs
macOS Sequoia 15.6 or later (https://developer.apple.com/xcode/system-requirements/), the Mac has macOS 14.7.6 and Xcode 16.2, so an OS upgrade (owner decision) is required first. The same page says uploads must target iOS 13 or later
since September 9, 2026 (this app targets 16.0).

## Risks

- A GPU that is 3x slower than the desktop card on film frames (M1) and an A11 / A12 (iPhone X / XR) that is slower again: interactive film frames need measuring on the phones; the CPU path stays as fallback.
- iOS background limits (transfer server, long exports): needs `BGTaskScheduler` / foreground-only design.
- Memory: jetsam limits (about half of RAM on 3 GB phones: iPhone X has 3 GB, XR 3 GB); the 512 MB transient pool and 24 MP source residency must be budgeted per device (`poolBudget`).
- The `gpu_context.cpp` / `engine_session.cpp` strings still say "vulkan" for Metal (alias); the zero-readback present is not wired.
- SPM plugin integration is newer than CocoaPods: plugins without SPM support (none among the current ones) would need CocoaPods.
- Free-account signing breaks after 7 days; Apple may change the sign-in protocol (`plumesign` is third-party).
- Flutter debug builds of the app run only from Xcode / flutter tooling on a device; simulator debug builds are fine.
