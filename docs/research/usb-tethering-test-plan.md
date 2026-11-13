# USB camera import: first real-camera test plan

Date: 2026-11-14. Goes with `usb-tethering.md` (experiments E1 to E9). Step 1 is built: plug the camera into the phone, new shots are received, and "导入照片…" picks older ones from the card. Everything below is what the owner does and what to send back. The app logs everything under one tag, `DappleUsb`, and the picker has "复制诊断信息" (copies the whole log plus the USB descriptors).

## What was verified without a camera

Only against a folder-backed fake camera (JVM tests, emulator): paging, RAW+JPEG pairing, identity and de-duplication, event-versus-poll choice, unplug mid-import, re-plug resume, the card states. Nothing about real PTP/MTP behaviour is verified. Unknown until this test: whether `MtpDevice.open` succeeds on each camera and mode, whether `getObjectHandles(storage, 0, 0)` returns the whole card, whether ObjectAdded events arrive, object dates and sizes while a file is still being written, thumbnail support, transfer speed.

## Before you start (once)

1. Install the app (debug or release). Open it once so it is the foreground app.
2. Use a USB-C to USB-C cable, phone as host. Photos taken before the test are fine; use a card with at least 20 photos including a RAW+JPEG pair.
3. Android asks "Dapple 访问 USB 设备" the first time: choose 允许 (tick "默认使用" if offered).
4. After every attempt: open the picker (tap the card's "导入照片…", or if there is no such button just go back to the home screen), tap "复制诊断信息", paste it into a message. If the app never showed a camera, still open 设置 or any picker once and send the log from a later attempt; the log also lists every USB device the phone saw.

## Reading the card on the home screen

| Card | Meaning |
|---|---|
| 相机已插入 · 等待允许访问 | system permission dialog is up |
| 型号 · 已连接, "约 N 个文件 · 自动接收" | camera opened, baseline taken (what is on the card now is NOT imported by itself) |
| 正在接收第 i 张 · 共 n 张 | importing |
| 收到 DSC… · 共 n 张 | last shot received |
| 型号 · 已拔出 | cable pulled; stays 45 s |
| 相机 · 连接失败 + 相机没有响应 / 相机被占用 / 读不到存储卡 / 读不到卡里的照片 / 未允许访问相机 | see the log line quoted below |

Videos are skipped on purpose (picker footer says 视频暂不导入).

## Test A: Sony A7R V, USB connection mode MTP (E1, E2, E5)

Camera: MENU, Setup, USB, USB Connection Mode = MTP. Card in slot 1 (slot 1 is the only one MTP exposes).

1. Camera on, plug in. Expect the permission dialog, then "ILCE-7RM5 · 已连接" (maker and model come from the camera). Log lines to check: `usb device ... vid=1356 (0x54c)`, `interface ... class=6`, `device info:`, `operations supported:`, `events supported:` (is 0x4002 ObjectAdded in the list), `storages:`, `handles(storage ...) = N in X ms`, `baseline:`.
2. Tap 导入照片…: the list must show the newest photos first within a second or two, thumbnails appearing as you scroll. Pick one RAW+JPEG pair and a JPEG only, tap 导入. Log: `import <name>: <bytes> B in <ms> ms (<KB/s>)`. Write down the speed of the ARW and the JPEG, and whether the ARW arrives as a usable photo in the library.
3. Shoot one frame (E2). Wait 10 s. Log lines to read:
   - `event 0x4002 ObjectAdded` present: events work. Note the delay between the shutter and the `new shots:` line.
   - Only `poll: 1 new handles ... (mode=POLL)`: events do not arrive; polling works. Note the delay.
   - Neither after 30 s: the camera does not show new files while connected; unplug and re-plug, and say whether the new shot is then on the card list.
   - `size never settled`: the camera announces a file before it is complete.
4. Shoot RAW+JPEG a few times in a row (E5): how many `new shots:` lines per frame, the order (JPEG first or RAW first), whether the photo in the library is one photo (pair) in the end.
5. Pull the cable while importing a big list (E5): card shows 已拔出, no half file stays in the library. Plug back in, import the same photos again: finished ones are not copied again (`already in the inbox ... not copied again`).
6. Let the camera auto power-off (or sleep) while connected: send the log; it shows whether the app saw an unplug or an error.

## Test B: Sony A7R V, Remote Shooting mode (E3)

USB Connection Mode = PC Remote (Remote Shooting). Plug in. This probably does not give a card to browse. What matters is the descriptor lines (`interface ... class=... sub=... proto=...`) and whether the card says 连接失败 (`MtpDevice.open returned false`) or opens. Send the log either way. If it opens, repeat step 3 of test A and note where the shot is written (Still Img. Save Dest.).

## Test C: Fujifilm X-T50, USB card reader mode (E4)

Camera: SET-UP, CONNECTION SETTING, CONNECTION MODE = USB CARD READER; USB POWER SUPPLY/COMM SETTING = AUTO (or COMM ON). Android sees it as a PTP camera.

1. Plug in. Expect the permission dialog and "FUJIFILM X-T50 · 已连接". Same log lines as test A step 1.
2. 导入照片…, pick a RAF and a JPEG pair, import. Speed, and whether the RAF arrives as a usable photo.
3. Shoot a frame with the cable in. Read the log as in test A step 3. The X-T50 has no tether mode, so the interesting answers are: do new handles appear while it stays connected (poll), is a file listed before it is complete.
4. Camera off then on with the cable in; cable out and in during an import (as A5, A6).

## Test D: both, power and battery (E6)

15 minutes connected and idle with the screen on: phone battery percent and camera battery percent at start and end. For the Fuji repeat with POWER SUPPLY OFF/COMM ON. Note whether the phone charged the camera.

## What the first real test must show (decisions it unlocks)

1. Does `MtpDevice.open` work for A7R V (MTP mode), A7R V (Remote Shooting), X-T50 (card reader)? (E1, E3, E4)
2. Do ObjectAdded events arrive on the A7R V, on the X-T50? If not, is poll latency acceptable (the poll runs every 2 s while no event was ever seen; each listing time is logged, the interval grows to 3x the listing time)? (E2, E4)
3. Is `getObjectHandles(storage, 0, 0)` complete, how long does it take on a card with thousands of files (`handles(...) in X ms`)?
4. Object dates and sizes: is the capture date there (the picker order and the de-duplication key use it), is the size final when announced?
5. Transfer speed per file type. Does the foreground-service question matter: how long does a 20-photo import take with the screen on? (A foreground service of type connectedDevice was deliberately not built in step 1; it is the first thing step 2 adds.)
6. RAW+JPEG order and pairing in the library.

Mapping to the research list: E1 = test A steps 1-2, E2 = A3, E3 = B, E4 = C, E5 = A4-A6 and C4, E6 = D. E7 and E8 are iOS (not in this round). E9 is a menu check on the camera, no app needed.

## Fake camera on the emulator (no hardware)

Debuggable builds only. Build with `flutter build apk --debug -Pdapple.x86=true --target-platform android-x64`, install with `tools/emu.sh install`. Make the folder `/sdcard/Android/data/com.dapple.dapple/files/fakecam/DCIM/100MSDCF/` and push JPG/ARW files into it (`tools/emu.sh adb push`). Control files in `fakecam/`:

- `ATTACHED`: present = cable plugged in. `touch` to plug in, `rm` to unplug (works in the middle of an import).
- `SLOW`: copies run at about 1 MB/s (time to unplug mid-import).
- `EVENTS`: the fake sends ObjectAdded events; without it the camera has no events and the app polls.
- `NOCARD`: the camera reports no storage.

New shot = copy a file into `DCIM/100MSDCF/` while attached. Log: `tools/emu.sh adb shell "logcat -d -s DappleUsb"`.

## iOS later (what the implementation needs)

Same behaviour behind `UsbCameraBackend` (`app/lib/platform/usb_camera.dart`), implemented with ImageCaptureCore (`ICDeviceBrowser`, `ICCameraDevice`):

- events with the same maps as `dapple/usbcam_events`: `state` (none, permission is not needed on iOS, opening, ready with `device` and `card.total`, unplugged, error with `reason`), `newShots` (groups), `importStart/Progress/Done/Failed/QueueDone`;
- `list` with a cursor in newest-first order and groups by folder+stem; `thumbnail` (ICCameraFile thumbnail); `importHandles` (requestDownloadFile into the inbox, ".part" then rename); `cancelImport`; `diagnostics` text;
- the identity key must be built the same way: `usb:<lower-case name>|<size>|<capture second>`, so FTP and cable de-duplication keep working;
- new shots from `ICCameraDeviceDelegate` added-items callbacks plus a polling fallback; only while the app is in the foreground;
- Dart needs no change: `NoUsbCamera` is returned today, `UsbCameraBackend.forPlatform()` is the single switch.
