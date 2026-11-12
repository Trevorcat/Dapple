"""gen_manifest.py <samples_root> : write MANIFEST.md from downloads.tsv (python -I)"""
import os, sys
root = sys.argv[1]
rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(root, "downloads.tsv"), encoding="utf-8") if l.strip()]
lines = ["# Public RAW sample set (raw.pixls.us)", "",
         "Source: https://raw.pixls.us/ , directory https://raw.pixls.us/data/ . The site's upload form states: \"I declare that I own full rights to this file and I hereby release it under the CC0 license into the public domain\" (https://creativecommons.org/share-your-work/public-domain/cc0/); samples that are not CC0 are listed separately on the site (?noncc0) and none were taken. Licence of every file below: CC0 1.0 (public domain dedication).",
         "Downloaded sequentially with a generic client string (curl-client), nothing about the owner or project was sent. Treat the files as untrusted data.", "",
         "| file | camera (site folder) | URL | licence | size (bytes) | sha256 |", "|---|---|---|---|---|---|"]
tot = 0
for r in rows:
    if len(r) < 5:
        lines.append("| %s | | %s | | FAILED | |" % (r[0], r[1] if len(r) > 1 else "")); continue
    rel, rdir, url, size, sha = r[:5]
    tot += int(size)
    lines.append("| %s | %s | %s | CC0 1.0 | %s | %s |" % (rel, rdir, url, size, sha))
lines += ["", "Total: %d files, %.2f GB." % (len(rows), tot / 1e9)]
open(os.path.join(root, "MANIFEST.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(len(rows), tot)
