"""Per-file exposure anchor validation: tier / EV / confidence and the mean L* difference between the engine's auto render and the camera's embedded
JPEG, with the anchor off (profile baseline only), on, and on with the camera profile forced to 'generic'.
usage: python -I scripts/samples/anchor_table.py <dapple.exe> <work dir> <out.md> <file> [<file> ...]   (outputs stay under build/)
Public CC0 samples and the owner's ARWs are only read; nothing is copied into the repo."""
import sys, os, subprocess, re
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

def lstar_img(path, edge=400):
    im = Image.open(path).convert('RGB')
    im.thumbnail((edge, edge))
    a = np.asarray(im).astype(np.float64) / 255
    l = np.where(a <= .04045, a / 12.92, ((a + .055) / 1.055) ** 2.4)
    y = l @ np.array([.2126729, .7151522, .0721750])
    f = np.where(y > 216 / 24389, np.cbrt(y), (24389 / 27 * y + 16) / 116)
    return float((116 * f - 16).mean())

def run(exe, args, env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.run([exe] + args, capture_output=True, text=True, env=env)

def main():
    exe, work, outmd = sys.argv[1:4]
    files = sys.argv[4:]
    os.makedirs(work, exist_ok=True)
    def one(f):
        tag = re.sub(r'[^A-Za-z0-9_.-]', '_', f.replace('\\', '/').split('/')[-3] + '_' + os.path.basename(f))[:80]
        prev = os.path.join(work, tag + '_prev.jpg')
        run(exe, ['thumb', f, '-o', prev, '--long-edge', '400'])
        if not os.path.exists(prev):
            return None
        pl = lstar_img(prev)
        info = run(exe, ['info', '--anchor', f]).stdout
        m = re.search(r'tier (\S+) .*?, ([+-][\d.]+) EV, confidence ([\d.]+)', info)
        mk = re.search(r'camera\s+(\S+) (.*)', info)
        res = {}
        for name, env in (('off', {'DAPPLE_ANCHOR': 'off'}), ('on', {}), ('generic', {'DAPPLE_PROFILE': 'generic'})):
            out = os.path.join(work, '%s_%s.png' % (tag, name))
            if os.path.exists(out):
                os.remove(out)
            run(exe, ['develop', f, '-o', out, '--long-edge', '400'], env)
            res[name] = lstar_img(out) - pl if os.path.exists(out) else None
        return f, mk.group(1) if mk else '?', mk.group(2).strip() if mk else '', m.group(1) if m else '-', m.group(2) if m else '-', m.group(3) if m else '-', pl, res

    rows = []
    with ThreadPoolExecutor(2) as ex:
        for r in ex.map(one, files):
            if r:
                rows.append(r)
                print(r[0].split('/')[-1], r[3:6], {k: (round(v, 1) if v is not None else None) for k, v in r[7].items()}, flush=True)
    L = ['| file | camera | tier | EV | conf | JPEG mean L* | dL off | dL anchor | dL anchor, generic profile |', '|---|---|---|---|---|---|---|---|---|']
    mk = {}
    for f, make, model, tier, ev, conf, pl, res in rows:
        fmt = lambda v: '-' if v is None else '%+.1f' % v
        L.append('| %s | %s %s | %s | %s | %s | %.1f | %s | %s | %s |' % (os.path.basename(f), make, model, tier, ev, conf, pl, fmt(res['off']), fmt(res['on']), fmt(res['generic'])))
        if all(res[k] is not None for k in res):
            mk.setdefault(make, []).append(res)
    L += ['', '| maker | n | mean dL off | mean dL anchor | mean abs dL anchor | max abs dL anchor | mean dL anchor generic | max abs generic - anchor |', '|---|---|---|---|---|---|---|---|']
    for k, v in sorted(mk.items()):
        off = np.array([r['off'] for r in v]); on = np.array([r['on'] for r in v]); ge = np.array([r['generic'] for r in v])
        L.append('| %s | %d | %+.1f | %+.1f | %.1f | %.1f | %+.1f | %.2f |' % (k, len(v), off.mean(), on.mean(), np.abs(on).mean(), np.abs(on).max(), ge.mean(), np.abs(ge - on).max()))
    open(outmd, 'w').write('\n'.join(L))
    print('\n'.join(L[-(len(mk) + 3):]))

main()
