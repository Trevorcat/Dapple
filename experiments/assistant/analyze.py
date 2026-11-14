"""Tables from build/assistant/results -> build/assistant/report.md (+ report.json). python -I experiments/assistant/analyze.py"""
import glob, itertools, json, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import taste, tools, vision

OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'assistant'))
RES = os.path.join(OUT, 'results')
FEATS = json.load(open(os.path.join(OUT, 'features.json'))) if os.path.exists(os.path.join(OUT, 'features.json')) else {}


def load(model, cfg):
    p = os.path.join(RES, model, cfg + '.jsonl')
    if not os.path.exists(p): return {}
    out = {}
    for l in open(p, encoding='utf-8'):
        if l.strip():
            r = json.loads(l); out[r['id']] = r
    return out


def models():
    return sorted(d for d in os.listdir(RES) if os.path.isdir(os.path.join(RES, d)) and d != 'engine') if os.path.exists(RES) else []


def l1(a, b):
    return taste.edit_distance(a, b)[0]


def corr(x, y):
    x, y = np.asarray(x, float), np.asarray(y, float)
    if len(x) < 5 or x.std() < 1e-9 or y.std() < 1e-9: return float('nan')
    return float(np.corrcoef(x, y)[0, 1])


def action_check(text, e):
    """Does the Chinese text describe the action the call actually took? Returns (claims, mismatches)."""
    import re
    t = text or ''; ar = e.get('areas', [])
    up = e['light']['x'] > 0 or any(a['op'] in ('lift', 'pop') for a in ar)
    down = e['light']['x'] < 0 or any(a['op'] == 'darken' for a in ar)
    warm = e['color']['x'] > 0 or any(a['op'] == 'warm' for a in ar) or (e['base'] not in tools.MODERN and e['base'] != 'none')
    cool = e['color']['x'] < 0 or any(a['op'] == 'cool' for a in ar)
    soft = any(a['op'] == 'soften' for a in ar) or e['texture']['x'] < 0
    pairs = [(re.search('提亮|提升亮度|增加亮度|调亮|亮度略微增加|增强了亮度', t), up), (re.search('压暗|变暗|降低亮度|暗化', t), down),
             (re.search('加暖|暖色|提温|更温暖|微暖|偏暖一些|暖调', t) and not re.search('偏暖[，。,；;]|色调偏暖', t), warm),
             (re.search('降温|调冷|冷色|冷一些', t), cool), (re.search('柔化|柔和处理', t), soft)]
    claims = sum(1 for said, _ in pairs if said); bad = sum(1 for said, did in pairs if said and not did)
    return claims, bad


def summary(recs):
    ok = [r for r in recs.values() if 'error' not in r]
    if not ok: return None
    calls = [rr for r in ok for rr in r['rounds']]
    c = [rr['cost'] for rr in calls]
    f = lambda k: float(np.mean([x[k] for x in c if x.get(k) is not None])) if any(x.get(k) is not None for x in c) else float('nan')
    rat = [r['rationale'] for r in ok]
    claims = sum(x['claims'] for x in rat); wrong = sum(x['wrong'] for x in rat)
    ids = [r['id'] for r in ok]
    feats = [FEATS.get(i, {}).get('f') for i in ids]
    lx = [r['edit']['light']['x'] for r, fe in zip(ok, feats) if fe]; L50 = [fe['L50'] for fe in feats if fe]
    cx = [r['edit']['color']['x'] for r, fe in zip(ok, feats) if fe]; cb = [fe['cast_b'] for fe in feats if fe]
    bases = [r['edit']['base'] for r in ok]
    return dict(n=len(ok), errors=len(recs) - len(ok), valid=float(np.mean([rr['valid'] for rr in calls])),
                rounds=float(np.mean([r['n_rounds'] for r in ok])), prompt_tok=f('prompt_tokens'), out_tok=f('completion_tokens'),
                prompt_ms=f('prompt_ms'), gen_ms=f('predicted_ms'), wall_s=f('wall_s'),
                wall_per_image=float(np.mean([sum(rr['cost']['wall_s'] for rr in r['rounds']) for r in ok])),
                guard_rate=float(np.mean([any(not g.startswith('area') for g in r['rounds'][-1]['guards']) for r in ok])),
                pad_l1=float(np.mean([l1(r['edit'], tools.empty_edit()) for r in ok])),
                base_none=bases.count('none') / len(bases), base_modern=sum(b in tools.MODERN for b in bases) / len(bases),
                base_film=sum(b not in tools.MODERN and b != 'none' for b in bases) / len(bases),
                areas=float(np.mean([len(r['edit']['areas']) for r in ok])),
                claims_per=claims / len(ok), wrong_rate=wrong / claims if claims else float('nan'),
                action_mismatch=(lambda cb: cb[1] / cb[0] if cb[0] else float('nan'))(tuple(map(sum, zip(*[action_check(r['reason_zh'], r['edit']) for r in ok])))),
                two_sent=float(np.mean([x['sentences'] == 2 for x in rat])), chinese=float(np.mean([x['chinese'] for x in rat])),
                corr_lightx_L50=corr(lx, L50), corr_colorx_castb=corr(cx, cb))


