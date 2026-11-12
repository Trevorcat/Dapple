#!/usr/bin/env bash
# ON THE PC (Git Bash): sign the unsigned .ipa with the autotest framework's plumesign + saved session, install on one iPhone with its
# pymobiledevice3, push the public CC0 samples into the app container, run the on-device measurement script, pull the log, screenshots.
#   device-test.sh <xr|x> <unsigned.ipa> [files...]
# Needs: a build made with --dart-define=DAPPLE_IOS_AUTOTEST=true. Reads (never writes) the framework's venv; never prints credentials.
set -euo pipefail
cd "$(dirname "$0")/../.."
W="$(pwd)"; AF=/f/projects/autotest_framework
PY="$AF/.venv/Scripts/python.exe"; PM="$PY -m pymobiledevice3"
P="$AF/tools/ios/plumesign-2.7.2/plumesign-windows-x86_64.exe"
export APPDATA="$LOCALAPPDATA/autotest-framework/ios/plume"
case "$1" in
  xr) UDID=00008020-00151DE10CF1002E; BID=com.lumi.dapple.00008020;;
  x)  UDID=26d2a524a312a8bf3ea6ff9864d5d59eedd71b5b; BID=com.lumi.dapple.26d2a524;;
  *) echo "device xr|x"; exit 2;;
esac
DEV="$1"; IPA="$2"; shift 2
OUT="$W/build/ios"; mkdir -p "$OUT/device"
SIGNED="$OUT/device/$(basename "${IPA%.ipa}")-$BID-signed.ipa"
NO_COLOR=1 RUST_LOG=warn "$P" sign -p "$IPA" --apple-id --custom-identifier "$BID" -o "$SIGNED" < /dev/null > "$OUT/device/sign_$DEV.log" 2>&1 || true
[ -s "$SIGNED" ] || { echo "signing failed, see $OUT/device/sign_*.log"; exit 1; }
$PM apps install "$SIGNED" --udid "$UDID" 2>&1 | tail -1
