#!/usr/bin/env bash
# ON THE MAC: ONNX Runtime v1.32.0 minimal build for iOS (device arm64 + simulator arm64), static onnxruntime.xcframework with only the operators of the
# shipped models (scripts/ios/ort-required-ops.config), CPU provider only, no exceptions, no ML ops: the iOS twin of the Android build in docs/legal/ai-models.md.
# Source: https://github.com/microsoft/onnxruntime tag v1.32.0 cloned to ~/dapple/tools/ort-src (MIT). Output: ~/dapple/tools/ort-min/onnxruntime.xcframework
# (ios-arm64 + ios-arm64-simulator slices; the macOS host-test slice stays the official one). Usage: build-ort-min.sh   (about 10 - 25 minutes)
set -euo pipefail
source "$(dirname "$0")/env.sh"
HERE="$(cd "$(dirname "$0")" && pwd)"; SRC="$DAPPLE_HOME/tools/ort-src"; OUT="$DAPPLE_HOME/tools/ort-min"
[ -f "$SRC/tools/ci_build/build.py" ] || git clone --depth 1 --branch v1.32.0 https://github.com/microsoft/onnxruntime "$SRC"
mkdir -p "$OUT"
# the ORT build scripts need Python 3.10+ (the system one is 3.9) and the flatbuffers module: a venv under ~/dapple/tools from uv (https://github.com/astral-sh/uv, MIT / Apache-2.0)
VENV="$DAPPLE_HOME/tools/ort-venv"
if [ ! -x "$VENV/bin/python" ]; then
  [ -x "$DAPPLE_HOME/tools/uv/uv" ] || { mkdir -p "$DAPPLE_HOME/tools/uv"; curl -fsSL https://github.com/astral-sh/uv/releases/download/0.5.31/uv-aarch64-apple-darwin.tar.gz | tar -xz -C "$DAPPLE_HOME/tools/uv" --strip-components=1; }
  export UV_PYTHON_INSTALL_DIR="$DAPPLE_HOME/tools/uv-python" UV_CACHE_DIR="$DAPPLE_HOME/tools/uv-cache"
  "$DAPPLE_HOME/tools/uv/uv" venv --python 3.12 "$VENV"
  "$DAPPLE_HOME/tools/uv/uv" pip install --python "$VENV/bin/python" flatbuffers numpy packaging
fi
cat > "$OUT/settings.json" <<JSON
{
  "build_osx_archs": {"iphoneos": ["arm64"], "iphonesimulator": ["arm64"]},
  "build_params": {
    "base": ["--parallel", "--build_apple_framework", "--skip_tests", "--skip_pip_install", "--no_kleidiai", "--minimal_build", "--disable_exceptions", "--disable_ml_ops",
             "--include_ops_by_config=$HERE/ort-required-ops.config", "--enable_reduced_operator_type_support", "--compile_no_warning_as_error",
             "--cmake_extra_defines=onnxruntime_BUILD_UNIT_TESTS=OFF"],
    "iphoneos": ["--ios", "--use_xcode", "--apple_deploy_target=16.0"],
    "iphonesimulator": ["--ios", "--use_xcode", "--apple_deploy_target=16.0"]
  }
}
JSON
cd "$SRC"
"$VENV/bin/python" tools/ci_build/github/apple/build_apple_framework.py --config MinSizeRel --build_dir "$OUT/build" "$OUT/settings.json" 2>&1 | tee "$OUT/build.log" | tail -5
rm -rf "$OUT/onnxruntime.xcframework"; cp -R "$OUT/build/framework_out/onnxruntime.xcframework" "$OUT/"
du -sh "$OUT/onnxruntime.xcframework"/*
