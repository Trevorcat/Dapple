#!/usr/bin/env bash
# One-way PC -> Mac sync: snapshot of git HEAD (git archive) + the iOS-port files listed in scripts/ios/overlay.txt.
# Never copies tools/ or build/ (they are untracked) and never anything outside the listed paths. ~/dapple/src/tools/_dl on the
# Mac (written by bootstrap.sh) is preserved: extraction only adds/overwrites, nothing is deleted except stale tracked files
# when --clean is given.   Usage: scripts/ios/sync.sh [host]
set -euo pipefail
cd "$(dirname "$0")/../.."
H="${1:-dapple-mac}"
# overlay entries: tracked-or-new paths; a path starting with '!' is taken from the Mac-only patch dir (scripts/ios/macpatch)
LIST=$(grep -v '^\s*#' scripts/ios/overlay.txt | grep -v '^\s*$' | while read -r p; do ls -d $p 2>/dev/null || true; done | sort -u)
echo "overlay: $(echo "$LIST" | wc -l) paths"
# shellcheck disable=SC2086
{ git -c core.autocrlf=false archive HEAD | ssh "$H" 'mkdir -p ~/dapple/src && tar -x -C ~/dapple/src'; }
tar -c $LIST | ssh "$H" 'tar -x -C ~/dapple/src'
# Mac-only replacement of shared files whose working-tree version carries other engineers' unfinished edits:
# scripts/ios/macpatch/<same relative path> overrides the HEAD copy (kept as a small diff-able copy).
if [ -d scripts/ios/macpatch ]; then tar -c -C scripts/ios/macpatch . | ssh "$H" 'tar -x -C ~/dapple/src'; fi
ssh "$H" 'date > ~/dapple/src/.synced'
# Shared files whose PC working copy carries other engineers' unfinished edits are not overlaid; the iOS hunk is applied to the
# HEAD copy on the Mac instead:
#   app/pubspec.yaml: the dapple_engine path dependency
ssh "$H" 'python3 - <<"PY"
import os
p=os.path.expanduser("~/dapple/src/app/pubspec.yaml")
s=open(p).read()
if "dapple_engine" not in s:
    a="  permission_handler: 12.0.1\n"
    assert a in s
    s=s.replace(a,a+"  dapple_engine:\n    path: native_ios/dapple_engine\n",1)
    open(p,"w").write(s)
PY'
ssh "$H" 'find ~/dapple/src/scripts/ios -name "*.sh" -exec sed -i "" "s/\r$//" {} +'
