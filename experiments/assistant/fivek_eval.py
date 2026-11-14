"""FiveK metric (i): distance of each arm's rendering to the expert renditions (sRGB thumbnails of experts A-E, 100 px long edge),
in CIE Lab: mean dE2000 and mean |dL*| after resampling our rendering to the thumbnail's size. What it measures: 'expert-like global tone
and colour' at thumbnail scale; it cannot see local contrast, sharpness, noise, or whether a different-but-good rendering is good.
Also an ORACLE arm: the best pad offsets (light x/y, colour x/y; base none) for expert C found by coordinate search: the room for
improvement that exists inside the tool surface at all.
  python -I experiments/assistant/fivek_eval.py oracle | score"""
import glob, json, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data, tools, vision

OUT = data.OUT
FK = os.path.join(OUT, 'fivek')


def expert(name, x):
    p = os.path.join(FK, 'thmb_' + x, name + '.jpg')
    return np.asarray(Image.open(p).convert('RGB')) if os.path.exists(p) else None


def to_thumb(img, ref):
    """Resample img (HxWx3 uint8) to ref's size; None when the aspect differs by > 4 % (expert crop) even after a 90 deg turn."""
    h, w = ref.shape[:2]; H, W = img.shape[:2]
    if abs(W / H - w / h) / (w / h) > 0.04:
        return None
    return np.asarray(Image.fromarray(img).resize((w, h), Image.LANCZOS))


def dist(img, ref):
    t = to_thumb(img, ref)
    if t is None: return None
    a, b = vision.srgb8_to_lab(t), vision.srgb8_to_lab(ref)
    return float(vision.de2000(a, b).mean()), float(np.abs(a[..., 0] - b[..., 0]).mean())


def fk_name(iid): return iid[3:-4]   # fk_<name>.dng


def cmd_oracle():
    res_p = os.path.join(OUT, 'results', 'engine', 'O_expC.jsonl'); os.makedirs(os.path.dirname(res_p), exist_ok=True)
    done = {json.loads(l)['id'] for l in open(res_p)} if os.path.exists(res_p) else set()
    for path in data.image_sets()['fivek']:
        iid = data.image_id(path)
        if iid in done: continue
        ref = expert(fk_name(iid), 'c')
        try: P = data.Prepared(path)
        except Exception as ex:
            print('skip', iid, ex); continue
        e = tools.empty_edit()
        def score(ed):
            img = P.render(ed, edge=256)[0]; d = dist(img, ref); return d[0] if d else None
        best = score(e)
        if best is None: P.close(); open(res_p, 'a').write(json.dumps(dict(id=iid, skipped='aspect')) + '\n'); continue
        start = best
        for _ in range(2):
            for cat, ax in (('light', 'x'), ('light', 'y'), ('color', 'x'), ('color', 'y'), ('light', 's')):
                lim = tools.MAXS if ax == 's' else tools.MAXSTEP
                for v in range(-lim, lim + 1):
                    if v == e[cat][ax]: continue
                    f = json.loads(json.dumps(e)); f[cat][ax] = v
                    s = score(f)
                    if s is not None and s < best - 1e-4: best, e = s, f
        img = P.render(e)[0]
        p = os.path.join(OUT, 'renders', 'O_expC', 'engine'); os.makedirs(p, exist_ok=True)
        Image.fromarray(img).save(os.path.join(p, iid + '.jpg'), quality=90)
        open(res_p, 'a').write(json.dumps(dict(id=iid, edit=e, dE_auto=start, dE_oracle=best)) + '\n')
        P.close(); print('oracle', iid, f'{start:.2f} -> {best:.2f}', tools.describe(e), flush=True)


def score_dir(d):
    """Mean distances of every rendering in renders/<config>/<model>/ to experts A-E."""
    rows = {}
    for p in glob.glob(os.path.join(d, 'fk_*.jpg')):
        iid = os.path.basename(p)[:-4]; img = np.asarray(Image.open(p).convert('RGB'))
        r = {}
        for x in 'abcde':
            ref = expert(fk_name(iid), x)
            if ref is None: continue
            dd = dist(img, ref)
            if dd: r[x] = dd
        if r: rows[iid] = r
    return rows


def expert_spread():
    """Inter-expert distance (A, B, D, E vs C): how far apart five trained humans are: the noise floor of this metric."""
    out = []
    for p in glob.glob(os.path.join(FK, 'thmb_c', '*.jpg')):
        n = os.path.basename(p)[:-4]; c = expert(n, 'c')
        for x in 'abde':
            o = expert(n, x)
            if o is not None and o.shape == c.shape:
                a, b = vision.srgb8_to_lab(o), vision.srgb8_to_lab(c); out.append(float(vision.de2000(a, b).mean()))
    return float(np.mean(out)) if out else None


def cmd_score():
    res = {}
    for d in sorted(glob.glob(os.path.join(OUT, 'renders', '*', '*'))):
        cfg, model = d.replace('\\', '/').split('/')[-2:]
        rows = score_dir(d)
        if rows: res[f'{cfg}|{model}'] = rows
    res['_expert_spread_vs_C'] = expert_spread()
    json.dump(res, open(os.path.join(OUT, 'fivek_scores.json'), 'w'), indent=0)
    print('scored', len(res))


if __name__ == '__main__':
    {'oracle': cmd_oracle, 'score': cmd_score}[sys.argv[1]]()