def consistency(m):
    base, rep = load(m, 'C_if1'), load(m, 'C_if1_rep')
    t = [load(m, f'C_if1_t07{s}') for s in 'abc']
    same = [tools.describe(base[i]['edit']) == tools.describe(rep[i]['edit']) for i in rep if i in base and 'edit' in rep[i] and 'edit' in base[i]]
    d07, b07, d0 = [], [], []
    for i in t[0]:
        es = [x[i]['edit'] for x in t if i in x and 'edit' in x[i]]
        if len(es) < 2: continue
        for a, b in itertools.combinations(es, 2):
            d07.append(l1(a, b)); b07.append(a['base'] == b['base'])
        if i in base and 'edit' in base[i]: d0 += [l1(base[i]['edit'], e) for e in es]
    return dict(t0_identical=float(np.mean(same)) if same else float('nan'), n_t0=len(same),
                t07_pair_l1=float(np.mean(d07)) if d07 else float('nan'), t07_base_agree=float(np.mean(b07)) if b07 else float('nan'),
                t07_vs_t0_l1=float(np.mean(d0)) if d0 else float('nan'))


def pin_check(m):
    r = load(m, 'C_if1_pin')
    ok = [all(v == 0 for v in x['edit']['light'].values()) and x['edit']['base'] == 'none' for x in r.values() if 'edit' in x]
    return float(np.mean(ok)) if ok else float('nan')


def rounds_effect(m):
    a, b = load(m, 'C_if1'), load(m, 'C_if3')
    ch = [l1(b[i]['rounds'][0]['edit'], b[i]['edit']) for i in b if 'edit' in b[i]]
    stop = [b[i]['n_rounds'] for i in b if 'edit' in b[i]]
    return dict(if3_mean_rounds=float(np.mean(stop)) if stop else float('nan'), if3_change_after_r1=float(np.mean(ch)) if ch else float('nan'),
                if3_changed_frac=float(np.mean([c > 0 for c in ch])) if ch else float('nan'))


_rule_cache = {}


def rule_img(who, iid):
    k = (who, iid)
    if k not in _rule_cache:
        p = os.path.join(OUT, 'renders', f'R_{who}', 'engine', iid + '.jpg')
        _rule_cache[k] = np.asarray(Image.open(p).convert('RGB').resize((256, 256))) if os.path.exists(p) else None
    return _rule_cache[k]


def img_de(path, ref):
    if ref is None or not os.path.exists(path): return None
    a = np.asarray(Image.open(path).convert('RGB').resize((256, 256)))
    return float(vision.de2000(vision.srgb8_to_lab(a), vision.srgb8_to_lab(ref)).mean())


