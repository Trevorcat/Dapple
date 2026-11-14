# Store release checklist (Android)

Status 2026-11-14. done / missing / open (needs the owner or a check outside the repo). Build and signing: `docs/release/android.md`.

## Build and signing

| Item | State |
|---|---|
| Release build type with R8 + resource shrinking, JNI keep rules | done |
| Signing from git-ignored `key.properties`, loud warning on debug-key fallback | done (mechanism) |
| Upload keystore itself | **missing**: owner creates it (steps in android.md), keeps it out of the repo, backs it up |
| Store-side app signing enrolment | open |
| Version name / code from pubspec | done; raise `+N` per upload |
| AAB produced | done (87 MB); Flutter prints a false "failed to strip" because `cmdline-tools` (apkanalyzer) is not in `tools/android-sdk` |
| Native debug symbols in the AAB | done (SYMBOL_TABLE) |
| 16 KB ELF alignment of all native libs | done (checked) |
| Release start-up on an emulator | done (x86_64 emulator; see android.md). Full pass on an arm64 phone: open |

## Target SDK and manifest

| Item | State |
|---|---|
| `targetSdk` 36, `compileSdk` 36, `minSdk` 29 (from the pinned Flutter's Gradle plugin) | done. Re-check the store's minimum target level at submission time |
| `MANAGE_EXTERNAL_STORAGE` (used to list RAW files MediaStore does not classify as images) | **missing decision**: Google Play allows it only for a few core-function categories and needs a declaration; a photo editor that can use the picker / MediaStore is normally rejected. Remove it for the store build or justify it. Other stores differ |
| `READ_MEDIA_IMAGES`, `READ_MEDIA_VIDEO`, `READ_MEDIA_VISUAL_USER_SELECTED` | in the manifest; the Play photo and video permissions declaration is open |
| `CAMERA` | in the manifest, optional (`uses-feature required=false`) |
| `FOREGROUND_SERVICE_DATA_SYNC` (camera FTP receiver) | in the manifest; the foreground-service declaration is open |
| `POST_NOTIFICATIONS`, `WAKE_LOCK`, `INTERNET`, network / Wi-Fi state | in the manifest; `INTERNET` serves the local FTP and camera links: confirm in the data-safety answers |
| `queries` for WeChat, Xiaohongshu, Weibo, Douyin, Instagram | in the manifest; the store may ask why (share targets) |
| Backup / data-extraction rules | missing: not set, defaults apply; decide whether caches and edit data may be backed up |

## Privacy

| Item | State |
|---|---|
| Privacy policy text (a public URL is required by the stores) | **missing**: no policy text in the repo (only `docs/design/requirements.md` mentions privacy). Content below |
| In-app link to the policy | missing |
| Data-safety / privacy-label answers | missing; basis below |

What the policy has to say, from how the app works today (check each point against the code before publishing):
* Photos and videos: read from the media store, a picker, USB and FTP cameras, to edit on the device. Edits go to new files. Nothing is uploaded: no server, no account, nothing downloaded at run time (`docs/legal/ai-models.md`).
* Camera permission: for the in-app capture screen only; the photo stays on the device.
* Location: GPS tags inside photos are read and copied as EXIF; the app does not request the location permission. Verify against the EXIF code before writing this.
* Network: local-network FTP and camera links only; no analytics, advertising id or crash-upload SDK is known. Verify at release (`flutter pub deps`, AAR list).
* Face data: AI masks detect faces and landmarks on the device for retouching; nothing leaves the device. Some regions treat face processing as sensitive data: say so plainly.
* Contact, retention, children, deletion: the owner's text.

## Open-source notices: in the app vs what is bundled

The in-app page is Settings > 开源许可 (`app/lib/screens/library/licenses_page.dart`; texts in `app/assets/`). Compared with `docs/legal` and with the libraries in the release APK:

| Bundled in the APK | Licence | docs/legal | Shown in the app |
|---|---|---|---|
| spektrafilm film profiles (`dapple.dfp`) | CC BY-SA 4.0 | docs/film | done: credit, notice, full text |
| FFmpeg 9.0.2 shared libs (5) | LGPL-2.1+ | `ffmpeg-lgpl.md` | notice + link done. **Missing**: a copy of the LGPL text in the app or distribution (the legal doc, section 5, requires it) and the "source on request" contact |
| LibRaw 0.23.0 (static) | CDDL-1.0 | `libraw.md` | notice + link; CDDL text only linked |
| libjxl 0.11.1 + Highway 1.2.0 (static) | BSD-3 | `libjxl.md` | notice + link; the doc asks for the BSD-3 text itself: **missing** |
| LLVM OpenMP (static) | Apache-2.0 with LLVM exception | `libomp.md` | notice + link done |
| LiteRT (`libtensorflowlite_jni.so`) | Apache-2.0 | `ai-models.md` | Apache text done. LiteRT's third-party notices (flatbuffers, XNNPACK, ruy ...) **not fetched**; the legal doc says do it before a store release |
| ONNX Runtime | MIT | `ai-models.md` | done (MIT + ThirdPartyNotices) |
| MediaPipe models (face detector, face landmarks, selfie multiclass, tap encoder / decoder) | Apache-2.0 | `ai-models.md` | done (credits + Apache text); re-verify upstream NOTICE files before release: open |
| Depth Anything V2 Small | Apache-2.0 | `ai-models.md` | done; not bundled by default |
| Flutter engine / framework and pub packages (`NOTICES.Z` is in the APK) | BSD and others | none | **missing**: the page does not call `showLicensePage` / `LicenseRegistry`, so these notices are in the APK but not shown |
| CameraX, AndroidX, Kotlin runtime | Apache-2.0 | none | **missing**: no `docs/legal` entry, not in the page |
| sRGB / Display P3 ICC profiles | generated, no vendor file | `srgb-icc.md` | nothing to show |

Not shipped, so no notice: SPIRV-Cross, ANGLE, SwiftShader (host tools, `gles-toolchain.md`), MobileSAM.
Also open: a lawyer's reading of the AI model chain ("what a lawyer should see" in `ai-models.md`) and the FFmpeg H.264 / HEVC patent-pool note (`ffmpeg-lgpl.md`).

## Store listing and assets

| Item | State |
|---|---|
| Final adaptive launcher icon (foreground / background / monochrome) | **missing** (PNG placeholder, no adaptive icon) |
| Launch screen artwork | placeholder |
| Screenshots, feature graphic, description, categories | missing |
| English or other string resources (strings are Chinese only) | missing; decide store languages |
| Content rating questionnaire | open |
| Final `applicationId` confirmation | open |
| Crash reporting | symbols go with the AAB; no in-app crash SDK |
| Test track on a real device matrix (GPU self-test on many GPUs) | open |
