"""Runs the benchmark binaries on the Redmi (adb serial 268a8ef9, /data/local/tmp/dapple-ai only), pulls outputs, compares with the desktop
reference written by phone_prep.py.  Usage:
  phone_run.py push                       # binaries (tools/ai-rt/bin), libs, models, inputs
  phone_run.py run <runtime> <case...> [--threads 1,2,4] [--mode default|gpu|xnn|...] [--runs N] [--tag T]
  phone_run.py clean                      # rm -rf /data/local/tmp/dapple-ai
Results append to build/ai-eval/phone/results.jsonl."""
import sys, os, json, subprocess, argparse; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from common import ROOT, MD, OUT
SER = '268a8ef9'; DEV = '/data/local/tmp/dapple-ai'; P = os.path.join(OUT, 'phone'); BIN = os.path.join(ROOT, 'tools', 'ai-rt', 'bin')
ADB = os.path.join(ROOT, 'tools', 'android-sdk', 'platform-tools', 'adb.exe')
def adb(*a, check=True):
    r = subprocess.run([ADB, '-s', SER, *a], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and r.returncode: raise RuntimeError(' '.join(a) + '\n' + r.stdout + r.stderr)
    return r.stdout + r.stderr
def push_if_new(src, dst):
    sz = os.path.getsize(src); have = adb('shell', f'stat -c %s {dst} 2>/dev/null', check=False).strip()
    if have != str(sz): adb('push', src, dst)
def push(cases):
    adb('shell', f'mkdir -p {DEV}/bin {DEV}/models {DEV}/in {DEV}/out')
    for d in os.listdir(os.path.join(ROOT, 'tools', 'ai-rt')):
        if d == 'bin' or d.startswith('bin-'):
            v = d if d != 'bin' else 'bin-litert'; adb('shell', f'mkdir -p {DEV}/{v}')
            for f in os.listdir(os.path.join(ROOT, 'tools', 'ai-rt', d)): push_if_new(os.path.join(ROOT, 'tools', 'ai-rt', d, f), f'{DEV}/{v}/{f}')
            adb('shell', f'chmod 755 {DEV}/{v}/*_bench')
    for k, c in cases.items():
        mp = c.get('devmodel') or os.path.join(MD, c['path']); push_if_new(mp, f"{DEV}/models/{os.path.basename(mp)}")
        for i in range(len(c['ins'])): push_if_new(f"{P}/in/{k}.{i}.bin", f"{DEV}/in/{k}.{i}.bin")
def cmp(case, c, tag):
    res = []
    for i, (dt, shp) in enumerate(c['outs']):
        loc = f"{P}/out/{tag}_{case}_{i}.bin"
        if not os.path.exists(loc): res.append(None); continue
        a = np.fromfile(loc, dt); b = np.fromfile(f"{P}/ref/{case}.{i}.bin", dt)
        if a.size != b.size: res.append(dict(err=f'size {a.size} vs {b.size}')); continue
        a = a.astype(np.float64); b = b.astype(np.float64); rng = float(b.max() - b.min()) or 1.0
        res.append(dict(maxabs=float(np.abs(a - b).max()), rel=float(np.abs(a - b).max() / rng), rng=rng))
    return res
if __name__ == '__main__':
    cases = json.load(open(P + '/cases.json'))
    ORTM = os.path.join(ROOT, 'tools', 'ai-rt', 'ort-models', 'onnx')
    ap = argparse.ArgumentParser(); ap.add_argument('cmd'); ap.add_argument('rest', nargs='*'); ap.add_argument('--threads', default='1,2,4'); ap.add_argument('--mode', default='default')
    ap.add_argument('--runs', type=int, default=10); ap.add_argument('--tag', default=''); ap.add_argument('--cpus', default='4,5,6,7'); ap.add_argument('--extra', default=''); ap.add_argument('--ort', action='store_true', help='use the .ort twin from tools/ai-rt/ort-models/onnx')
    a = ap.parse_args()
    if a.cmd == 'clean': print(adb('shell', f'rm -rf {DEV}; ls /data/local/tmp | head -30')); sys.exit()
    if a.cmd == 'push' and a.ort:
        for k, v in cases.items():
            if v['rt'] == 'onnx': v['devmodel'] = os.path.join(ORTM, os.path.basename(v['path']).replace('.onnx', '.ort'))
    if a.cmd == 'push': push({k: v for k, v in cases.items() if not a.rest or v['model'] in a.rest or k in a.rest}); sys.exit()
    rt, names = a.rest[0], a.rest[1:]
    os.makedirs(P + '/out', exist_ok=True)
    for name in names:
        for k, c in cases.items():
            if k != name and c['model'] != name: continue
            if a.ort and c['rt'] == 'onnx': c['devmodel'] = os.path.join(ORTM, os.path.basename(c['path']).replace('.onnx', '.ort'))
            for t in [int(x) for x in a.threads.split(',')]:
                tag = f"{rt}_{a.mode}_{a.tag}t{t}"; dm = c.get('devmodel') or c['path']; mp = f"{DEV}/models/{os.path.basename(dm)}"
                bd = f'{DEV}/bin-{rt}'; exe = rt.split('-')[0]
                cmd = f"cd {DEV} && LD_LIBRARY_PATH={bd} {bd}/{exe}_bench --model {mp} --threads {t} --runs {a.runs} --cpus {a.cpus} --mode {a.mode} --out {DEV}/out/{tag}_{k} {a.extra}"
                for i in range(len(c['ins'])): cmd += f" --in {DEV}/in/{k}.{i}.bin"
                if exe == 'ort':
                    for i, (dt, shp) in enumerate(c['ins']): cmd += f" --shape {i}=" + 'x'.join(map(str, shp))
                out = adb('shell', cmd, check=False).strip().splitlines()
                line = next((l for l in out if '{"' in l), '')
                try: r = json.loads(line[line.index('{'):])
                except Exception: print(k, t, 'FAILED:', ' | '.join(out)[-400:]); continue
                for i in range(len(c['outs'])): adb('pull', f"{DEV}/out/{tag}_{k}_{i}.bin", f"{P}/out/{tag}_{k}_{i}.bin", check=False)
                r.update(case=k, model=c['model'], tag=tag, diff=cmp(k, c, tag)); r.pop('io', None) if False else None
                open(P + '/results.jsonl', 'a').write(json.dumps(r) + '\n')
                d = r['diff'][0] if r['diff'] else None
                if 'error' in r: print(k, t, 'ERROR', r['error'])
                print(f"{k:42s} t{t} load {r.get('load_ms')} first {r.get('first_ms')} med {r.get('median_ms')} rss_d {r.get('rss_delta_mb')} peak {r.get('peak_rss_mb')} diff0 {d}")
