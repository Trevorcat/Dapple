#!/usr/bin/env python3
"""Regenerate app/assets/film/film_map.json (the "film map": richness x, contrast y per catalog stock).

Usage (Git Bash, from the repo root):
  python scripts/film/gen_film_map.py [--cli build/filmmap/bin/dapple.exe] [--stocks id,id] [--auto-exposure match|off]
                                    [--work build/filmmap] [--no-write] [--chart-tool dapple_base_previews_tests.exe] [--modern-only]
Method and limits: docs/film/film-map.md. Needs numpy + Pillow. Renders a synthetic 24-patch chart with `dapple film`
(default paper of the stock, spatial effects off, same input for every stock), measures per stock

  richness = mean over the 18 colour patches of C*ab(output) / C*ab(input)      (CIELAB, D65 white, sRGB values)
  contrast = least-squares slope of output L* vs input L* over the 4 mid greys (the 6 greys without white and black)

and maps both to [-1, 1] (robust, per axis): median -> 0, p5 -> -0.85, p95 -> +0.85, clamp.
With --stocks the table is measured for those stocks only (smoke test) and the asset is not written.

The modern renderings (modern_clean / modern_vivid / modern_soft, app/lib/model/bases.dart) are measured on the same chart with the same two
numbers and mapped with the SAME per-axis scale as the stocks (median / p5 / p95 of the stocks only, so adding them moves no stock): the engine's
reference render with `film` = the id at strength 100 and no auto correction, through `<chart-tool> chart in.pfm id out.png` (the `chart` mode of
tests/base_previews_tests.cpp, built as dapple_base_previews_tests: the CLI has no scene-referred chart input for the standard pipeline). They are
written with kind "modern" and measured true; when the tool is missing the existing asset entries of the modern ids are kept as they are.
--modern-only keeps every stock entry of the existing asset byte for byte and only replaces the modern ones.
"""
import argparse, json, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(os.path.join(HERE, '..', '..'))


def load_names():
    """(id, code) in the order of app/lib/model/film_names.dart."""
    src = open(os.path.join(R, 'app/lib/model/film_names.dart'), encoding='utf-8').read()
    body = src.split('kFilmDisplayNames = {')[1].split('};')[0]
    return re.findall(r"'([a-z0-9_]+)':\s*'([^']+)'", body)


def load_modern():
    """[(id, name)] of kModernBases in app/lib/model/bases.dart."""
    src = open(os.path.join(R, 'app/lib/model/bases.dart'), encoding='utf-8').read()
    return re.findall(r"ModernBase\('(modern_[a-z0-9_]+)',\s*'([^']+)'", src)


def info_of(sid):
    p = json.load(open(os.path.join(R, 'core/data/film/profiles', sid + '.json'), encoding='utf-8'))
    return p['info']


# 24-patch ColorChecker-like chart (sRGB 8 bit), as build/film/chart_exp.py
CC = [(115,82,68),(194,150,130),(98,122,157),(87,108,67),(133,128,177),(103,189,170),(214,126,44),(80,91,166),(193,90,99),
      (94,60,108),(157,188,64),(224,163,46),(56,61,150),(70,148,73),(175,54,60),(231,199,31),(187,86,149),(8,133,161),
      (243,243,242),(200,200,200),(160,160,160),(122,122,121),(85,85,85),(52,52,52)]
M_S2X = np.array([[0.4360747,0.3850649,0.1430804],[0.2225045,0.7168786,0.0606169],[0.0139322,0.0971045,0.7141733]])
M_PP2X = np.array([[0.7976749,0.1351917,0.0313534],[0.2880402,0.7118741,0.0000857],[0,0,0.8252100]])
M = np.linalg.inv(M_PP2X) @ M_S2X   # linear sRGB -> ProPhoto (the engine's working space)
S = 64


def lin(c):
    c = np.asarray(c, float) / 255.
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def rgb2lab(rgb01):  # gamma-encoded sRGB 0..1 -> CIELAB, D65
    l = lin(np.asarray(rgb01) * 255.)
    xyz = l @ np.array([[0.4124564,0.2126729,0.0193339],[0.3575761,0.7151522,0.1191920],[0.1804375,0.0721750,0.9503041]])
    xyz = xyz / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216/24389, np.cbrt(xyz), (24389/27 * xyz + 16) / 116)
    return np.stack([116*f[..., 1]-16, 500*(f[..., 0]-f[..., 1]), 200*(f[..., 1]-f[..., 2])], -1)


