#!/usr/bin/env bash
# usage: download.sh <list.tsv> <dest root>   -- sequential, polite, generic client string; files are data only, never executed.
set -u
list="$1"; root="$2"
enc() { python -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$1"; }
mkdir -p "$root"
: > "$root/downloads.tsv"
grep -v '^#' "$list" | while IFS=$'\t' read -r rdir f ldir; do
  [ -z "$f" ] && continue
  d="$root/$ldir"; mkdir -p "$d"
  url="https://raw.pixls.us/data/$(echo -n "$rdir" | python -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.stdin.read()))')/$(enc "$f")"
  if [ -s "$d/$f" ]; then echo "have $f"; else
    curl -sSL -A "curl-client" -m 900 --fail "$url" -o "$d/$f.part" && mv "$d/$f.part" "$d/$f" || { echo "FAIL $url"; rm -f "$d/$f.part"; printf '%s\t%s\tFAIL\n' "$ldir/$f" "$url" >> "$root/downloads.tsv"; sleep 3; continue; }
    sleep 3
  fi
  sz=$(stat -c %s "$d/$f"); sha=$(sha256sum "$d/$f" | cut -d' ' -f1)
  printf '%s\t%s\t%s\t%s\t%s\n' "$ldir/$f" "$rdir" "$url" "$sz" "$sha" >> "$root/downloads.tsv"
  echo "ok $ldir/$f $sz"
done
