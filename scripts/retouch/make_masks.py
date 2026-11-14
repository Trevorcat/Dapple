"""Mask planes + face metadata for the CC0 portraits (retouch evidence). Mirrors core/src/ai_masks_build.cpp faceFeatures + person parts.
Run: F:/projects/sony-render/tools/py-ai/Scripts/python.exe -I F:/projects/sony-render/scripts/retouch/make_masks.py [name ...]
Writes build/retouch/masks/<name>/ (photo.png 2048, planes at long edge 1024, meta.json) and build/retouch/masks/<name>_check.jpg."""
import sys, os, math, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'ai-eval'))
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from ai_ref import lng, detect_faces, face_landmarks, segment_person_raw, iou
from common import ROOT

NAMES = ['p1_glasses_flag', 'p2_dark_skin_dreads', 'p3_curly_glasses_outdoor', 'p5_headscarf', 'p6_asian_woman_grey', 'p8_three_girls_busy_bg', 'p9_three_people_frontal']
PORT = os.path.join(ROOT, 'build', 'ai-eval', 'portraits')
OUT = os.path.join(ROOT, 'build', 'retouch', 'masks')
FRONTAL_YAW, MOUTH_OPEN = 28.0, 0.10
FACE_OVAL = [10, 109, 67, 103, 54, 21, 162, 127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454, 356, 389, 251, 284, 332, 297, 338]
LIPS_O = [0, 37, 39, 40, 185, 61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267]
LIPS_I = [13, 82, 81, 80, 191, 78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308, 415, 310, 311, 312]
EYE_L = [249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466, 263]
EYE_R = [7, 33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163]
IRIS_L, IRIS_R = [474, 475, 476, 477], [469, 470, 471, 472]
BROW = dict(RU=[70, 63, 105, 66, 107], RL=[46, 53, 52, 65, 55], LU=[300, 293, 334, 296, 336], LL=[276, 283, 282, 295, 285])
LID_R = [33, 7, 163, 144, 145, 153, 154, 155, 133]
LID_L = [263, 249, 390, 373, 374, 380, 381, 382, 362]
SS = 4


def poly_plane(w, h, rings, sigma):
    """even-odd polygon fill, 4x4 coverage, gaussian soft edge; float 0..1 (mirrors polygonPlane)"""
    out = np.zeros((h, w), np.float32)
    pts = np.concatenate([np.array(r, np.float32) for r in rings])
    pad = int(math.ceil(3 * sigma)) + 2
    x0, y0 = max(0, int(pts[:, 0].min()) - pad), max(0, int(pts[:, 1].min()) - pad)
    x1, y1 = min(w, int(math.ceil(pts[:, 0].max())) + pad), min(h, int(math.ceil(pts[:, 1].max())) + pad)
    if x1 <= x0 or y1 <= y0: return out
    bw, bh = x1 - x0, y1 - y0
    acc = np.zeros((bh * SS, bw * SS), bool)
    for r in rings:
        im = Image.new('1', (bw * SS, bh * SS), 0)
        ImageDraw.Draw(im).polygon([((x - x0) * SS, (y - y0) * SS) for x, y in r], fill=1)
        acc ^= np.asarray(im, bool)
    cov = acc.reshape(bh, SS, bw, SS).mean((1, 3)).astype(np.float32)
    if sigma >= 0.3: cov = ndimage.gaussian_filter(cov, sigma, mode='constant', truncate=3.0)
    out[y0:y1, x0:x1] = cov
    return out


def q8(a): return np.clip(np.rint(a * 255), 0, 255).astype(np.uint8)
def p8(a): return np.round(a * 255) / 255      # the C++ planes are 8-bit between operations
def save(d, name, a): Image.fromarray(q8(a), 'L').save(os.path.join(d, name))


def grow(a, r):
    """dilate a soft plane by r pixels (distance transform), re-softened a little"""
    dist = ndimage.distance_transform_edt(a <= 0.5)
    g = (dist <= r).astype(np.float32)
    return np.maximum(a, ndimage.gaussian_filter(g, 0.6))


def iris_ring(xy, idx):
    c = xy[idx].mean(0); r = np.mean(np.hypot(*(xy[idx] - c).T))
    return [(c[0] + r * math.cos(2 * math.pi * k / 28), c[1] + r * math.sin(2 * math.pi * k / 28)) for k in range(28)]


def brow_rings(xy, grow_px):
    rings = []
    for u, l in (('RU', 'RL'), ('LU', 'LL')):
        r = [tuple(xy[i]) for i in BROW[u]] + [tuple(xy[i]) for i in BROW[l][::-1]]
        c = np.mean(r, 0); n = len(r); o = []
        for i in range(n):
            a0, a1 = np.array(r[(i - 1) % n]), np.array(r[(i + 1) % n])
            t = a1 - a0; t /= max(1e-3, np.hypot(*t)); nn = np.array([-t[1], t[0]])
            if nn @ (np.array(r[i]) - c) < 0: nn = -nn
            o.append(tuple(np.array(r[i]) + nn * grow_px))
        rings.append(o)
    return rings


