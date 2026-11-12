# Swift side: designs of the parts that are still stubs (capture, wireless transfer, codec layer)

Status of the Swift plugin `app/native_ios/dapple_engine` (`DappleEnginePlugin.swift`, `DappleViewPlugin.swift`): engine start-up (GPU probe), PhotoKit library
(`dapple/media`, `dapple/photos`, `dapple/media_events`), zero-copy display (`dapple/view`). Everything below keeps the channel names and result maps of the Kotlin
side, so the Dart classes in `app/lib/services/*` do not change; the iOS stubs in `app/lib/platform/ios_services.dart` are swapped for channel clients.

## 1. Phone camera capture (Android: `CaptureActivity.kt` + `CapturePlugin.kt`, channel `dapple/capture`)

Android behaviour: CameraX preview, one shutter press returns `{jpg, dng?, source}`; capture mode RAW+JPEG -> RAW -> JPEG per lens, remembered, with automatic
re-shoot when a mode stalls (RAW_TIMEOUT 10 s); RAW-only shots keep the viewfinder frame as a JPEG thumbnail; results go to the inbox; `saveToGallery(path, name, mime)`.

| Kotlin | iOS (AVFoundation) |
|---|---|
| `CameraSelector` (back lens), lens switch | `AVCaptureDevice.DiscoverySession` (wide / ultra-wide / tele), `AVCaptureDeviceInput` |
| `ImageCapture` OUTPUT_FORMAT_RAW_JPEG / RAW / JPEG | `AVCapturePhotoOutput`: `appleProRAWEnabled` (A12 Bionic and newer on ProRAW phones only: 12 Pro and later) -> `availablePhotoPixelFormatTypes` with `AVCapturePhotoOutput.isAppleProRAWPixelFormat`, else Bayer RAW `availableRawPhotoPixelFormatTypes` (`isBayerRAWPixelFormat`), else JPEG/HEIC; `AVCapturePhotoSettings(rawPixelFormatType:processedFormat:)` for RAW+JPEG |
| DNG file | `AVCapturePhoto.fileDataRepresentation()` of the RAW photo is already a DNG; processed photo -> JPEG (`AVVideoCodecType.jpeg`) |
| mode fallback RAW+JPEG -> RAW -> JPEG, remembered per camera id and OS build | same ladder, key `AVCaptureDevice.uniqueID` + `UIDevice.systemVersion`, `UserDefaults` |
| camera permission (`Manifest.CAMERA`), `CapturePerm {granted, denied, blocked}` | `AVCaptureDevice.authorizationStatus(for: .video)` -> granted / denied (`.notDetermined` = ask) / blocked (`.denied`, `.restricted`: only Settings helps, `openSettings` = `UIApplication.openSettingsURLString`) |
| preview | `AVCaptureVideoPreviewLayer` in a presented `UIViewController` (same one-shot full screen flow as `CaptureActivity`); no texture needed |
| `saveToGallery` | already implemented by `dapple/media saveToGallery` (PhotoKit) |

