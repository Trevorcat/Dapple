#!/usr/bin/env bash
# ON THE PC: install a trace build and run the frame-trace scenarios on one phone, pull the traces. run-trace-pair.sh <xr|x> <ipa> <tag> <prebake true|false>
set -uo pipefail
cd "$(dirname "$0")/../.."
dev=$1; ipa=$2; tag=$3; pre=$4
export AUTOTEST_JSON='{"files":[],"trace":{"file":"a7m5.ARW","tag":"'$tag'","prebake":'$pre',"base":"kodak_portra_400","slide":["kodak_portra_160","kodak_ektar_100","fujifilm_c200","kodak_gold_200","fujifilm_pro_400h","kodak_vision3_500t","kodak_ultramax_400","fujifilm_xtra_400"],"padStocks":["kodak_portra_400","fujifilm_c200"],"slideMs":150,"dragMs":2000}}'
timeout 1200 bash scripts/ios/device-run.sh $dev $ipa ${dev}_$tag 2>&1 | tail -1
bash scripts/ios/pull-traces.sh $dev $tag build/ios/device/traces > /dev/null
