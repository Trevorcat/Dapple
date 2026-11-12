# Platform interface (Dart) for the Android-only native features

Code: `app/lib/platform/platform_services.dart` (interfaces, Android implementation, graceful stubs), `app/lib/platform/ios_services.dart`
(iOS implementation). Nothing in `app_state.dart` uses it yet (see "Wiring" below): the existing channel classes stay the Android
implementations, unchanged.

`PlatformServices.current` returns one object per process (`android`, `ios`, or `host` for desktop tests; replaceable in tests):

| Area | Interface | Android today (Kotlin, channel) | iOS today | iOS plan (Swift) |
|---|---|---|---|---|
| Photo library / ingest | `MediaLibrary` (`source` = `IngestSource`, `permissionGranted/requestPermission`, `copyToFile`, `saveToLibrary`) | MediaStore (`dapple/media`: `queryNew`, `copyToFile`, `saveToGallery`) | **works**: `Documents/Photos` folder (visible in Files, "On My iPhone / Dapple"); `saveToLibrary` returns false | PhotoKit (`PHPhotoLibrary`, limited-library aware, album "Dapple", RAW via `PHAssetResource`), keep Documents as a second source |
| Phone camera capture | `CaptureBackend` (`available`, `permission`, `capture`, `saveToGallery`) | CameraX + DNG (`dapple/capture`) | `NullCaptureBackend`: `available == false`, `capture()` returns null, permission `denied` | AVFoundation: `AVCapturePhotoOutput` with ProRAW (`appleProRAWEnabled`) or Bayer RAW (`availableRawPhotoPixelFormatTypes`), DNG written by `fileDataRepresentation()` |
| Camera -> phone transfer | `FtpBackend` (`start/stop/status/listInbox/events`) | FTP server service (`dapple/ftp`, `dapple/ftp_events`) | `NullFtpBackend`: `status().error == 'unsupported'`, no events | Network.framework FTP/PTP-IP listener in a background-capable task (iOS limits: no long-running server in background); or Image Capture / USB `ImageCaptureCore` for tethered |
| Video / codec layer | `VideoPlatform` (`available`, `listVideos`, `probe`, `codecReport`, `capabilities`) | `LiveBridge` + CodecLadder (`dapple/live`) | `NullVideoPlatform`: empty results, `available == false` | VideoToolbox (`VTDecompressionSession`, `VTCompressionSession`: HEVC Main10 / H.264) + AVAssetReader/Writer; ladder = same rungs, self-test cached per OS build; Live Photo pairing through PhotoKit |
| Share | `ShareService` (existing) | share sheet, open app by package | `share_plus` (UIActivityViewController) already works; `openApp` returns false | URL schemes allow-list in Info.plist (`LSApplicationQueriesSchemes`) |
| GPU view | `gpuView` flag (`dapple/view` texture) | Vulkan swapchain into a Flutter `SurfaceProducer` | false: readback path (`EditorEngine`) | `FlutterTexture` backed by an IOSurface/CVPixelBuffer, drawn by `hal::CommandList::copyImageToExternal` (already implemented in the Metal HAL: RGBA8 blit, BGRA8 swizzle kernel) |
| Haptics | `Haptics` (existing, `dapple/haptics`) | Kotlin | Flutter `HapticFeedback` already used on iOS | optionally `UIImpactFeedbackGenerator` |

## Contract for implementers

1. A feature shows its entry point only when `<area>.available` / `permission` allow it; calling anyway must not throw: stubs return
   the neutral value in the table (empty list, `null`, `false`, empty map / string).
2. Paths, not URIs, cross to the engine (FFI takes file paths). `MediaLibrary.copyToFile` is the conversion point (Android
   `content://`, iOS PhotoKit asset -> file in the app sandbox).
3. Channel names stay `dapple/<area>`; a Swift implementation registers the same method names and result maps as the Kotlin one (the
   Dart classes in `app/lib/services/*` are the specification; see their `invokeMethod` calls).
4. Errors: platform exceptions are caught on the Dart side where the existing classes do (`catch (_)` in `LiveBridge`, `ShareService`).

## Wiring (not done: files owned by others)

`app_state.dart` still guards ingest / permission / FTP / Live with `Platform.isAndroid`, so on iOS the library screen is fed only by the
picker import (`importFromPicker`, system document picker, works). To switch iOS to `PlatformServices.current.media.source`, replace the
guards in `refreshPermission`, `requestPermission`, `scan` by `PlatformServices.current.media` calls and make `mediaSource` come from
it. `IosDebugOpen` (debug builds, `app/lib/platform/ios_debug_open.dart`) feeds `Documents/Photos` into the library through
`ingestFtpFiles` for the simulator tests.