Channel `dapple/capture`: `capture` -> `{jpg, dng?, source:"avfoundation"}` or null when cancelled; `saveToGallery`; `sdkInt` is Android only (the Dart side returns 0 off Android).
Info.plist: `NSCameraUsageDescription` (already present). Needs a real phone (no camera in the simulator): `available` = `AVCaptureDevice.default(for: .video) != nil`.
Risks: ProRAW files are 25-75 MB and large (the engine's DNG path handles them, memory per `docs/ios/device-results.md`); `appleProRAWEnabled` must be set after the session is configured;
the Android "RAW takes 3-10 s" watchdog is not needed (RAW delivery is one callback).

## 2. Wireless camera transfer (Android: `FtpService.kt`, `FtpServer.kt`, channels `dapple/ftp`, `dapple/ftp_events`)

Android behaviour: a foreground service runs a small upload-oriented FTP server (RFC 959 + EPSV + MLSD/SIZE/MDTM subset: USER PASS PWD CWD TYPE PASV EPSV PORT EPRT LIST/NLST/MLSD STOR/APPE/STOU SIZE MDTM MKD RNFR/RNTO; RETR / DELE refused)
on a configurable port (default 2121) with a user/password; the camera (Sony / Fujifilm / Nikon FTP transfer) pushes files into an inbox; events `clients start progress done failed state`; status = running, clients, port, addresses (hotspot detection by interface name), error.

iOS constraints: no foreground service; an app has about 30 s after it goes to the background (`UIApplication.beginBackgroundTask`), then it is suspended and its sockets stop. The listener can only serve while the app is in the foreground
(keep the screen awake with `isIdleTimerDisabled` while the transfer screen is open, `UIBackgroundModes` does not include a legitimate mode for an FTP server). Local network: iOS 14+ shows the Local Network prompt
(`NSLocalNetworkUsageDescription`, `NSBonjourServices` only if Bonjour is used).

| Kotlin | iOS |
|---|---|
| `ServerSocket` + threads | `Network.framework` `NWListener` (TCP, port from `start`), one `NWConnection` per control connection; PASV / EPSV data listeners on ephemeral `NWListener`s; Swift concurrency per connection |
| command table in `FtpServer.kt` | port it 1:1 (it is 300 lines of line-oriented protocol); state machine unchanged, UTF-8 `FEAT` reply identical |
| inbox directory | app `Documents/ftp_inbox` (visible in Files); `listInbox` = directory listing; `done` event after `STOR` completes (size check vs `SIZE` when the camera sends it) |
| addresses (`wlan0`, hotspot `ap0`) | `getifaddrs` (`en0` Wi-Fi, `bridge100` personal hotspot) |
| `status`, `events` stream | `dapple/ftp` MethodChannel + `dapple/ftp_events` EventChannel with the same maps |
| camera apps that drop files in the gallery (Sony Creators' App, Imaging Edge Mobile, Fujifilm XApp) | already covered by PhotoKit ingest (they save to Photos); this is the main iOS path |

Alternative for tethered cameras: `ImageCaptureCore` (`ICDeviceBrowser`, USB PTP) lists and downloads from a camera attached by Lightning/USB-C (no server needed); `dapple/media_events` can carry its arrivals.
Recommendation: ship PhotoKit first (done), then ImageCaptureCore (USB, reliable), then the FTP listener as foreground only.

## 3. Codec capability layer (Android: `CodecCatalog.kt`, `CodecLadder.kt`, `CodecSelfTest.kt`, `CodecManager.kt`; `docs/engine/codec-capability.md`)

Principle is unchanged: codecs adapt to what the device declares, verified by a one-time self-test, nothing hard-coded by model.

| Android concept | iOS mapping |
|---|---|
| `MediaCodecList` query: codecs, profiles, colour formats, size range, frame-rate, hardware / software | `VTCopyVideoEncoderList`, `VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC / _H264 / _AV1)`, `VTCopySupportedPropertyDictionaryForEncoder`, `VTSessionCopySupportedPropertyDictionary`; `AVAssetExportSession.allExportPresets()` for HEVC/Apple ProRes availability; decoders: `VTDecompressionSessionCreate` with `kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder` as the probe |
| Decode ladder 1: 8-bit 4:2:0 hardware | `VTDecompressionSession` (H.264 / HEVC) -> `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` |
| Decode ladder 2: 10-bit hardware -> P010 | `kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange` (this is P010); HEVC Main10 hardware decode is available on A10 and newer |
| Decode ladder 3: FFmpeg software (4:2:2 10-bit Sony, 4:4:4) | the same FFmpeg build as Android (`core/video`, LGPL, dynamic) rebuilt for iOS as a dynamic framework; Apple's hardware decoders do not take 4:2:2 H.264 / most S-Log footage on all devices: the self-test decides |
| Encode ladder 1: HEVC Main10 + P010 input, HLG/PQ tags | `VTCompressionSession` HEVC with `kVTProfileLevel_HEVC_Main10_AutoLevel`, input `...10BiPlanarVideoRange`, colour primaries / transfer / matrix attachments (`kCVImageBufferTransferFunction_ITU_R_2100_HLG`) |
| ladder 2: HEVC Main 8 bit; ladder 3: H.264 High | `kVTProfileLevel_HEVC_Main_AutoLevel`, `kVTProfileLevel_H264_High_AutoLevel`; `AVAssetWriter` for the container (MP4/MOV) |
| self-tests `enc10 / enc8hevc / enc8avc / dec10`, cache keyed by `Build.FINGERPRINT` | same tests on synthetic frames (640x352), results in `UserDefaults` keyed by `UIDevice.systemVersion` + machine (`uname`), purged when the OS build changes; stall watchdog = a timeout on `VTCompressionSessionEncodeFrame` callbacks |
| `CodecManager.report()` JSON on `dapple/live` `codecReport` | same JSON keys (declared capabilities, ladders, self-test results) from Swift; `capabilities` -> `{main10Encoder, ultraHdr:false, videoLib}` |
| Motion Photo (Android) <-> Live Photo (iOS) | Live Photo = still + paired MOV with the same content identifier (`com.apple.quicktime.content.identifier` metadata in both): `PHAssetCreationRequest` with `.photo` + `.pairedVideo` resources; reading: `PHAssetResourceType.pairedVideo` through the same `copyToFile` |
| `LiveBridge` trim / frame / thumbs | `AVAssetReader` + `AVAssetImageGenerator` (frames, thumbs), `AVAssetExportSession` / `AVAssetWriter` (trim) |

Order of work: probe + report (read-only, one day), decode ladder 1-2 for Live Photo clips, encode ladder, then Live Photo writing. The software rung needs the FFmpeg iOS build (`tools/ffmpeg` scripts are Android / host only today).
