"""Fit the exposure (EV) at which the engine's render has the same mean L* as the camera's embedded JPEG, per RAW sample.
usage: python -I scripts/samples/fit_baseline.py <dapple.exe> <rawsamples dir> <sweep out dir with *_prev.jpg> <work dir> [evs=0,1,2] [--auto]
Renders each file with --no-auto (unless --auto) at the given exposures (long edge 400), reports mean L* of engine and preview and the
interpolated EV that matches the preview. Output: one line per file + per-maker medians. Public samples only (CC0), outputs under build/."""
import sys, os, subprocess, json, glob, math
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

def lstar(path, edge=400):
    im = Image.open(path).convert('RGB')
    im.thumbnail((edge, edge))
    a = np.asarray(im).astype(np.float64) / 255
    l = np.where(a <= .04045, a / 12.92, ((a + .055) / 1.055) ** 2.4)
    y = l @ np.array([.2126729, .7151522, .0721750])
    f = np.where(y > 216 / 24389, np.cbrt(y), (24389 / 27 * y + 16) / 116)
    return float((116 * f - 16).mean())

def main():
    exe, root, prevdir, work = sys.argv[1:5]
    evs = [float(x) for x in (sys.argv[5] if len(sys.argv) > 5 and not sys.argv[5].startswith('--') else '0,1,2').split(',')]
    auto = '--auto' in sys.argv
    os.makedirs(work, exist_ok=True)
    files = [f for f in glob.glob(os.path.join(root, '*', '*', '*')) if os.path.isfile(f) and not f.endswith(('.part', '.md', '.tsv'))]
    files.sort()
    def one(f):
        rel = os.path.relpath(f, root).replace('\\', '/')
        tag = rel.replace('/', '__').rsplit('.', 1)[0]
        prev = os.path.join(prevdir, tag + '_prev.jpg')
        if not os.path.exists(prev):
            return rel, None, None
        res = []
        for ev in evs:
            pj = os.path.join(work, 'p_%s.json' % str(ev).replace('.', '_').replace('-', 'm'))
            out = os.path.join(work, '%s_%s.png' % (tag, str(ev).replace('.', '_').replace('-', 'm')))
            cmd = [exe, 'develop', f, '-o', out, '--long-edge', '400', '--params', json.dumps({'exposure': ev * 20}), '--auto' if auto else '--no-auto']
            subprocess.run(cmd, capture_output=True)
            res.append(lstar(out) if os.path.exists(out) else None)
        return rel, lstar(prev), res
    for ev in evs:
        json.dump({'exposure': ev}, open(os.path.join(work, 'p_%s.json' % str(ev).replace('.', '_').replace('-', 'm')), 'w'))
    rows = []
    with ThreadPoolExecutor(2) as ex:
        for rel, p, res in ex.map(one, files):
            if p is None or any(r is None for r in res):
                print('skip', rel); continue
            # EV where the engine L* reaches the preview L* (piecewise linear, extrapolated)
            xs, ys = np.array(evs), np.array(res)
            ev_fit = float(np.interp(p, ys, xs)) if ys[0] <= p <= ys[-1] else float(xs[0] + (p - ys[0]) * (xs[-1] - xs[0]) / (ys[-1] - ys[0]))
            rows.append((rel, p, res, ev_fit))
            print('%-70s prev %.1f engine %s  ev_fit %+.2f' % (rel, p, ' '.join('%.1f' % r for r in res), ev_fit), flush=True)
    by = {}
    for rel, p, res, e in rows:
        by.setdefault(rel.split('/')[0], []).append(e)
    print('--- per maker median ev_fit')
    for k, v in sorted(by.items()):
        print('%-10s n=%d median %+.2f min %+.2f max %+.2f' % (k, len(v), float(np.median(v)), min(v), max(v)))

main()
