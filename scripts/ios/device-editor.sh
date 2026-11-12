#!/usr/bin/env bash
# ON THE PC: after device-run.sh: open the editor on a sample with a film applied (marker file), probe the zero-copy view, screenshot, pull logs.
#   device-editor.sh <xr|x> <name-substring> <stock>
set -uo pipefail
cd "$(dirname "$0")/../.."
AF=/f/projects/autotest_framework; PY="$AF/.venv/Scripts/python.exe"; PM="$PY -m pymobiledevice3"
case "$1" in xr) U=00008020-00151DE10CF1002E; B=com.lumi.dapple.00008020;; x) U=26d2a524a312a8bf3ea6ff9864d5d59eedd71b5b; B=com.lumi.dapple.26d2a524;; esac
O=build/ios/device; DEV="$1"; WHO="$2"; STOCK="$3"
$PM apps rm $B Documents/autotest.json --udid $U >/dev/null 2>&1
echo "{\"documents\":true,\"open\":\"$WHO\",\"group\":\"style\",\"film\":\"$STOCK\",\"probeView\":true}" > $O/push/DAPPLE_OPEN
$PM apps push $B $O/push/DAPPLE_OPEN Documents/DAPPLE_OPEN --udid $U 2>&1 | tail -1 | cut -c1-80
$PM developer dvt launch $B --udid $U 2>&1 | tail -1 | cut -c1-80
sleep 35
$PM developer dvt screenshot $O/../device_${DEV}_editor_${STOCK}.png --udid $U 2>&1 | tail -1 | cut -c1-60
sleep 10
$PM apps pull $B Documents/probe_log.txt $O/${DEV}_probe_log.txt --udid $U >/dev/null 2>&1
cat $O/${DEV}_probe_log.txt 2>/dev/null | cut -c1-900
