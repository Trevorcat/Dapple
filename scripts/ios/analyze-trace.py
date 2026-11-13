#!/usr/bin/env python3
"""Summarise frame-trace TSVs (dapple_frame_trace_dump) of the on-device autotest: frame-time histogram, gaps, look discontinuities.
usage: analyze-trace.py <dir> <device> <tag> [refresh_hz=60]"""
import sys, os, glob, json
d, dev, tag = sys.argv[1:4]
hz = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0
P = 1000.0 / hz
KIND = {-1: 'fail', 0: '-', 1: 'exact', 2: 'coarse', 3: 'stale', 4: 'fallbackStock', 5: 'interp', 6: 'lagging'}
MATCH = {0: '-', 1: 'hit', 2: 'interp', 3: 'stalePrev', 4: 'computed'}
def load(p):
    rows = [l.rstrip('\n').split('\t') for l in open(p)]
    h = rows[0]
    return [dict(zip(h, r)) for r in rows[1:]]
def f(r, k): return float(r[k])
def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))] if v else 0
out = {}
for p in sorted(glob.glob(f'{d}/{dev}_{tag}_*.tsv')):
    name = os.path.basename(p)[len(dev) + len(tag) + 2:-4]
    if name == 'warm': continue
    rows = load(p)
    ia = [r for r in rows if r['interactive'] == '1']
    ev = {}
    try:
        meta = json.load(open(p[:-4] + '.meta.txt'))
    except Exception:
        meta = {}
    rt = [f(r, 'renderMs') for r in ia]
    hist = {'<=8': 0, '8-17': 0, '17-33': 0, '33-67': 0, '67-134': 0, '>134': 0}
    for x in rt:
        k = '<=8' if x <= 8 else '8-17' if x <= 16.7 else '17-33' if x <= 33.4 else '33-67' if x <= 66.7 else '67-134' if x <= 133 else '>134'
        hist[k] += 1
    # gaps between consecutive interactive frames that started within the drag (render end of one -> render end of next = frame interval)
    gaps = []
    for a, b in zip(ia, ia[1:]):
        g = (f(b, 'tMs') + f(b, 'renderMs')) - (f(a, 'tMs') + f(a, 'renderMs'))
        if g > 2 * P:
            gaps.append((round(f(a, 'tMs')), round(g), b['stock'], 'render %.0f tables %.0f match %.0f' % (f(b, 'renderMs'), f(b, 'tablesMs'), f(b, 'matchMs')), 'bgBake %s bgMatch %s frameBake %s' % (b['bgBake'], b['bgMatch'], b.get('frameBake','n/a'))))
    lat = []
    try:
        inter = set(); fl = {}
        for l in open(p[:-4] + '.events.txt'):
            c = l.rstrip().split('	')
            if c[0] == 'S' and c[3] == '1': inter.add(c[2])
            if c[0] == 'F' and c[3] == '0': fl[c[2]] = float(c[8])
        lat = [v for k, v in fl.items() if k in inter]
    except Exception:
        pass
    wrong = [r for r in ia if r['printKind'] in ('3', '4', '6') or r['matchKind'] == '3']
    backs = 0; prev = None; kindchg = 0; lutchg = 0; tier = 0
    for r in ia:
        if prev:
            if r['stock'] == prev['stock'] and int(r['printGen']) < int(prev['printGen']): backs += 1
            if r['stock'] == prev['stock'] and r['printKind'] != prev['printKind']: kindchg += 1
            if r['lutN'] != prev['lutN']: lutchg += 1
            if r['longEdge'] != prev['longEdge']: tier += 1
        prev = r
    ooo = sum(1 for a, b in zip(rows, rows[1:]) if int(b['seq']) <= int(a['seq']))
    kinds = {}
    for r in ia: kinds[KIND[int(r['printKind'])]] = kinds.get(KIND[int(r['printKind'])], 0) + 1
    mk = {}
    for r in ia: mk[MATCH[int(r['matchKind'])]] = mk.get(MATCH[int(r['matchKind'])], 0) + 1
    gl = [f(r, 'tMs') for r in ia]
    out[name] = dict(latMed=round(pct(lat, .5), 1), latP90=round(pct(lat, .9), 1), latMax=round(max(lat or [0]), 1), latOver2P=sum(1 for v in lat if v > 2 * P), frames=len(ia), med=round(pct(rt, .5), 1), p90=round(pct(rt, .9), 1), max=round(max(rt or [0]), 1), hist=hist, gaps=len(gaps),
                     gapList=gaps[:12], wrongLook=len(wrong), genBack=backs, kindChanges=kindchg, lutNChanges=lutchg, tierChanges=tier,
                     outOfOrderRows=ooo, cbOutOfOrder=meta.get('outOfOrderCallbacks'), kinds=kinds, match=mk, rungEnd=meta.get('rungEnd'),
                     discontinuities=len(wrong) + backs + tier + lutchg)
for k, v in out.items():
    print(k); print('  ', json.dumps({a: b for a, b in v.items() if a != 'gapList'}))
    for g in v['gapList']: print('     gap', g)
