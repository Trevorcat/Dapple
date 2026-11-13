#!/usr/bin/env bash
# ON THE PC: pull the frame-trace files of an autotest run. pull-traces.sh <xr|x> <tag> <outdir>   (names: warm A1 A2 B_{exposure,texture}_<stock>)
set -uo pipefail
cd "$(dirname "$0")/../.."
AF=/f/projects/autotest_framework; PM="$AF/.venv/Scripts/python.exe -m pymobiledevice3"
case "$1" in xr) U=00008020-00151DE10CF1002E; B=com.lumi.dapple.00008020;; x) U=26d2a524a312a8bf3ea6ff9864d5d59eedd71b5b; B=com.lumi.dapple.26d2a524;; esac
TAG="$2"; O="$3"; mkdir -p "$O"
for n in warm A1 A2 B_exposure_kodak_portra_400 B_texture_kodak_portra_400 B_exposure_fujifilm_c200 B_texture_fujifilm_c200; do
  for e in tsv events.txt meta.txt; do
    $PM apps pull $B Documents/trace_${TAG}_$n.$e "$O/${1}_${TAG}_$n.$e" --udid $U > /dev/null 2>&1
  done
done
ls "$O" | wc -l
