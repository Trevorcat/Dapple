"""Dump the two-sentence Chinese reasons of 20 images per model (C_if1) next to the measured facts, for reading (metric 7).
python -I experiments/assistant/rationale_dump.py > build/assistant/rationales.md"""
import json, os, sys
sys.stdout.reconfigure(encoding="utf-8")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze, vision

F = analyze.FEATS
ids = sorted(i for i in F if not i.startswith('fk_') and 'f' in F[i])[::2][:20]
print('# Chinese rationales, C_if1 (image + facts, 1 round, T=0)\n')
for i in ids:
    m = F[i]['m']; f = F[i]['f']
    print(f"## {i}\nfacts: median L* {m['L_p50']:.0f}, p95 {m['L_p95']:.0f}, cast {vision.cast_word(m['cast_a'], m['cast_b'])} (b* {m['cast_b']:+.1f}), "
          f"sky {f['sky'] * 100:.0f}%, faces {int(f['faces'])}, person {f['person'] * 100:.0f}%, clip {m['clip_hi'] * 100:.1f}%\n")
    for mod in analyze.models():
        r = analyze.load(mod, 'C_if1').get(i)
        if not r or 'edit' not in r: continue
        rc = r['rationale']
        print(f"- **{mod}** ({analyze.tools.describe(r['edit'])}; ids {','.join(r['reasons'])}; flagged: {','.join(rc['details']) or '-'}): {r['reason_zh']}")
    print()
