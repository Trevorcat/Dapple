# USB tethering / wired ingest research (A7R V + X-T50)

Date: 2026-11-14. Method: web search + fetches this session. Tags: [V] = read on the cited page this session; [K] = from general knowledge, not re-checked (treat as unverified until tested); [?] = could not confirm.

## Summary

1. Both cameras can be a USB PTP/MTP device. Sony A7R V: set USB Connection Mode to MTP (card in slot 1 browsable) or Remote Shooting (PC Remote; tether). Fujifilm X-T50: only USB CARD READER (which Android sees as "USB PTP"), USB WEBCAM, USB RAW CONV./BACKUP RESTORE.
2. **X-T50 has no tethered shooting.** Its menu lists no USB TETHER SHOOTING option, and Fujifilm's Tether App chart lists it with footnote "*4: only BACKUP RESTORE available". So for the X-T50 the achievable goal is "plug in, import what is on the card" (step 1), not live push. Polling the card for new files while it is connected is the only live-ish option (needs an experiment).
3. Android: `UsbManager` + `android.mtp.MtpDevice` (API 24+: `readEvent`, `importFile(fd)`) give browse/import/ObjectAdded events with platform APIs only, no root, no third-party licences. This is enough for step 1 and for the A7R V in MTP mode.
4. iOS: ImageCaptureCore is available on iOS/iPadOS 13+ (`ICDeviceBrowser`, `ICCameraDevice`, `requestSendPTPCommand`, `ptpEventHandler`), but `requestEnableTethering` / `requestTakePicture` are macOS-only. Import and sniffing PTP events are possible on iPhone; remote capture would need hand-made PTP vendor commands.
5. Sony Camera Remote SDK: Windows/macOS/Linux only, no Android/iOS build, proprietary licence. Fujifilm Camera Control SDK: desktop, B2B, forbids GPL/LGPL linkage. Neither is usable for a phone app. libgphoto2 is LGPL-2.1 and lists both cameras but has no maintained Android/iOS port.
6. Recommended: step 1 = platform APIs only (Android MtpDevice, iOS ImageCaptureCore) with a small Dart-side `UsbIngest` mirroring `FtpIngest`; step 2 = event-driven import; step 3 = own minimal PTP client (Apache/MIT pieces such as camlib/libfuji are references) for remote control. Experiment list at the end.

Repo context [V]: requirements.md only specifies FTP reception (line ~1296 "通过 FTP 接收 RAW"; open item 7 line ~1519: RAW+JPEG transfer stability, preview-first then RAW, foreground service type, Android 16 local-network permission). There is no wired/USB requirement yet; this file is the input for one.

---

## 1. Protocols and camera modes

