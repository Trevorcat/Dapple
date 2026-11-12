#!/usr/bin/env bash
# ON THE MAC: headless simulator run of the Flutter app (xcrun simctl only, never opens Simulator.app).
#   sim-run.sh [shot-prefix] [open-group]      screenshots go to ~/dapple/out/<prefix>_N.png
set -euo pipefail
source "$HOME/dapple/src/scripts/ios/env.sh"
P="${1:-sim}"; GROUP="${2:-}"
APP="$HOME/dapple/src/app/build/ios/iphonesimulator/Runner.app"; BID=com.dapple.dapple
OUT="$HOME/dapple/out"; mkdir -p "$OUT"
UDID=$(xcrun simctl list devices | grep -E "dapple-iphone \(" | head -1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/' || true)
if [ -z "$UDID" ]; then UDID=$(xcrun simctl create dapple-iphone com.apple.CoreSimulator.SimDeviceType.iPhone-16 com.apple.CoreSimulator.SimRuntime.iOS-18-3); fi
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || xcrun simctl boot "$UDID" || true
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || true
xcrun simctl terminate "$UDID" $BID >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP"
xcrun simctl launch "$UDID" $BID >/dev/null; sleep 5; xcrun simctl terminate "$UDID" $BID >/dev/null 2>&1 || true
DATA=$(xcrun simctl get_app_container "$UDID" $BID data)
mkdir -p "$DATA/Documents/Photos"; cp "$HOME"/dapple/samples/DAPPLE_sample_*.jpg "$DATA/Documents/Photos/"
printf "%s" "$GROUP" > "$DATA/Documents/DAPPLE_OPEN"
xcrun simctl launch "$UDID" $BID > "$OUT/${P}_launch.log" 2>&1 || true
for i in 1 2 3; do sleep 6; xcrun simctl io "$UDID" screenshot "$OUT/${P}_$i.png" >/dev/null 2>&1; done
echo "udid $UDID  data $DATA"; ls -la "$OUT"/${P}_*.png
