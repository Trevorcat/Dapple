#!/bin/sh
# Runs tiers for a list of models sequentially (one llama-server at a time). Usage: sh chain.sh "<models>" "<tiers>"
cd "$(dirname "$0")/../.."
for m in $1; do for t in $2; do
  echo "=== $m $t $(date +%T)"
  tools/py-ai/Scripts/python.exe -I experiments/assistant/run.py vlm $m $t 2>&1 | grep -a --line-buffered -v "^dapple_engine\|XNNPACK\|overflow\|sigmoid\|data corrupted"
done; done
echo "=== chain done $(date +%T)"
