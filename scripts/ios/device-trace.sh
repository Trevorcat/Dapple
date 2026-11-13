#!/usr/bin/env bash
# ON THE PC: re-run the frame-trace scenarios on an already installed autotest build. device-trace.sh <xr|x> <tag> '<env KEY=VAL ...>' (autotest.json is built here)
set -uo pipefail
cd "$(dirname "$0")/../.."
AF=/f/projects/autotest_framework; PM="$AF/.venv/Scripts/python.exe -m pymobiledevice3"
case "$1" in xr) U=00008020-00151DE10CF1002E; B=com.lumi.dapple.00008020;; x) U=26d2a524a312a8bf3ea6ff9864d5d59eedd71b5b; B=com.lumi.dapple.26d2a524;; esac
TAG="$2"; ENVS="${3:-}"; O=build/ios/device; PUSH=$O/push
J='{"files":[],"trace":{"file":"a7m5.ARW","tag":"'$TAG'","base":"kodak_portra_400","slide":["kodak_portra_160","kodak_ektar_100","fujifilm_c200","kodak_gold_200","fujifilm_pro_400h","kodak_vision3_500t","kodak_ultramax_400","fujifilm_xtra_400"],"padStocks":["kodak_portra_400","fujifilm_c200"],"slideMs":150,"dragMs":2000}}'
printf '%s' "$J" > $PUSH/autotest.json
$PM apps push $B $PUSH/autotest.json Documents/autotest.json --udid $U 2>&1 | tail -1 | cut -c1-100
EA=(); for e in $ENVS; do EA+=(--env "$e"); done
$PM developer dvt launch $B --env DAPPLE_ENGINE_LOG=1 "${EA[@]}" --udid $U 2>&1 | tail -1 | cut -c1-100
for i in $(seq 1 120); do
  sleep 8
  $PM apps pull $B Documents/autotest_log.txt $O/${TAG}_autotest_log.txt --udid $U > /dev/null 2>&1
  grep -q '"event":"done"' $O/${TAG}_autotest_log.txt 2>/dev/null && { echo DONE; break; }
done
bash scripts/ios/pull-traces.sh "$1" "$TAG" $O/traces
