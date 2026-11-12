#!/usr/bin/env bash
# Run a command on the Mac inside the dapple environment:  scripts/ios/mac.sh 'cmake --build ...'
exec ssh dapple-mac "source ~/dapple/src/scripts/ios/env.sh; $*"