# deepest point of the crescent at t = 0.6. The brief's t' = t^0.85 would peak at t = 0.44 (toward the OUTER corner); this exponent puts it at 0.6.
PEAK_EXP = math.log(0.5) / math.log(0.6)


def under_eye(w, h, xy, iod, eyes):
    down = np.array([xy[473][1] - xy[468][1], -(xy[473][0] - xy[468][0])])
    down /= np.hypot(*down)
    mouth = (xy[13] + xy[14]) / 2; eyec = (xy[468] + xy[473]) / 2
    if down @ (mouth - eyec) < 0: down = -down
    rings = []
    for lid in (LID_R, LID_L):
        P = xy[lid].astype(np.float64)
        seg = np.hypot(*np.diff(P, axis=0).T); cum = np.concatenate([[0], np.cumsum(seg)]) / seg.sum()
        ts = np.linspace(0, 1, 24)
        pts = np.stack([np.interp(ts, cum, P[:, 0]), np.interp(ts, cum, P[:, 1])], 1)
        s = np.sin(np.pi * ts ** PEAK_EXP) ** 0.7
        up = pts + down * 0.035 * iod
        lo = pts + down * ((0.035 + 0.24 * s)[:, None] * iod)
        rings.append([tuple(p) for p in up] + [tuple(p) for p in lo[::-1]])
    a = poly_plane(w, h, rings, 0.035 * iod)
    return a * (1 - eyes)


def tiled_faces(im, tile=320, stride=160):
    """the BlazeFace short-range detector misses faces under ~8 % of the frame: also run it on overlapping tiles (boxes mapped back, deduplicated)"""
    w, h = im.size; out = []
    for y0 in range(0, max(1, h - tile + stride), stride):
        for x0 in range(0, max(1, w - tile + stride), stride):
            x1, y1 = min(w, x0 + tile), min(h, y0 + tile); x0b, y0b = max(0, x1 - tile), max(0, y1 - tile)
            for f in detect_faces(im.crop((x0b, y0b, x1, y1))):
                f['box'][0] += x0b; f['box'][1] += y0b
                for k in f['kp']: k[0] += x0b; k[1] += y0b
                out.append(f)
    return out


def all_faces(im):
    faces = detect_faces(im)
    for f in sorted(tiled_faces(im), key=lambda f: -f['score']):
        if all(iou(f['box'], g['box']) < 0.2 for g in faces): faces.append(f)
    res = []
    for f in faces:
        if f['box'][2] < 120:        # small face: landmarks on a 2x upscaled copy, mapped back
            im2 = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)
            f2 = dict(f, box=[v * 2 for v in f['box']], kp=[[a * 2, b * 2] for a, b in f['kp']])
            m = face_landmarks(im2, f2); m['xy'] = m['xy'] / 2; m['z'] = m['z'] / 2
        else: m = face_landmarks(im, f)
        res.append((f, m))
    return res


