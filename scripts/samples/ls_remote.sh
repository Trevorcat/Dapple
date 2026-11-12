#!/usr/bin/env bash
# usage: ls_remote.sh "Maker/Model"  -> prints "size<TAB>filename" for each file in raw.pixls.us/data/<Maker>/<Model>/
# Generic client string, no project info is sent.
set -u
p="${1// /%20}"
curl -sSL -A "curl-client" -m 60 "https://raw.pixls.us/data/$p/" | python -I -c '
import sys,re,html
t=sys.stdin.read()
for m in re.finditer(r"<a href=\"([^\"?/][^\"]*)\">[^<]*</a></td><td[^>]*>[^<]*</td><td[^>]*>\s*([^<]*?)\s*</td>",t):
    print(m.group(2)+"\t"+html.unescape(m.group(1)))
'
