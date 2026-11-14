#!/bin/sh
# Follow-up after chain1 (replaces master.sh from its 4B stage on). Logs in build/assistant/logs/master2.log.
cd "$(dirname "$0")/../.."
P=tools/py-ai/Scripts/python.exe
echo "== stage taste+fivek (4B) $(date +%T)"
ASSISTANT_PORT=8093 sh experiments/assistant/chain.sh "gemma4-e4b qwen3vl-4b" "taste fivek"
# wait for the small-model chain (taste+fivek) that is still running
until [ -z "$(ps -ef | grep 'chain.sh "gemma4-e2b qwen3vl-2b qwen35-4b"' | grep -v grep)" ] && grep -q "qwen35-4b fivek" build/assistant/logs/master.log && tail -n 1 build/assistant/logs/master.log | grep -q "chain done"; do sleep 30; done
echo "== stage retry errored rows $(date +%T)"
for f in build/assistant/results/*/*.jsonl; do grep -v '"error"' "$f" > "$f.tmp"; mv "$f.tmp" "$f"; done
sh experiments/assistant/chain.sh "smolvlm2-2b minicpmv4" "core"
sh experiments/assistant/chain.sh "gemma4-e2b qwen3vl-2b gemma4-e4b qwen3vl-4b qwen35-4b" "full"
echo "== stage q8 $(date +%T)"
sh experiments/assistant/chain.sh "qwen3vl-4b-q8" "core fivek"
echo "== stage clean latency (GPU alone) $(date +%T)"
sh experiments/assistant/chain.sh "gemma4-e2b qwen3vl-2b gemma4-e4b qwen3vl-4b qwen35-4b smolvlm2-2b minicpmv4" "lat"
echo "== stage cpu 4 threads $(date +%T)"
for m in gemma4-e2b qwen3vl-2b gemma4-e4b qwen3vl-4b qwen35-4b; do
  echo "=== $m cpu $(date +%T)"
  $P -I experiments/assistant/run.py vlm $m cpu cpu 4 2>&1 | grep -a --line-buffered -v "^dapple_engine\|XNNPACK\|overflow\|sigmoid\|data corrupted"
done
echo "== master done $(date +%T)"
