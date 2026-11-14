"""Experiment runner. Results: build/assistant/results/<model>/<config>.jsonl (one record per image), renders under
build/assistant/renders/<config>/<model>/<id>.jpg. Re-running skips finished (model, config, image) triples.
  python -I experiments/assistant/run.py prep                     # prepare all images (auto, masks, facts)
  python -I experiments/assistant/run.py baseline                 # arm A (+ B for the synthetic photographers) renders
  python -I experiments/assistant/run.py vlm <model> <tier>       # tier: core | full | taste | fivek | cpu
"""
import json, os, subprocess, sys, time, traceback
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assist, data, llm, taste, tools, vision

OUT = data.OUT
RES = os.path.join(OUT, 'results')
REN = os.path.join(OUT, 'renders')
SUB20 = 20


def sets():
    s = data.image_sets()
    gen = s['raw'] + s['portrait']
    return gen, s['fivek']


def done_ids(path):
    if not os.path.exists(path): return set()
    return {json.loads(l)['id'] for l in open(path, encoding='utf-8') if l.strip()}


def save(model, config, rec, img=None):
    d = os.path.join(RES, model); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, config + '.jsonl'), 'a', encoding='utf-8') as f: f.write(json.dumps(rec, ensure_ascii=False) + '\n')
    if img is not None:
        p = os.path.join(REN, config, model); os.makedirs(p, exist_ok=True)
        Image.fromarray(img).save(os.path.join(p, rec['id'] + '.jpg'), quality=90)


def features_all():
    p = os.path.join(OUT, 'features.json')
    return json.load(open(p)) if os.path.exists(p) else {}


def cmd_prep():
    gen, five = sets(); feats = features_all()
    for path in gen + five:
        iid = data.image_id(path)
        if iid in feats: continue
        try:
            P = data.Prepared(path)
            feats[iid] = dict(path=path, f=P.features(), m=P.m_auto, facts=P.facts(), info={k: P.info.get(k) for k in ('make', 'model', 'iso', 'shutter', 'aperture', 'focalLength', 'timestamp')},
                              tone={k: P.tone.get(k) for k in ('key', 'range', 'nearest')}, rasters=sorted(P.rasters), prep_s=P.prep_s)
            P.close(); print('prep', iid, f'{feats[iid]["prep_s"]:.1f}s', flush=True)
        except Exception as e:
            print('prep FAILED', iid, e, flush=True); feats[iid] = dict(path=path, error=str(e))
        json.dump(feats, open(os.path.join(OUT, 'features.json'), 'w'), indent=0)


def group(iid):
    """Same camera folder = same shoot (near-duplicate files): never in each other's history."""
    return '_'.join(iid.split('_')[:3]) if iid.startswith('raw_') else iid


def histories():
    """History = the general set's photos (scene features only), used leave-one-shoot-out: for a held-out photo the history is every
    OTHER photo of the general set except files of the same camera folder (near-duplicates). The FiveK download was too slow to wait for."""
    feats = features_all()
    rows = [(i, v['f']) for i, v in feats.items() if not i.startswith('fk_') and 'f' in v]
    return {who: taste.make_history(who, rows) for who in taste.PHOTOGRAPHERS}


def hist_for(hist, iid):
    return [h for h in hist if group(h['photo']) != group(iid)]


def cmd_baseline():
    gen, five = sets(); hist = histories()
    for path in gen + five:
        iid = data.image_id(path)
        need = [c for c in ['A'] if iid not in done_ids(os.path.join(RES, 'engine', c + '.jsonl'))]
        if path in gen:
            for who in taste.PHOTOGRAPHERS:
                for c in (f'B_{who}', f'R_{who}'):
                    if iid not in done_ids(os.path.join(RES, 'engine', c + '.jsonl')): need.append(c)
        if not need: continue
        P = data.Prepared(path)
        for c in need:
            if c == 'A': e = tools.empty_edit(); img, m, scale, ev = P.auto_img, P.m_auto, {}, []
            else:
                who = c.split('_', 1)[1]
                e = taste.knn_predict(hist_for(hist[who], iid), P.features())[0] if c.startswith('B_') else taste.PHOTOGRAPHERS[who](P.features())
                img, m, scale, ev = assist.guard(P, e) if c.startswith('B_') else (*[P.render(e)[0]], None, {}, [])
                if m is None: m = vision.measure(img, P.planes, P.faces)
            save('engine', c, dict(id=iid, edit=e, eff=assist.effective(e, scale), m=m, guards=ev), img)
        P.close(); print('baseline', iid, need, flush=True)


