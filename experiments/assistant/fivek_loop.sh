#!/bin/sh
# While the FiveK subset downloads: prepare new photos, render arm A, run the oracle fit. Ends when the downloader printed DONE.
cd "$(dirname "$0")/../.."
P=tools/py-ai/Scripts/python.exe
while true; do
  fin=0; grep -q DONE build/assistant/logs/fivek_dl.log && fin=1
  $P -I experiments/assistant/run.py prep 2>&1 | grep -a --line-buffered -E "^prep"
  $P -I experiments/assistant/run.py baseline 2>&1 | grep -a --line-buffered -E "^baseline|Error"
  $P -I experiments/assistant/fivek_eval.py oracle 2>&1 | grep -a --line-buffered -E "^oracle|^skip|Error"
  [ $fin = 1 ] && break
  sleep 300
done
echo "fivek loop done $(date +%T)"
