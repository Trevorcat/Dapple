#!/usr/bin/env bash
# ON THE MAC: unsigned device .ipa from the release build (signing happens elsewhere: docs/ios/README.md "Device").
set -euo pipefail
APP="$HOME/dapple/src/app/build/ios/iphoneos/Runner.app"; OUT="$HOME/dapple/out"; T=$(mktemp -d)
mkdir -p "$T/Payload" "$OUT"; cp -R "$APP" "$T/Payload/"
( cd "$T" && zip -qry "$OUT/Dapple-unsigned.ipa" Payload )
rm -rf "$T"; ls -la "$OUT/Dapple-unsigned.ipa"
