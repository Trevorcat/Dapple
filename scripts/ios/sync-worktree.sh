#!/usr/bin/env bash
# One-way PC -> Mac sync of the CURRENT working tree (tracked + untracked, minus ignored: no tools/, build/) plus tools/shaderc.
# Files only ever flow PC -> Mac; ~/dapple/src/tools/_dl (written by bootstrap.sh) is kept. Usage: sync-worktree.sh [host]
set -euo pipefail
cd "$(dirname "$0")/../.."
H="${1:-dapple-mac}"
LIST="$(mktemp)"; DEL="$(mktemp)"
git ls-files -d > "$DEL"
python -I - "$DEL" "$LIST" <<'PY'
import subprocess, sys, os
dele = set(open(sys.argv[1], encoding='utf-8').read().split('\n'))
names = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z']).decode('utf-8').split('\0')
with open(sys.argv[2], 'wb') as f:
    for n in names:
        if n and n not in dele and os.path.isfile(n):
            f.write(n.encode('utf-8') + b'\0')
PY
tar --null -T "$LIST" -cf - | ssh "$H" 'mkdir -p ~/dapple/src && tar -x -m -C ~/dapple/src'
[ -s "$DEL" ] && ssh "$H" 'cd ~/dapple/src && xargs rm -f' < "$DEL"
tar -c tools/shaderc | ssh "$H" 'tar -x -m -C ~/dapple/src'
ssh "$H" 'find ~/dapple/src/scripts/ios -name "*.sh" -exec sed -i "" "s/\r$//" {} + ; date > ~/dapple/src/.synced-worktree'
rm -f "$LIST" "$DEL"
