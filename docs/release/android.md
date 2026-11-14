# Android release build

Status 2026-11-14. Files: `app/android/app/build.gradle.kts`, `app/android/app/proguard-rules.pro`, `app/android/key.properties.example`.

## Build

```
source tools/env.sh
cd app
flutter build apk --release        # build/app/outputs/flutter-apk/app-release.apk
flutter build appbundle --release  # build/app/outputs/bundle/release/app-release.aab
```

Switches (Gradle properties, `-P...`; `flutter build` passes them through):

| Property | Effect |
|---|---|
| `dapple.abis=arm64-v8a,x86_64` | explicit ABI list. Default: `arm64-v8a` only |
| `dapple.x86=true` | adds `x86_64` (emulator builds); also use `--target-platform android-arm64,android-x64` |
| `dapple.ai=false` | no AI runtimes and no models |
| `dapple.aiDepth=true` | bundles the 99 MB depth model (default: left out) |
| `dapple.gpu=false` | CPU-only engine |
| `dapple.idSuffix=.dev` | side-by-side install `com.dapple.dapple.dev`, label "Dapple Dev" |

Version name and code come from `app/pubspec.yaml` (`version: 1.0.0+1`: name 1.0.0, code 1) through the Flutter Gradle plugin; there is no second place to edit. Raise the `+N` for every upload to the store. Per-ABI split: with one ABI by default there is nothing to split; an AAB lets the store split per device by itself. If APKs for several ABIs are ever published side by side, they need distinct version codes (not set up).

## Signing

Release builds read `app/android/key.properties` (git-ignored, in `app/android/.gitignore` and the root `.gitignore`). Copy `key.properties.example`, fill in:

```
storeFile=../../tools/release-keys/dapple-upload.jks   # relative to app/android/
storePassword=...
keyAlias=...
keyPassword=...
```

Create the key once (keep the keystore and the passwords outside the repository and back them up; losing the upload key means a support request to the store):

```
keytool -genkeypair -v -keystore <path>.jks -alias dapple-upload -keyalg RSA -keysize 4096 -validity 10000
```

Without a complete `key.properties` (or if the keystore file does not exist) the release build is signed with the debug key and Gradle prints `!!!!!!!! WARNING: app/android/key.properties missing or incomplete ...`. `flutter build` hides Gradle warnings; it shows when running `./gradlew :app:help` in `app/android`. Such an APK installs for tests and cannot be published. No key or password is in the repository.

## R8 and resource shrinking

Release: `isMinifyEnabled` and `isShrinkResources` on, rules in `proguard-rules.pro`:
* every class with a `native` method keeps its name and those methods (JNI binds by name: `DappleViewNative`, `DappleVideoEdit` in `ClipEdit.kt`, `SwVideo`); the engine never calls back into Java by reflection (`FindClass` / `GetMethodID` do not occur in `core/`), so no callback rules;
* the `*Plugin` classes and `MainActivity`;
* LiteRT and ONNX Runtime are used as native libraries through their C APIs: there is no Java wrapper, the Java keep lines are insurance only;
* Play Core `dontwarn` for the Flutter embedding.

Result: classes.dex 1.6 MB. Checked: the release dex still has all `com.dapple.dapple` classes, every JNI-bearing class and the `native*` method names.

## Size report (2026-11-14, arm64-v8a, no depth model, GPU + AI on)

Release APK 97.6 MB (`app-release.apk`; the debug APK of the same day was 397 MB: unshrunk, x86_64 as well, depth model). Compressed bytes inside the APK:

| Part | MB | Notes |
|---|---|---|
| AI models (`assets/ai-models`) | 49.7 | stored uncompressed: `tap_encoder` 28.3, `person_parts` 16.4, `face_landmarks` 2.6, `tap_decoder` 2.3, `face_detector` 0.2 |
| Native libs total | 44.5 | |
| `libdapple.so` (engine, LibRaw, libjxl, GPU backends, AI masks) | 12.0 | |
| `libflutter.so` | 11.8 | |
| `libapp.so` (Dart AOT) | 7.1 | |
| AI runtimes: `libtensorflowlite_jni.so` 4.3 + `libonnxruntime.so` 2.8 | 7.1 | |
| FFmpeg (avcodec 2.5, swscale 1.2, avutil 0.7, avformat 0.5, swresample 0.1) | 5.0 | |
| `libc++_shared.so` + small libs | 1.5 | |
| `classes.dex` | 1.6 | after R8 |
| Flutter assets (film pack `dapple.dfp` 1.0, NOTICES, fonts) | 1.3 | |