def taste_table():
    rows = []
    for who in taste.PHOTOGRAPHERS:
        rule = load('engine', f'R_{who}')
        if not rule: continue
        arms = [('A (auto)', 'engine', 'A'), ('B (kNN taste)', 'engine', f'B_{who}')]
        for m in models():
            arms += [(f'C zero-shot {m}', m, 'C_if1'), (f'D VLM+profile {m}', m, f'D_{who}'), (f'F(B) semantic {m}', m, f'FB_{who}')]
        for name, m, cfg in arms:
            recs = load(m, cfg)
            ids = [i for i in recs if i in rule and 'edit' in recs[i]]
            if not ids: continue
            d = [taste.edit_distance(recs[i].get('eff', recs[i]['edit']), rule[i]['edit']) for i in ids]
            de = [img_de(os.path.join(OUT, 'renders', cfg, m, i + '.jpg'), rule_img(who, i)) for i in ids]
            de = [x for x in de if x is not None]
            rows.append(dict(who=who, arm=name, n=len(ids), pad_l1=float(np.mean([x[0] for x in d])), base_match=float(np.mean([x[1] for x in d])),
                             dE_to_rule=float(np.mean(de)) if de else float('nan')))
    return rows


def fivek_table():
    p = os.path.join(OUT, 'fivek_scores.json')
    if not os.path.exists(p): return [], None
    S = json.load(open(p)); spread = S.pop('_expert_spread_vs_C', None)
    A = S.get('A|engine', {})
    rows = []
    for k, rows_ in S.items():
        cfg, m = k.split('|')
        ids = [i for i in rows_ if 'c' in rows_[i]]
        if not ids: continue
        dc = [rows_[i]['c'][0] for i in ids]; dl = [rows_[i]['c'][1] for i in ids]
        dall = [np.mean([v[0] for v in rows_[i].values()]) for i in ids]
        common = [i for i in ids if i in A and 'c' in A[i]]
        win = float(np.mean([rows_[i]['c'][0] < A[i]['c'][0] - 0.25 for i in common])) if common else float('nan')
        loss = float(np.mean([rows_[i]['c'][0] > A[i]['c'][0] + 0.25 for i in common])) if common else float('nan')
        paired = float(np.mean([rows_[i]['c'][0] - A[i]['c'][0] for i in common])) if common else float('nan')
        paired_all = float(np.mean([np.mean([v[0] for v in rows_[i].values()]) - np.mean([v[0] for v in A[i].values()]) for i in common])) if common else float('nan')
        rows.append(dict(arm=cfg, model=m, n=len(ids), dE_C=float(np.mean(dc)), dL_C=float(np.mean(dl)), dE_all=float(np.mean(dall)),
                         paired_dE_C_vs_A=paired, paired_dE_all_vs_A=paired_all, closer_than_A=win, further_than_A=loss))
    return sorted(rows, key=lambda r: r['dE_C']), spread


def latency_table():
    rows = []
    for d in sorted(os.listdir(RES)):
        for cfg in ('lat_if3', 'cpu_if3'):
            recs = [r for r in load(d, cfg).values() if 'rounds' in r]
            if not recs: continue
            r1 = [r['rounds'][0]['cost'] for r in recs]
            later = [rr['cost'] for r in recs for rr in r['rounds'][1:]]
            meta = {}
            for mp in glob.glob(os.path.join(RES, f'meta_{d}_*.json')):
                mm = json.load(open(mp))
                if mm.get('rss_mb_end'): meta = mm
            g = lambda xs, k: float(np.median([x[k] for x in xs if x.get(k) is not None])) if xs else float('nan')
            rows.append(dict(model=d, where='GPU 4060 Ti' if cfg == 'lat_if3' else 'CPU 4 threads', n=len(recs),
                             r1_prompt_tok=g(r1, 'prompt_tokens'), r1_prefill_s=g(r1, 'prompt_ms') / 1000, r1_out_tok=g(r1, 'completion_tokens'),
                             decode_tps=g(r1, 'gen_tps'), prefill_tps=g(r1, 'prompt_tps'), r1_llm_s=g(r1, 'wall_s'),
                             later_prompt_tok=g(later, 'prompt_tokens'), later_llm_s=g(later, 'wall_s'),
                             rounds=float(np.mean([r['n_rounds'] for r in recs])),
                             total_llm_s=float(np.median([sum(rr['cost']['wall_s'] for rr in r['rounds']) for r in recs])),
                             rss_mb=meta.get('rss_mb_end', float('nan')) if cfg == 'cpu_if3' else float('nan'),
                             load_s=meta.get('load_s', float('nan'))))
    return rows


