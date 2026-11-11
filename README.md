# Dapple

A cross-platform app (Android first, iOS later) for photographers: get photos
from a Sony or Fujifilm camera onto the phone, develop RAW (ARW/RAF) and JPEG
automatically, apply looks (Lightroom `.xmp` presets, `.cube` LUTs), correct
each photo automatically, and share quickly (e.g. WeChat Moments).

* `core/` - portable C++23 image engine (decoders, camera profiles, renderer,
  presets/LUTs, automatic correction, ingest, C API)
* `cli/` - `dapple` desktop tool (info / develop / watch / thumb)
* `tests/` - unit tests, run with ctest
* `app/` - Flutter app (calls the core through `dart:ffi`)
* `docs/architecture.md` - layers, colour pipeline, calibration, adding cameras

## Building the core and CLI (Windows, project-local toolchain)

Every tool lives inside `tools/` (git-ignored); nothing is installed system-wide.
The pinned toolchain is llvm-mingw (clang, C++23, libc++), CMake and Ninja;
LibRaw 0.22.2 sources are expected in `tools/_dl/libraw/LibRaw-0.22.2`
(otherwise CMake downloads them).

```bash
export PATH="$PWD/tools/llvm-mingw-20261020-ucrt-x86_64/bin:$PWD/tools/cmake-4.4.3-windows-x86_64/bin:$PWD/tools/ninja:$PATH"
cmake -S . -B build/desktop -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DFETCHCONTENT_SOURCE_DIR_LIBRAW=$PWD/tools/_dl/libraw/LibRaw-0.22.2
cmake --build build/desktop
cd build/desktop && ctest --output-on-failure
```

Outputs land in `build/desktop/bin/`: `dapple.exe` (CLI), `libdapple.dll`
(FFI library, exports only `dapple_c.h`), `dapple_tests.exe`. Binaries are
statically linked, so they run without any DLLs next to them.

CMake options: `-DDAPPLE_BUILD_CLI=OFF -DDAPPLE_BUILD_TESTS=OFF` build only the
libraries. This is what the Android build uses (the shared target `dapple`
becomes `libdapple.so`; the NDK, Android SDK and Gradle also live under
`tools/`).

## CLI

```
dapple info <files...>
dapple develop <inputs...> [-o out.jpg | --out-dir DIR] [options]
dapple watch <dir> --out-dir DIR [develop options] [--existing] [--settle MS] [--max N] [--timeout SEC]
dapple thumb <raw> -o out.jpg [--long-edge N]        # camera's embedded JPEG preview
```

Develop options:

| option | meaning |
|---|---|
| `--xmp preset.xmp` | Lightroom / Camera Raw preset (unsupported settings are listed) |
| `--lut look.cube`, `--lut-amount N` | 3D LUT and its strength 0..100 |
| `--auto` / `--no-auto` | per-photo automatic correction (default on), `--strength 0..1` |
| `--full` | full-resolution demosaic (default: fast half-size) |
| `--long-edge N` | output long edge, 0 = native (default 2560) |
| `--quality N` | JPEG quality (default 92) |
| `--dump-params` | print the applied EditParams JSON |

Examples (paths may contain non-ASCII characters):

```bash
build/desktop/bin/dapple info "D:/图片/2026-07-10/DSC00111.ARW"
build/desktop/bin/dapple develop "D:/图片/2026-07-10/DSC00111.ARW" -o build/out/a.jpg --dump-params
build/desktop/bin/dapple develop D:/图片/2026-07-10 --out-dir build/out --xmp warm.xmp --lut film.cube --lut-amount 60
build/desktop/bin/dapple watch /path/to/ftp-drop --out-dir build/out     # simulates the camera FTP drop folder
```

Never write outputs next to the original photos; use `build/` or another scratch directory.

## Licenses

* LibRaw is used under CDDL-1.0 (it is dual-licensed LGPL-2.1 / CDDL-1.0), which
  permits static linking into the mobile apps. Ship its license text with the app.
* `stb_image` / `stb_image_write` (vendored in `third_party/stb`) are public domain.
