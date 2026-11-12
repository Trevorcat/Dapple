#!/usr/bin/env bash
# ON THE PC (Git Bash): full on-device measurement cycle for one phone. device-run.sh <xr|x> <unsigned.ipa> <prefix>
# uninstall -> sign -> install -> launch once -> push samples (build/ios/device/push/Photos) + autotest.json -> run -> pull the log.
set -uo pipefail
cd "$(dirname "$0")/../.."
AF=/f/projects/autotest_framework; PY="$AF/.venv/Scripts/python.exe"; PM="$PY -m pymobiledevice3"
case "$1" in xr) U=00008020-00151DE10CF1002E; B=com.lumi.dapple.00008020;; x) U=26d2a524a312a8bf3ea6ff9864d5d59eedd71b5b; B=com.lumi.dapple.26d2a524;; esac
DEV="$1"; IPA="$2"; PRE="$3"; O=build/ios/device; PUSH=$O/push
$PM apps uninstall $B --udid $U > /dev/null 2>&1; sleep 3
bash scripts/ios/device-test.sh "$DEV" "$IPA" || exit 1
$PM developer dvt launch $B --udid $U > /dev/null 2>&1; sleep 8
$PM apps push $B $PUSH/Photos Documents --udid $U 2>&1 | tail -1 | cut -c1-100
DEF='{"files":["sample.jpg","IMG_1105.dng","xs10.RAF","a7m5.ARW"],"stocks":["kodak_portra_160","kodak_ektar_100"],"drags":12,"export":false}'
printf '%s' "${AUTOTEST_JSON:-$DEF}" > $PUSH/autotest.json
$PM apps push $B $PUSH/autotest.json Documents/autotest.json --udid $U 2>&1 | tail -1 | cut -c1-100
$PM developer dvt launch $B --env DAPPLE_ENGINE_LOG=1 --udid $U 2>&1 | tail -1 | cut -c1-100
for i in $(seq 1 120); do
  sleep 10
  $PM apps pull $B Documents/autotest_log.txt $O/${PRE}_autotest_log.txt --udid $U > /dev/null 2>&1
  grep -q '"event":"done"' $O/${PRE}_autotest_log.txt 2>/dev/null && { echo DONE; break; }
done