def write_chart(path):
    img = np.zeros((4*S, 6*S, 3), np.float32)
    for i, c in enumerate(CC):
        r, cx = divmod(i, 6)
        img[r*S:(r+1)*S, cx*S:(cx+1)*S] = (M @ lin(c)).astype(np.float32)
    with open(path, 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (6*S, 4*S)); f.write(img[::-1].astype('<f4').tobytes())


def patches(png):
    a = np.asarray(Image.open(png).convert('RGB')).astype(float)
    a = a / 65535 if a.max() > 255 else a / 255
    P = []
    for i in range(24):
        r, cx = divmod(i, 6)
        P.append(a[r*S+16:(r+1)*S-16, cx*S+16:(cx+1)*S-16].reshape(-1, 3).mean(0))
    return rgb2lab(np.array(P))


def hue(l): return np.degrees(np.arctan2(l[:, 2], l[:, 1]))


def measure(lab, lab_in):
    cin, cout = np.hypot(lab_in[:18, 1], lab_in[:18, 2]), np.hypot(lab[:18, 1], lab[:18, 2])
    rich = float(np.mean(cout / cin))
    slope = float(np.polyfit(lab_in[19:23, 0], lab[19:23, 0], 1)[0])           # mid greys 200,160,122,85
    toe = float((lab[22, 0]-lab[23, 0]) / (lab_in[22, 0]-lab_in[23, 0]))       # 52 -> 85
    sho = float((lab[18, 0]-lab[19, 0]) / (lab_in[18, 0]-lab_in[19, 0]))       # 200 -> 243
    s5 = float(np.polyfit(lab_in[19:24, 0], lab[19:24, 0], 1)[0])               # greys 200..52 (adds the shadow)
    m = cin > 15
    dh = (hue(lab[:18]) - hue(lab_in[:18]) + 180) % 360 - 180
    cast = float(np.mean(np.hypot(lab[19:23, 1], lab[19:23, 2])))
    return dict(richness=round(rich, 4), contrast=round(slope, 4), contrast_with_black=round(s5, 4), grey_span_200_52=round(float(lab[19, 0]-lab[23, 0]), 1), toe=round(toe, 4), shoulder=round(sho, 4),
                hue_shift_deg=round(float(np.mean(dh[m])), 2), hue_shift_abs_deg=round(float(np.mean(np.abs(dh[m]))), 2),
                neutral_cast=round(cast, 2), L_white=round(float(lab[18, 0]), 2), L_black=round(float(lab[23, 0]), 2),
                L_mid_grey=round(float(lab[21, 0]), 2))


def scale_with(v, med, lo, hi):
    """One value on the scale of scale(): the stocks' median -> 0, p5 -> -0.85, p95 -> +0.85 (not clamped)."""
    dn, up = max(med - lo, 1e-9), max(hi - med, 1e-9)
    return (v - med) / dn * 0.85 if v < med else (v - med) / up * 0.85