def process(name):
    src = os.path.join(PORT, name + '.jpg'); full = Image.open(src).convert('RGB'); W0, H0 = full.size
    d = os.path.join(OUT, name); os.makedirs(d, exist_ok=True)
    s = 2048 / max(W0, H0); full.resize((round(W0 * s), round(H0 * s)), Image.LANCZOS).save(os.path.join(d, 'photo.png'))
    im = lng(full, 1024); w, h = im.size; rgb = np.asarray(im, np.uint8); L = max(w, h)
    prob = segment_person_raw(im)                                  # 256x256x6
    up = lambda a: np.asarray(Image.fromarray(a.astype(np.float32), 'F').resize((w, h), Image.BILINEAR), np.float32).clip(0, 1)
    fskin = up(prob[..., 3]); person = 1 - up(prob[..., 0])
    save(d, 'faceskin.png', fskin); save(d, 'person.png', person)
    faces = all_faces(im)
    rej = sum(1 for _, m in faces if not m['valid']); good = []
    for f, m in faces:
        if not m['valid'] or m['xy'].shape[0] < 478: continue
        xy = m['xy'].astype(np.float64)
        good.append((float(np.hypot(*(xy[454] - xy[234]))), xy, m['z']))
    good.sort(key=lambda g: -g[0]); good = good[:4]
    meta = dict(name=name, src=src.replace('\\', '/'), w=W0, h=H0, plane_w=w, plane_h=h, detected=len(faces), rejected=rej, faces=[])
    tint = rgb.astype(np.float32).copy()
    layers = []
    for n, (fw, xy, z) in enumerate(good, 1):
        dxy = xy[454] - xy[234]; dist3 = math.sqrt(dxy[0] ** 2 + dxy[1] ** 2 + float(z[454] - z[234]) ** 2)
        yaw = math.degrees(math.asin(max(-1, min(1, float(z[454] - z[234]) / max(dist3, 1e-6)))))
        frontal = abs(yaw) < FRONTAL_YAW
        face = p8(poly_plane(w, h, [[tuple(p) for p in xy[FACE_OVAL]]], max(0.8, 0.012 * fw)))
        gap = np.hypot(*(xy[13] - xy[14])); mw = max(1e-3, np.hypot(*(xy[78] - xy[308]))); mopen = float(gap / mw)
        iod = float(np.hypot(*(xy[473] - xy[468])))
        info = dict(index=n, iod=iod / L, eyeR=[xy[468][0] / w, xy[468][1] / h], eyeL=[xy[473][0] / w, xy[473][1] / h], faceW=fw / L, yaw=yaw, frontal=bool(frontal),
                    mouth_open=mopen, teeth=False, note='eyeR = person right eye (mesh 468, image left); eyeL = person left eye (473, image right)')
        save(d, f'f{n}_face.png', face)
        if frontal:
            sE, sM = max(0.6, 0.006 * fw), max(0.6, 0.008 * fw)
            eyes = p8(poly_plane(w, h, [[tuple(p) for p in xy[EYE_R]], [tuple(p) for p in xy[EYE_L]]], sE))
            iris = p8(poly_plane(w, h, [iris_ring(xy, IRIS_R), iris_ring(xy, IRIS_L)], sE)) * eyes
            sclera = eyes * (1 - p8(iris))
            lips = p8(poly_plane(w, h, [[tuple(p) for p in xy[LIPS_O]], [tuple(p) for p in xy[LIPS_I]]], sM))
            brows = p8(poly_plane(w, h, brow_rings(xy, 0.02 * fw), max(0.6, 0.008 * fw)))
            ue = under_eye(w, h, xy, iod, eyes)
            teeth = None
            if mopen >= MOUTH_OPEN:
                inner = p8(poly_plane(w, h, [[tuple(p) for p in xy[LIPS_I]]], max(0.5, 0.004 * fw)))
                at = inner > 127.5 / 255
                if at.sum() >= 12:
                    px = rgb[at].astype(np.float32); Y = px @ np.array([0.2126, 0.7152, 0.0722], np.float32)
                    yhi = np.sort(Y)[len(Y) * 9 // 10]
                    if yhi >= 90:
                        def sst(a, b, x):
                            t = np.clip((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)
                        mx, mn = px.max(1), px.min(1); sat = (mx - mn) / np.maximum(1, mx)
                        v = sst(0.55 * yhi, 0.8 * yhi, Y) * (1 - sst(0.30, 0.55, sat))
                        t = np.zeros_like(inner); t[at] = inner[at] * v
                        if (v > 0.5).sum() >= 0.08 * at.sum(): teeth = t
            if teeth is not None:
                info['teeth'] = True; save(d, f'f{n}_teeth.png', teeth)
            r = 0.01 * fw
            mouth = p8(poly_plane(w, h, [[tuple(p) for p in xy[LIPS_I]]], max(0.5, 0.004 * fw)))  # the open mouth (teeth, tongue) is not skin
            if mopen >= MOUTH_OPEN: save(d, f'f{n}_mouth.png', poly_plane(w, h, [[tuple(p) for p in xy[LIPS_I]]], sM))  # app/engine 'mouth' plane (sigma 0.008 x face width)
            excl = 1 - (1 - grow(eyes, r)) * (1 - grow(brows, r)) * (1 - grow(lips, r)) * (1 - grow(mouth, r))
            skin = fskin * face * (1 - excl)
            for nm, a in (('eyes', eyes), ('iris', iris), ('sclera', sclera), ('lips', lips), ('brows', brows), ('undereye', ue), ('skin', skin)): save(d, f'f{n}_{nm}.png', a)
            layers += [(skin, (255, 0, 0), 0.45), (ue, (0, 80, 255), 0.8), (sclera, (0, 255, 0), 0.7), (iris, (255, 255, 0), 0.7), (lips, (255, 0, 255), 0.6), (brows, (0, 255, 255), 0.6)]
            if teeth is not None: layers.append((teeth, (255, 140, 0), 0.8))
        meta['faces'].append(info)
    for a, col, k in layers:
        for c in range(3): tint[..., c] = tint[..., c] * (1 - k * a) + col[c] * k * a
    Image.fromarray(np.clip(tint, 0, 255).astype(np.uint8)).save(os.path.join(OUT, name + '_check.jpg'), quality=90)
    json.dump(meta, open(os.path.join(d, 'meta.json'), 'w'), indent=1)
    print(name, 'detected', len(faces), 'rejected', rej, 'written', [(f['index'], round(f['iod'], 4), round(f['yaw'], 1), f['frontal'], round(f['mouth_open'], 2), f['teeth']) for f in meta['faces']])


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for n in (sys.argv[1:] or NAMES): process(n)