CONFIGS = {
    # name: (mode, rounds, temperature, semantic_only, extra)
    'C_if1': ('image+facts', 1, 0.0, False, {}),
    'C_i1': ('image', 1, 0.0, False, {}),
    'C_f1': ('facts', 1, 0.0, False, {}),
    'C_if3': ('image+facts', 3, 0.0, False, {}),
    'F_A': ('image+facts', 1, 0.0, True, {}),
    'C_if1_rep': ('image+facts', 1, 0.0, False, {'subset': SUB20}),
    'C_if1_t07a': ('image+facts', 1, 0.7, False, {'subset': SUB20, 'seed': 101}),
    'C_if1_t07b': ('image+facts', 1, 0.7, False, {'subset': SUB20, 'seed': 202}),
    'C_if1_t07c': ('image+facts', 1, 0.7, False, {'subset': SUB20, 'seed': 303}),
    'C_if1_free': ('image+facts', 1, 0.0, False, {'subset': SUB20, 'constrained': False}),
    'C_if1_pin': ('image+facts', 1, 0.0, False, {'subset': SUB20, 'pinned': ('light', 'base')}),
    'cpu_if3': ('image+facts', 3, 0.0, False, {'subset': 4}),
    'lat_if3': ('image+facts', 3, 0.0, False, {'subset': 10}),
}
TIERS = {
    'core': ['C_if1', 'F_A', 'C_if1_free'],
    'full': ['C_if1', 'F_A', 'C_i1', 'C_f1', 'C_if3', 'C_if1_rep', 'C_if1_t07a', 'C_if1_t07b', 'C_if1_t07c', 'C_if1_free', 'C_if1_pin'],
    'taste': ['D_warm_film', 'D_clean_contrast', 'FB_warm_film', 'FB_clean_contrast', 'C_if1_pin'],
    'fivek': ['C_if1', 'F_A'],
    'fivek3': ['C_if3'],
    'cpu': ['cpu_if3'],
    'lat': ['lat_if3'],
}


def record(iid, r, P):
    rc = assist.check_rationale(r['reason_zh'], r['reasons'], P.m_auto)
    return dict(id=iid, edit=r['edit'], eff=r['eff'], m=r['m'], scale=r['scale'], reason_zh=r['reason_zh'], reasons=r['reasons'], rationale=rc,
                n_rounds=r['n_rounds'], rounds=[{k: v for k, v in rr.items() if k != 'edit'} | {'edit': rr['edit']} for rr in r['rounds']])


def cmd_vlm(model, tier, backend='cuda', threads=None):
    gen, five = sets()
    hist = histories() if tier == 'taste' else None
    configs = TIERS[tier]
    images = five if tier.startswith('fivek') else gen
    tag = (model if backend == 'cuda' else f'{model}@cpu{threads}').replace(':', '_')
    todo = []
    for c in configs:
        sub = CONFIGS.get(c, (None, None, None, None, {}))[4].get('subset')
        imgs = images[:: max(1, len(images) // sub)][:sub] if sub else images
        cname = c if not tier.startswith('fivek') else 'fk_' + c
        dn = done_ids(os.path.join(RES, tag, cname + '.jsonl'))
        todo += [(c, cname, p) for p in imgs if data.image_id(p) not in dn]
    if not todo: print('nothing to do', model, tier); return
    if model.startswith('cloud') and tier.startswith('fivek'): raise SystemExit('FiveK images are never sent to a cloud service')
    client = llm.Cloud(model.split(':', 1)[1] if ':' in model else None) if model.startswith('cloud') else llm.Server(model, backend, threads)
    meta = dict(model=tag, load_s=getattr(client, 'load_s', None), rss_mb_after_load=client.rss_mb())
    by_img = {}
    for c, cname, p in todo: by_img.setdefault(p, []).append((c, cname))
    try:
        for p, cl in by_img.items():
            iid = data.image_id(p)
            P = data.Prepared(p)
            for c, cname in cl:
                try:
                    if c in CONFIGS:
                        mode, rounds, temp, sem, ex = CONFIGS[c]
                        r = assist.run(client, P, mode, rounds, temp, seed=ex.get('seed', 7), pinned=ex.get('pinned', ()), semantic_only=sem,
                                       numbers_from=tools.empty_edit() if sem else None, constrained=ex.get('constrained', True))
                    else:
                        kind, who = c.split('_', 1)
                        f = P.features(); hw = hist_for(hist[who], iid); tt = taste.profile_text(hw, f)
                        if kind == 'D': r = assist.run(client, P, 'image+facts', 1, 0.0, taste_text=tt)
                        else: r = assist.run(client, P, 'image+facts', 1, 0.0, taste_text=tt, semantic_only=True, numbers_from=taste.knn_predict(hw, f)[0])
                    save(tag, cname, record(iid, r, P), r['img'])
                except Exception as e:
                    traceback.print_exc(); save(tag, cname, dict(id=iid, error=str(e)))
            P.close(); print(tag, iid, [x[1] for x in cl], flush=True)
        meta['rss_mb_end'] = client.rss_mb()
        try: meta['gpu_mem'] = subprocess.run(['nvidia-smi', '--query-compute-apps=pid,used_memory', '--format=csv,noheader'], capture_output=True, text=True).stdout
        except Exception: pass
    finally:
        client.close()
        with open(os.path.join(RES, f'meta_{tag}_{tier}.json'), 'w') as f: json.dump(meta, f, indent=1)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'prep': cmd_prep()
    elif a[0] == 'baseline': cmd_baseline()
    elif a[0] == 'vlm': cmd_vlm(a[1], a[2], a[3] if len(a) > 3 else 'cuda', int(a[4]) if len(a) > 4 else None)
