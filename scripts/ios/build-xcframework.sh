#!/usr/bin/env bash
# ON THE MAC: engine for iOS device + simulator -> dapple.xcframework inside the Flutter plugin app/native_ios/dapple_engine.
set -euo pipefail
B="$HOME/dapple/build"; SRC="$HOME/dapple/src"
bash "$SRC/scripts/ios/build-engine.sh" ios-sim on
bash "$SRC/scripts/ios/build-engine.sh" ios-device on
OUT="$SRC/app/native_ios/dapple_engine/ios/dapple_engine/Frameworks"
rm -rf "$OUT/dapple.xcframework"; mkdir -p "$OUT"
xcodebuild -create-xcframework -framework "$B/ios-device/bin/dapple.framework" -framework "$B/ios-sim/bin/dapple.framework" -output "$OUT/dapple.xcframework"
echo "xcframework: $OUT/dapple.xcframework"
