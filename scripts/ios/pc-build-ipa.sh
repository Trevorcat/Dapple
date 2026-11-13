#!/usr/bin/env bash
# ON THE PC: sync the working tree to the Mac, build the device engine + the autotest ipa there, copy it to build/ios/<name>.ipa. pc-build-ipa.sh <name>
set -euo pipefail
cd "$(dirname "$0")/.."/..
bash scripts/ios/sync-worktree.sh 2>&1 | tail -1
bash scripts/ios/mac.sh 'cd ~/dapple/src && bash scripts/ios/build-engine.sh ios-device on 2>&1 | grep -E " error |FAILED|Linking CXX shared" | head -20
B=$HOME/dapple/build; OUT=$HOME/dapple/src/app/native_ios/dapple_engine/ios/dapple_engine/Frameworks; rm -rf $OUT/dapple.xcframework; mkdir -p $OUT
xcodebuild -create-xcframework -framework $B/ios-device/bin/dapple.framework -output $OUT/dapple.xcframework 2>&1 | tail -1
cd ~/dapple/src/app && flutter build ios --release --no-codesign --dart-define=DAPPLE_IOS_AUTOTEST=true 2>&1 | grep -E "error|Built" | head
bash ~/dapple/src/scripts/ios/make-ipa.sh' 2>&1 | tail -6
scp -q dapple-mac:dapple/out/Dapple-unsigned.ipa "build/ios/$1.ipa"; ls -la "build/ios/$1.ipa"