def scale(vals):
    v = np.array(vals); med, lo, hi = np.median(v), np.percentile(v, 5), np.percentile(v, 95)
    dn, up = max(med - lo, 1e-9), max(hi - med, 1e-9)       # each side scaled on its own
    return np.clip(np.where(v < med, (v - med) / dn * 0.85, (v - med) / up * 0.85), -1, 1), (med, lo, hi)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', default=os.path.join(R, 'build/filmmap/bin/dapple.exe'))
    ap.add_argument('--work', default=os.path.join(R, 'build/filmmap'))
    ap.add_argument('--stocks', default='')
    ap.add_argument('--auto-exposure', default='match')
    ap.add_argument('--no-write', action='store_true')
    ap.add_argument('--chart-tool', default='')
    ap.add_argument('--modern-only', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    pf = os.path.join(a.work, 'chart.pfm'); write_chart(pf)
    lab_in = rgb2lab(np.array(CC) / 255.)
    only = set(filter(None, a.stocks.split(',')))
    rows = {}
    for sid, code in load_names():
        inf = info_of(sid)
        if inf.get('stage') != 'filming' or (only and sid not in only):
            continue                                   # print papers / print films are not camera stocks
        out = os.path.join(a.work, 'chart_%s.png' % sid)
        cmd = [a.cli, 'film', '--in', pf, '--out', out, '--film', sid, '--paper', 'default', '--no-spatial',
               '--auto-exposure', a.auto_exposure]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit('render failed for %s: %s' % (sid, r.stderr))
        t = 'cine' if '_vision3_' in sid else ('rev' if inf['type'] == 'positive' else 'neg')
        d = measure(patches(out), lab_in)
        d.update(code=code, type=t, log=r.stdout.strip().split('\n')[-1][-140:])
        rows[sid] = d
        print('%-24s %-7s rich %.3f contrast %.3f' % (sid, code, d['richness'], d['contrast']), flush=True)
    xs, xst = scale([v['richness'] for v in rows.values()])
    ys, yst = scale([v['contrast'] for v in rows.values()])
    for (sid, v), x, y in zip(rows.items(), xs, ys):
        v['x'] = round(float(x), 3); v['y'] = round(float(y), 3)
    # the modern renderings: same chart, same measure, the stocks' scale (clamped to the map)
    old = {}
    asset = os.path.join(R, 'app/assets/film/film_map.json')
    if os.path.exists(asset):
        old = json.load(open(asset, encoding='utf-8'))
    tool = a.chart_tool
    if not tool:
        for c in ('build/swatch-desktop/bin/dapple_base_previews_tests.exe', 'build/check-desktop/bin/dapple_base_previews_tests.exe'):
            if os.path.exists(os.path.join(R, c)):
                tool = os.path.join(R, c)
                break
    modern = {}
    if not only:
        for mid, name in load_modern():
            code = old.get(mid, {}).get('code') or 'M-' + mid.split('_', 1)[1].upper()
            if not tool or not os.path.exists(tool):
                if mid in old:
                    modern[mid] = old[mid]
                    print('%-24s no chart tool: kept the existing entry' % mid)
                continue
            out = os.path.join(a.work, 'chart_%s.png' % mid)
            r = subprocess.run([tool, 'chart', pf, mid, out], capture_output=True, text=True)
            if r.returncode:
                sys.exit('chart render failed for %s: %s %s' % (mid, r.stdout, r.stderr))
            d = measure(patches(out), lab_in)
            xm = float(np.clip(scale_with(d['richness'], *xst), -1, 1)); ym = float(np.clip(scale_with(d['contrast'], *yst), -1, 1))
            d.update(code=code, type='modern', kind='modern', x_unclamped=round(float(scale_with(d['richness'], *xst)), 3),
                     y_unclamped=round(float(scale_with(d['contrast'], *yst)), 3), x=round(xm, 3), y=round(ym, 3))
            modern[mid] = d
            print('%-24s %-8s rich %.3f contrast %.3f -> x %+.3f y %+.3f (unclamped %+.3f %+.3f)' % (mid, code, d['richness'], d['contrast'], xm, ym, d['x_unclamped'], d['y_unclamped']), flush=True)
    raw = dict(method=dict(chroma='CIELAB C*ab D65 sRGB', auto_exposure=a.auto_exposure,
                           richness_med_p5_p95=[float(t) for t in xst], contrast_med_p5_p95=[float(t) for t in yst]),
               stocks=rows, modern=modern)
    json.dump(raw, open(os.path.join(a.work, 'measure.json'), 'w'), indent=2, ensure_ascii=False)
    if not a.no_write and not only:
        m = {sid: {'x': v['x'], 'y': v['y'], 'type': v['type'], 'code': v['code']} for sid, v in rows.items()}
        if a.modern_only:  # the stocks of the existing asset stay exactly as they are
            m = {sid: old[sid] for sid in rows if sid in old} or m
        for mid, v in modern.items():
            m[mid] = v if 'measured' in v else {'x': v['x'], 'y': v['y'], 'type': 'modern', 'kind': 'modern', 'code': v['code'], 'measured': True}
        outp = os.path.join(R, 'app/assets/film/film_map.json')
        with open(outp, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(m, f, indent=2, ensure_ascii=False); f.write('\n')
        print('wrote', outp)


if __name__ == '__main__':
    main()
