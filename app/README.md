# Dapple app (Flutter)

Android first (minSdk 26, arm64-v8a only), iOS scaffold present but not wired to the native core yet.
All UI strings are Chinese. The image engine is the C++ core (`../core`, shared lib `libdapple.so`)
called through `dart:ffi` (`lib/native/dapple_bindings.dart`, hand-written from `core/include/dapple/dapple_c.h`).

## Toolchain (all inside the repo, `tools/`, gitignored)

| Item | Version |
| --- | --- |
| Flutter | 3.47.8 (Dart 3.13.7), `tools/flutter` |
| Android SDK | `tools/android-sdk`: platform-tools, platforms;android-36, build-tools;36.0.0, cmake;3.31.6, ndk;28.2.13676358 |
| JDK | system JDK 17 (read-only use, `JAVA_HOME` set per session) |
| pub cache / gradle home | `tools/pub-cache`, `tools/gradle-home` |

Session environment (does not touch system PATH):

```bash
source tools/env.sh          # Git Bash
. .\tools\env.ps1            # PowerShell
```

## Build and install

```bash
source tools/env.sh
cd app
flutter pub get
flutter analyze
flutter build apk --debug --target-platform android-arm64
adb install -r build/app/outputs/flutter-apk/app-debug.apk
```

Gradle drives CMake (`android/app/build.gradle.kts`): it builds the repo-root `CMakeLists.txt` with
`-DDAPPLE_BUILD_CLI=OFF -DDAPPLE_BUILD_TESTS=OFF`, target `dapple`, ABI `arm64-v8a`, LibRaw from
`tools/_dl/libraw/LibRaw-0.22.2` (`FETCHCONTENT_SOURCE_DIR_LIBRAW`). The first native build compiles LibRaw and
takes a few minutes. Release: `flutter build apk --release --target-platform android-arm64` (signed with the debug key for now).

If `libdapple.so` is missing or fails to load the app still starts and shows a banner; no photo can be developed then.

## Architecture

Design source of truth: `../docs/design/requirements.md` (chapter 6 = engine parameter keys) and `guidelines.md`.

- `lib/theme/tokens.dart` - light/dark colour tokens (`ThemeExtension` `DC`, read with `context.c`), fixed colours, type scale, spacing, radii, `buildTheme`.
- `lib/model/params.dart` - the chapter-6 slider table (74 numeric keys), `EditState` (per-photo overrides, only non-defaults stored), `toWire()` (JSON sent to `dapple_session_render`), merge/paste/copy semantics, per-photo `UndoStack`.
- `lib/model/catalog.dart` - 9 looks, 6 publish presets (second-hand numbers, to be verified), demo scenes for tests.
- `lib/state/app_state.dart` - library, groups, connection card state, selection, freeze / "new" pill, edits + undo, clipboard, toasts, settings, develop queue, export. `AppState.demo()` is the in-memory variant used by tests.
- `lib/native/*` - dart:ffi bindings + `DappleSession` (each native call in an isolate; analyze symbols are optional).
- `lib/services/session_cache.dart` (decoded session per photo, LRU 3), `editor_engine.dart` (latest-wins debounced renders, histogram analysis, 9 tone previews), `ingest.dart` (MediaStore / picker / FTP stub).
- `lib/widgets/*` - icons (custom painter), controls (slider, chips, switch, toast...), `PhotoArt`.
- `lib/screens/home|editor|publish|library` - screens. Responsive by window width (< 600 phone, >= 600 rail + inspector), never by device.
- Tests: `test/*_test.dart`; `test/support/harness.dart` loads a CJK font and writes screenshots to `build/app_shots/`.

Not in this pass (entry points + TODO in code): HDR / panorama / live tools, FTP ingest, look import (.xmp/.cube/Fuji recipes), point curves, free crop box, iOS/Cupertino variants.