def fmt(v, p=2):
    if isinstance(v, float):
        if np.isnan(v): return '-'
        return f'{v:.{p}f}'
    return str(v)


def table(rows, cols):
    out = ['| ' + ' | '.join(cols) + ' |', '|' + '---|' * len(cols)]
    for r in rows: out.append('| ' + ' | '.join(fmt(r.get(c, '')) for c in cols) + ' |')
    return '\n'.join(out)


def main():
    rep = dict(summary={}, consistency={}, pin={}, rounds={})
    md = ['# Assistant prototype: generated tables', '']
    rows = []
    for m in models():
        for cfg in sorted(os.path.basename(p)[:-6] for p in glob.glob(os.path.join(RES, m, '*.jsonl'))):
            s = summary(load(m, cfg))
            if s: s.update(model=m, config=cfg); rows.append(s); rep['summary'][f'{m}|{cfg}'] = s
        rep['consistency'][m] = consistency(m); rep['pin'][m] = pin_check(m); rep['rounds'][m] = rounds_effect(m)
    md += ['## Per model and configuration', table(rows, ['model', 'config', 'n', 'errors', 'valid', 'rounds', 'prompt_tok', 'out_tok', 'prompt_ms', 'gen_ms',
                                                          'wall_per_image', 'guard_rate', 'pad_l1', 'base_none', 'base_modern', 'base_film', 'areas',
                                                          'claims_per', 'wrong_rate', 'action_mismatch', 'two_sent', 'chinese', 'corr_lightx_L50', 'corr_colorx_castb']), '']
    cr = [dict(model=m, **rep['consistency'][m], pin_respected=rep['pin'][m], **rep['rounds'][m]) for m in models()]
    md += ['## Consistency, pins, rounds', table(cr, ['model', 't0_identical', 'n_t0', 't07_pair_l1', 't07_base_agree', 't07_vs_t0_l1', 'pin_respected',
                                                     'if3_mean_rounds', 'if3_change_after_r1', 'if3_changed_frac']), '']
    lt = latency_table(); rep['latency'] = lt
    md += ['## Latency (lat_if3: GPU alone; cpu_if3: llama.cpp CPU build, 4 threads; 3-round loop, stops when the call repeats)',
           table(lt, ['model', 'where', 'n', 'r1_prompt_tok', 'r1_prefill_s', 'prefill_tps', 'r1_out_tok', 'decode_tps', 'r1_llm_s', 'later_prompt_tok',
                      'later_llm_s', 'rounds', 'total_llm_s', 'rss_mb', 'load_s']), '']
    tt = taste_table(); rep['taste'] = tt
    md += ['## Taste following (held-out general set; distance to the photographer\'s noise-free rule)', table(tt, ['who', 'arm', 'n', 'pad_l1', 'base_match', 'dE_to_rule']), '']
    fk, spread = fivek_table(); rep['fivek'] = fk; rep['fivek_expert_spread'] = spread
    md += [f'## FiveK (distance to expert renditions; inter-expert spread vs C = {fmt(spread)} dE2000)',
           table(fk, ['arm', 'model', 'n', 'dE_C', 'dL_C', 'dE_all', 'paired_dE_C_vs_A', 'paired_dE_all_vs_A', 'closer_than_A', 'further_than_A']), '']
    json.dump(rep, open(os.path.join(OUT, 'report.json'), 'w'), indent=1)
    open(os.path.join(OUT, 'report.md'), 'w', encoding='utf-8').write('\n'.join(md))
    print('\n'.join(md))


if __name__ == '__main__':
    main()