So AI models plus runtimes are about 57 MB of 98 MB; the engine, Flutter and FFmpeg about 29 MB. Depth model: +99 MB if bundled (off by default).

AAB 87.0 MB (`app-release.aab`): models 44.4, libs 18.8 deflated, plus 20.1 MB of native debug symbols (`BUNDLE-METADATA/.../*.sym`) that the store keeps and users never download. Download size per device will be close to 70 MB (the base APK of the bundle; not measured with the store's tool).

All native libs have ELF segment alignment of 16 KB or more (16 KB page-size requirement; checked with a header read of the APK).

## Known tool limits

* `flutter build appbundle` writes the AAB and then exits with "Release app bundle failed to strip debug symbols". Cause: Flutter's own check calls `apkanalyzer`, which is in the Android SDK `cmdline-tools` that `tools/android-sdk` does not have. The AAB is complete (`.sym` files for libflutter, libapp, libdapple present). Install `cmdline-tools;latest` into `tools/android-sdk` to silence it (not done).
* Keep the `.cxx` directory builds one at a time: parallel Gradle builds in `app/android` lock each other.

## App name, icon (what is placeholder)

* Label: "Dapple" (`manifestPlaceholders["appLabel"]`, "Dapple Dev" with a suffix). Final.
* Launcher icon: PNG mipmaps `mipmap-{m,h,xh,xxh,xxxh}dpi/ic_launcher.png` (1.4 KB at xxxhdpi), committed in the first commit. **Placeholder / needs the final artwork.** No adaptive icon (`mipmap-anydpi-v26/ic_launcher.xml` with foreground / background layers) and no monochrome layer for themed icons: missing. Without it, Android 8+ shows the PNG inside a plain white or squircle frame.
* Launch screen: `drawable/launch_background.xml` (Flutter template, plain). **Placeholder.**
* Notification channel strings are in Chinese only (`values/strings.xml`); no English or other locale resources.
* `applicationId` `com.dapple.dapple`: confirm this is the final package name; it cannot change after the first store upload.

## Smoke test of the release build

Result: last section of this file.

## Smoke test of the release build (2026-11-14)

Emulator `emulator-5554` (Android 35 google_apis x86_64; no physical phone touched). A release APK with `x86_64` added (`flutter build apk --release --target-platform android-arm64,android-x64 -Pdapple.x86=true -Pdapple.idSuffix=.dev`, 184 MB, debug-signed because there is no key) installed side by side as `com.dapple.dapple.dev`:
* cold start 9.6 s on the software-rendered emulator, process alive after 15 s, no `FATAL EXCEPTION`, no `UnsatisfiedLinkError` / `NoSuchMethodError`;
* the engine log line `dapple_engine: gpu probe: ...` appears: `libdapple.so` loaded and the JNI call `DappleViewNative.startGpu` ran after R8 (SwiftShader has no usable GPU backend, so frames stay on the CPU as designed);
* the home screen rendered (screenshot checked): tab capsule, "还不能读取照片" card, empty state.
The test app was uninstalled afterwards.

Not tested: opening a photo, the edit pipeline, AI masks, video (FFmpeg), USB / FTP, capture. These touch the other JNI classes (`DappleVideoEdit`, `SwVideo`) that R8 keeps by rule; only the static dex check covers them. Run a full pass on the arm64 release APK on a phone before the first store upload (a physical-phone step for the owner).

Static comparison, arm64 release APK vs the debug APK: every native library and every `ai-models` / Flutter asset of the debug build is in the release APK. Differences: `depth.ort` (debug only: the release build leaves it out by default), `libapp.so` and `libflutter.so` (release only: AOT code instead of the debug kernel). The release arm64 APK is kept at `build/rel-inspect/app-release-arm64.apk`; `app/build/.../flutter-apk/app-release.apk` currently holds the x86 test build.
