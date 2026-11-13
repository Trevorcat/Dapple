#!/usr/bin/env bash
# ON THE MAC: engine for iOS device + simulator -> dapple.xcframework inside the Flutter plugin app/native_ios/dapple_engine.
set -euo pipefail
B="$HOME/dapple/build"; SRC="$HOME/dapple/src"
bash "$SRC/scripts/ios/build-engine.sh" ios-sim on
bash "$SRC/scripts/ios/build-engine.sh" ios-device on
OUT="$SRC/app/native_ios/dapple_engine/ios/dapple_engine/Frameworks"
rm -rf "$OUT/dapple.xcframework"; mkdir -p "$OUT"
xcodebuild -create-xcframework -framework "$B/ios-device/bin/dapple.framework" -framework "$B/ios-sim/bin/dapple.framework" -output "$OUT/dapple.xcframework"
# both slices or nothing: UI engineers need the simulator slice, a device-only framework breaks `flutter build ios --simulator`
for S in ios-arm64 ios-arm64-simulator; do
  [ -d "$OUT/dapple.xcframework/$S" ] || { echo "xcframework is missing the slice $S"; exit 1; }
done
echo "xcframework: $OUT/dapple.xcframework (device + simulator)"