### 1.1 Protocol layers
- PTP (ISO 15740) is the base; MTP is a Microsoft superset; both use USB still-image class (class 6, subclass 1) with bulk-in/out plus an interrupt endpoint for events. [K] Android's `MtpDevice` doc itself says it handles "an MTP or PTP device" [V: https://developer.android.com/reference/android/mtp/MtpDevice].
- Vendor extensions (Sony, Fuji) add property codes, capture/live-view operations and proprietary events; they are not public specs. Open implementations: libgphoto2 `camlibs/ptp2` (Sony, Fuji tables) [V: https://github.com/gphoto/libgphoto2], petabyt libfuji (MIT) [V licence via GitHub API: https://github.com/petabyt/libfuji].
- Sony remote protocol over USB = "PC Remote" / Remote Shooting mode, which the Camera Remote SDK wraps; libgphoto2 handles it as the PTP vendor driver (see 1.2).

### 1.2 Sony A7R V (ILCE-7RM5)
| Item | Finding | Source |
|---|---|---|
| USB Connection Mode menu | MENU > Setup > USB > USB Connection Mode: Sel. When Connect, USB Streaming, Mass Storage (MSC), MTP, Remote Shooting | [V] https://helpguide.sony.net/ilc/2230/v1/en/contents/TP0002887823.html |
| MTP mode | Camera shows as media device; stills and movies transferable; **only memory card slot 1 is exposed** | same page [V] |
| Mass Storage | Removable disk; Android and iOS do not mount MSC cameras the way PCs do [K] (Android: no automatic MSC for host-mode without UsbMassStorage code or SAF volume mount; unverified for a camera) | [?] |
| Remote Shooting | Control from computer with Imaging Edge Desktop (Remote); Sony "Creators' App" can shoot and send images to a smartphone over USB | same page [V] |
| Save destination in Remote Shooting | MENU > Network > Cnct./Remote Sht. > Remote Shoot Setting > Still Img. Save Dest.: Destination Only / Dest.+Camera / Camera Only; RAW+J Save Image applies with Dest.+Camera and RAW & JPEG. So the camera **can keep writing to the card while tethered** (Dest.+Camera) | [V] https://helpguide.sony.net/ilc/2230/v1/en/contents/TP0003057069.html (via search summary; the page body was not opened) |
| libgphoto2 | `Sony:ILCE-7RM5 (PC Control)`, USB id 0x054c:0x0e0c, flags PTP_CAP \| PTP_CAP_PREVIEW (= capture + live view in the driver table) | [V] https://github.com/gphoto/libgphoto2/blob/master/camlibs/ptp2/library.c line 1411 |
| Camera Remote SDK | ILCE-7RM5 supported since 1.07.00 (2022-10-27); latest listed 2.02.00 (2026-06-10) | [V via search] https://support.d-imaging.sony.co.jp/app/sdk/en/ |
| Power | Separate "USB Power Supply" setting exists; whether the camera charges/powers from the phone and the effect on the phone battery: [?] |  |
- MTP ObjectAdded events while shooting in MTP mode: [?] Sony cameras in MTP mode generally do not push new-shot events for card writes; must be tested (experiment E3).
- Does the phone get RAW (.ARW, ~60+ MB, compressed/lossless variants) over MTP: yes as normal objects [K]; speed is USB 3.2 on camera side only if the cable/phone support it [K].

### 1.3 Fujifilm X-T50
| Item | Finding | Source |
|---|---|---|
| CONNECTION MODE options | USB CARD READER, USB WEBCAM, USB RAW CONV./BACKUP RESTORE. **No USB TETHER SHOOTING AUTO/FIXED.** | [V] https://fujifilm-dsc.com/en/manual/x-t50/connections/network_usb_menu/ |
| USB POWER SUPPLY/COMM | AUTO; POWER SUPPLY ON/COMM OFF (draws power, no data); POWER SUPPLY OFF/COMM ON (data only, does not draw power from host) | same [V] |
| Tether compatibility chart | X-T50 listed with footnote *4: "Only the BACKUP RESTORE function is available". Chart covers Windows/macOS only | [V] https://www.fujifilm-x.com/en-gb/support/compatibility/software/tether-app/ |
| Older/other bodies | X-T5 and X-M5 manuals have USB TETHER SHOOTING AUTO/FIXED under CONNECTION MODE | [V via search] https://fujifilm-dsc.com/en/manual/x-t5/connections/tethere/ |
| Smartphone USB (official) | Android (Type-C): CONNECTION MODE = USB CARD READER, AUTO or COMM ON; Android shows notification "Connected to USB PTP"; apps get it as MTP/PTP; third-party cable; do not use a hub; fresh battery. iOS: Lightning needs Apple Lightning to USB Camera Adapter; Photos app imports | [V] https://fujifilm-dsc.com/en-int/manual/x-t50/connections/usage_usb/ |
| libgphoto2 | `Fuji:Fujifilm X-T50` id 0x04cb:0x0306 in the table; flags PTP_CAP \| PTP_CAP_PREVIEW (copied from siblings; real X-T50 capture is contradicted by Fuji's chart) | [V] library.c line 2788 |
| Remote shooting (Fuji wired) | X-T50 not tether-capable per chart; remote control is via Fujifilm XApp over Wi-Fi/Bluetooth, outside this scope [K] | |
- So: "USB CARD READER" is a PTP/MTP session in practice (Android labels it USB PTP). Whether new-shot events or live-updated handles appear while the camera is on and shooting: [?] (E4).

### 1.4 Fujifilm wired tether on other bodies (for context)
AUTO starts tether when the computer starts; FIXED is tether-only. [V via search: https://www.fujifilm-x.com/en-gb/learning-centre/options-for-shooting-tethered/]

---

## 2. Android

### 2.1 Platform APIs
- USB host: `android.hardware.usb` (`UsbManager`, `UsbDevice`, `UsbDeviceConnection`); declare `<uses-feature android:name="android.hardware.usb.host"/>`; attach intent `UsbManager.ACTION_USB_DEVICE_ATTACHED` with a `device_filter` resource (interface class 6 = still image) launches the app; runtime permission via `requestPermission` (per device, a dialog; can be remembered for the intent-launched app). [K] Docs: https://developer.android.com/develop/connectivity/usb/host
- `android.mtp.MtpDevice` (API 12; works with PTP too): `open(UsbDeviceConnection)`, `getStorageIds`, `getObjectHandles(storage, format, parent)`, `getObjectInfo`, `getThumbnail`, `importFile(handle, ParcelFileDescriptor)` (API 24), `getPartialObject` (API 24), `readEvent(CancellationSignal)` (API 24; blocking; returns `MtpEvent`, e.g. ObjectAdded) [V: https://developer.android.com/reference/android/mtp/MtpDevice]. `importFile(handle, path)` is the older path variant (external storage path); use the fd variant for scoped storage.
- Not covered by `MtpDevice`: arbitrary PTP operations (vendor capture/property commands, GetObject with custom sizes, live view). For that use `UsbDeviceConnection.bulkTransfer` directly with your own PTP implementation (step 3) [K].
- Constraints: `MtpDevice` calls block, run on a worker/isolate-thread; `importFile` of a ~65 MB RAW is fine, but the dialog-less approach means the foreground service type must fit (connectedDevice for USB) [K: https://developer.android.com/develop/background-work/services/fgs/service-types]. Android 14+ requires the permission and a declared type for foreground services [K].
- Power/role: a USB-C to USB-C cable usually makes the phone host, but power role/data role swap depends on the phone; the Fuji manual says "POWER SUPPLY OFF/COMM ON" avoids the camera drawing from the phone [V]; the Sony equivalent [?]. A hub is discouraged by Fuji [V].
- Root: not needed. Android's own "Camera Importer"/Files MTP stack is the same API [V Fuji page].
- Flutter: no first-party plugin; need a small Kotlin plugin (MethodChannel/EventChannel) around `UsbManager` and `MtpDevice`, plus a Dart service parallel to `app/lib/services/ftp_ingest.dart`.

### 2.2 Libraries and apps (licences matter)
| Library / app | Role | Licence | Maintenance | Notes / source |
|---|---|---|---|---|
| libgphoto2 | full PTP/camera library; lists A7R V (PC Control) and X-T50 | **LGPL-2.1** (copyleft; dynamic linking OK, static linking in a closed app is not) | active, pushed 2026-11-13 | [V] https://github.com/gphoto/libgphoto2 (licence via GitHub API) |
| libusb | USB access for libgphoto2/libmtp | **LGPL-2.1** | active | [V] https://github.com/libusb/libusb |
| libmtp | MTP library; Android forks existed | **LGPL-2.1** | active | [V] https://github.com/libmtp/libmtp |
| thebino/libgphoto2android | Android wrapper around libgphoto2 | none declared in repo (so all-rights-reserved by default; wraps LGPL code) | stale (last push 2024-07-31, 41 stars, old NDK per README) | [V] https://github.com/thebino/libgphoto2android |
| petabyt/camlib (libpict) | C PTP client library | **Apache-2.0** | active, pre-release | [V] https://github.com/petabyt/camlib |
| petabyt/libfuji | Fujifilm USB/Wi-Fi protocol on top of PTP | **MIT** | active | [V] https://github.com/petabyt/libfuji |
| petabyt/fudge | Android/desktop Fuji app using libfuji | none shown | active | [V] https://github.com/petabyt/fudge (licence not confirmed) |
| petabyt/vcam | virtual PTP camera (testing aid) | **LGPL-2.1** | active | [V] https://github.com/petabyt/vcam |
| leirf/libptp | small Java/PTP lib | none | dead (2016) | [V] https://github.com/leirf/libptp |
| remoteyourcam-usb ("RYC") | Android PTP app for Canon/Nikon | repo not found at the guessed GitHub paths; licence [?] | [?] | [?] |
| qDslrDashboard, Camera Connect & Control, PhotoSync | closed apps; DSLRDashboard uses libgphoto2-like own stack [K]; approaches unverified | proprietary | n/a | [?] |
| Android `MtpDevice` | platform | Android platform (no licence burden for the app) | platform | [V] |
- Practical stance: no copyleft code in the app. The Android-side minimum is platform APIs. For step 3 write a small own PTP client (the PTP spec is public: ISO 15740; the vendor operations can be studied from libgphoto2/libfuji source, observing that reading GPL/LGPL code to learn opcodes is fine; copying code is not).

---

## 3. iOS / iPadOS

- ImageCaptureCore: `ICDeviceBrowser`, `ICCameraDevice` available on **iOS 13.0+, iPadOS 13.0+**, macOS 10.4+, visionOS 1.0 [V Apple's DocC JSON: https://developer.apple.com/documentation/imagecapturecore, `.../icdevicebrowser`, `.../iccameradevice`].
- Per symbol [V, same JSON API]: `tetheredCaptureEnabled` iOS 13+; `ptpEventHandler` iOS 13+ (macOS 12); `requestSendPTPCommand(_:outData:completion:)` iOS 13+ (macOS 10.15); **`requestEnableTethering()` and `requestTakePicture()` macOS only**. The API therefore gives iOS apps a raw PTP passthrough and PTP event stream, but not Apple's high-level capture.
- Hardware: Lightning iPhones need Apple's Lightning to USB Camera Adapter (Fuji says USB-C to Lightning cable does not work) [V Fuji X-T50 page]; USB-C iPhones (15 and later) connect directly [K, Apple USB-C iPhone docs not re-fetched].
- Entitlements / keys [K, not re-verified]: no special entitlement; `ICDeviceBrowser` needs the browser mask `.local`/`.camera` and, on iOS, the camera device is exposed through the delegate; Info.plist: no MFi `UISupportedExternalAccessoryProtocols` needed because PTP is class-driver, not External Accessory. Real-device test needed for any device-access prompt (E7).
- Photos/Files import: Photos app imports from cameras over the adapter [V Fuji page]. This is a manual flow, not an app integration.
- Third-party practice: Cascable (iOS) says over USB it imports from any PTP camera but fully remote-controls only Canon EOS and Nikon; Sony is through Sony Camera Remote API (Wi-Fi) [V https://apps.apple.com/us/app/cascable/id974193500]. Cascable publicly warns the Phase One IQ4 direct USB-C case; Capture One for iPad relies on Cascable [V https://www.captureintegration.com/capture-one-for-ipad-cascable-your-iq4-150-digital-back/]. Whether they use ImageCaptureCore or own stack: [?]. Lightroom mobile tether: [?].
- Limits: iOS apps are suspended in background; USB sessions end when backgrounded [K]. ICCameraDevice content catalog on iOS can be slow for large cards [K].

---

## 4. Official SDKs and compatibility with the repo

| SDK | Platforms | Licence terms | A7R V / X-T50 | Fit |
|---|---|---|---|---|
| Sony Camera Remote SDK (Toolkit) | Windows, macOS, Linux (x64, ARMv8, ARMv7); **no Android/iOS** [V https://support.d-imaging.sony.co.jp/app/sdk/licenseagreement_d/en.html and /en/] | Proprietary agreement: redistribute only as part of your app with the library "in an inseparable way"; no sublicensing the SDK; sharing the SDK itself needs Sony authorisation; end users licensed only to control the device; GPL/LGPL components keep their own licences [V same page] | ILCE-7RM5 since SDK 1.07.00 [V] | Cannot be used on phones; also cannot be committed to an open repo (SDK redistribution needs Sony approval). |
| Fujifilm X/GFX Camera Control SDK | Windows/macOS [V via search https://www.fujifilm-x.com/global/camera-control-sdk/agreement/] | Limited, non-exclusive, non-transferable, royalty-free; **must not be put under GPL/LGPL**; B2B access via contact form; using it voids the camera warranty (per forum quote, unverified) | X-T50 not on tether chart [V] | Not usable (no mobile, X-T50 unsupported, B2B). |
- An open-source Dapple repo: the Sony SDK binary cannot live in it; Dapple's own PTP code (Apache/MIT-style) can.
- Sony Creators' App/Imaging Edge Mobile are Sony's own apps [V first] and are not SDKs.

---

## 5. Recommendation

### Step 1 (smallest): plug in, import what is on the card
- Camera setting: A7R V = USB Connection Mode: MTP (or Remote Shooting for step 2; MTP first). X-T50 = CONNECTION MODE: USB CARD READER, USB POWER SUPPLY/COMM SETTING: AUTO or COMM ON.
- Android: manifest `usb.host` + `ACTION_USB_DEVICE_ATTACHED` filter (class 6); `MtpDevice`: list storage -> `getObjectHandles` -> filter by name/format (ARW, RAF, JPG/HIF) -> `importFile(fd)` into the app's ingest folder using the same pipeline as FTP (de-dup by name+size+datetime; skip already imported; keep the card untouched). Foreground service (connectedDevice) while importing.
- iOS: `ICDeviceBrowser` (camera mask) -> `ICCameraDevice` -> `requestDownloadFile` (macOS-style API also on iOS 13+ [V list on the iccameradevice page]) into the ingest folder; request only while foregrounded.
- Dart: one `UsbIngest` service with the same events as `FtpIngest` (arrived/failed/resumed) so the UI cards need no new states; transfers resume by listing again and skipping done files (idempotent).
- Needs no third-party code.

### Step 2: live as shots are taken
- A7R V: use Remote Shooting with Still Img. Save Dest. = Dest.+Camera (card keeps the copy; RAW+J option decides which file is sent) [V]. Over MTP use `MtpDevice.readEvent` (ObjectAdded) on Android and `ptpEventHandler` on iOS [V]; fallback = poll `getObjectHandles` every 1-2 s.
- X-T50: polling only (no tether). RAW+JPEG: request JPEG/embedded preview first, then RAW; resume after unplug via the idempotent listing above.
- Handle half-written files: wait for ObjectInfo size and a stable size/ObjectAdded after capture completes [K].

### Step 3: remote control, live view
- Needs an own PTP vendor client over `bulkTransfer` on Android and `requestSendPTPCommand` on iOS: Sony PC-Remote property set + capture + live view (opcodes learned from libgphoto2/libfuji as references, written independently), authentication/handshake specifics [?]. Fuji X-T50: not available wired; use the official wireless path or skip.
- Licensing: no copied LGPL code; Sony/Fuji SDKs unusable on mobile.

### Risks / unknowns
- X-T50 behaviour in card-reader mode while the camera is on and shooting; MTP event support on both cameras; A7R V exposing only slot 1 [V].
- USB-C cable/power negotiation (phone powering camera; dead-battery risk) and heat/battery on long tethers.
- Android 14-16 foreground-service typing and Play review; iOS background suspension.
- Large RAW over MTP speed; Mass Storage mode handling on phones.

### Experiments with the owner's cameras (run on the real hardware, record logs)
- E1 (A7R V, Android): USB Connection Mode = MTP, plug USB-C to USB-C; does the attach intent fire; `getDeviceInfo` (VID/PID 0x054c:?, operations/events supported lists); can handles and RAW be listed/imported; MB/s for an ARW and a JPEG.
- E2 (A7R V): while connected in MTP, shoot a frame: any ObjectAdded event via `readEvent`? does the new file appear in `getObjectHandles` without reconnecting? does the card get written? (slot 1 vs 2 with dual recording).
- E3 (A7R V): Remote Shooting mode: is it PTP class 6 or vendor class (0xFF) (matters for `MtpDevice`); what the interface descriptor shows; does `MtpDevice.open` succeed; does Dest.+Camera write both.
- E4 (X-T50, Android): CONNECTION MODE = USB CARD READER; Android shows "USB PTP"? list/import; shoot while connected; new handles appear? events? file stable only after the camera finishes writing?
- E5 (both): RAW+JPEG pair behaviour (order, naming, sizes), unplug mid-transfer and replug (resume), camera sleep/auto power-off while connected, battery drain with/without USB power setting.
- E6 (both, power): phone battery and camera battery over 15 minutes; with "POWER SUPPLY OFF/COMM ON" on the Fuji.
- E7 (iPhone USB-C, iOS 17/18): the same list/import with ICCameraDevice; do `ptpEventHandler` events arrive on a shot; does the camera appear at all in each USB mode; app background behaviour.
- E8: whether `requestSendPTPCommand` accepts Sony PC-Remote property reads (for step 3 feasibility) on iOS.
- E9: Fuji tether lists: re-check the X-T50 firmware's menu for a tether entry after the latest firmware (the chart/footnote may change).

## Source index
- Android MtpDevice: https://developer.android.com/reference/android/mtp/MtpDevice (fetched 2026-11-14)
- Android USB host: https://developer.android.com/develop/connectivity/usb/host [K]
- Apple ImageCaptureCore: https://developer.apple.com/documentation/imagecapturecore (platform data via DocC JSON, 2026-11-14)
- Sony A7R V Help Guide USB mode: https://helpguide.sony.net/ilc/2230/v1/en/contents/TP0002887823.html; remote shoot setting: .../TP0003057069.html
- Sony Camera Remote SDK: https://support.d-imaging.sony.co.jp/app/sdk/en/ ; licence: https://support.d-imaging.sony.co.jp/app/sdk/licenseagreement_d/en.html
- Fuji X-T50 menu: https://fujifilm-dsc.com/en/manual/x-t50/connections/network_usb_menu/ ; smartphone USB: https://fujifilm-dsc.com/en-int/manual/x-t50/connections/usage_usb/ ; tether chart: https://www.fujifilm-x.com/en-gb/support/compatibility/software/tether-app/ ; Fuji SDK agreement: https://www.fujifilm-x.com/global/camera-control-sdk/agreement/
- libgphoto2 camera table: https://github.com/gphoto/libgphoto2/blob/master/camlibs/ptp2/library.c ; other repos listed in the 2.2 table (licence/pushed_at read via GitHub API 2026-11-14)
- Cascable: https://apps.apple.com/us/app/cascable/id974193500
