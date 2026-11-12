# Platform interface (Dart) for the Android-only native features

Code: `app/lib/platform/platform_services.dart` (interfaces, Android implementation, graceful stubs), `app/lib/platform/ios_services.dart`
(iOS implementation). Nothing in `app_state.dart` uses it yet (see "Wiring" below): the existing channel classes stay the Android
implementations, unchanged.

`PlatformServices.current` returns one object per process (`android`, `ios`, or `host` for desktop tests; replaceable in tests):

| Area | Interface | Android today (Kotlin, channel) | iOS today | iOS plan (Swift) |
|---|---|---|---|---|
| Photo library / ingest | `MediaLibrary` (`source` = `IngestSource`, `permissionGranted/requestPermission`, `copyToFile`, `pick(inbox)`, `saveToLibrary`) | MediaStore (`dapple/media`: `queryNew`, `copyToFile`, `saveToGallery`), document picker | **PhotoKit** (`DappleEnginePlugin.swift`): full and limited access, listing by creation date, RAW / ProRAW DNG preferred over the JPEG of a pair, HEIC converted to JPEG on copy, save into album "Dapple", change notifications, system photo picker (`PHPicker`, no permission) for import | optional: ImageCaptureCore (USB tethering) as a second source |
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

## Wiring (done in `app_state.dart` / `ingest.dart` / `media_permission.dart`, three one-line gates + the iOS branches)

* `app_state.dart`: `refreshPermission`, `requestPermission` and `scan` accept `Platform.isIOS` next to `Platform.isAndroid` (the FTP / Live / MotionPhoto gates stay Android only).
* `media_permission.dart`: on iOS `isGranted()` / `request()` call `dapple/photos` (`authorizationStatus`, `requestAuthorization`; `limited` counts as granted, `MediaPermission.presentLimitedPicker()` widens the selection).
* `ingest.dart`: `MediaStoreIngestSource.scan` / `changes` also run on iOS (same channel names, PhotoKit behind them); `PickerIngestSource.pick()` uses `PHPicker` on iOS and copies the originals (RAW first) into the inbox.
* `IosDebugOpen` / `IosAutotest` (`app/lib/platform/`): simulator / device test hooks only (marker file `Documents/DAPPLE_OPEN`, `--dart-define=DAPPLE_IOS_AUTOTEST=true`).

Behaviour to know: PhotoKit has no "date added", the listing filters on the creation date, and the existing first-run rule (last 7 days) applies; older photos come in through the picker import. "Limited" access lists only the photos the user chose.
The system permission prompt shows when the app first asks (Settings -> 许可): it can only be answered by touch. `Documents/DAPPLE_OPEN` or `autotest.json` suppress the prompt (test runs).
