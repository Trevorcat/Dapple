"""Colour maths, desktop AI masks (Python reference of core/ai: scripts/ai-eval/ai_ref.py) and the measured-facts extractor.
On desktop the engine DLL has DAPPLE_AI off, so the planes are computed here from the neutral standard rendering (the same input the
engine service uses, pipeline-architecture 41.1) and handed to the engine through dapple_mask_raster_set.
Sky is a classical prior (top-connected, smooth, blue or bright-neutral, far in depth): the engine's own sky prior is in ai_masks_sky.cpp
and not exported by the DLL; this is a stand-in, so 'sky' results are rough on purpose."""
import os, sys, json, hashlib, datetime
import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'scripts', 'ai-eval'))

# ------------------------------------------------------------------------------------------------ colour

def srgb_to_lin(a):
    a = np.asarray(a, np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)

M_SRGB_XYZ = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]], np.float32)
WP_D65 = np.array([0.95047, 1.0, 1.08883], np.float32)
WP_D50 = np.array([0.96422, 1.0, 0.82521], np.float32)
BRADFORD_D65_D50 = np.array([[1.0478112, 0.0228866, -0.0501270], [0.0295424, 0.9904844, -0.0170491], [-0.0092345, 0.0150436, 0.7521316]], np.float32)


def xyz_to_lab(xyz, wp):
    t = xyz / wp
    f = np.where(t > (6 / 29) ** 3, np.cbrt(t), t / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def srgb8_to_lab(img, d50=False):
    xyz = srgb_to_lin(img) @ M_SRGB_XYZ.T
    if d50: return xyz_to_lab(xyz @ BRADFORD_D65_D50.T, WP_D50)
    return xyz_to_lab(xyz, WP_D65)


def de2000(lab1, lab2):
    L1, a1, b1 = lab1[..., 0], lab1[..., 1], lab1[..., 2]
    L2, a2, b2 = lab2[..., 0], lab2[..., 1], lab2[..., 2]
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2); Cb = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cb ** 7 / (Cb ** 7 + 25 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p, h2p = np.degrees(np.arctan2(b1, a1p)) % 360, np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p; dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh)); dh = np.where(C1p * C2p == 0, 0, dh)
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lbp, Cbp = (L1 + L2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hbp = np.where(C1p * C2p == 0, hs, np.where(np.abs(h1p - h2p) > 180, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2), hs / 2))
    T = 1 - 0.17 * np.cos(np.radians(hbp - 30)) + 0.24 * np.cos(np.radians(2 * hbp)) + 0.32 * np.cos(np.radians(3 * hbp + 6)) - 0.20 * np.cos(np.radians(4 * hbp - 63))
    dth = 30 * np.exp(-((hbp - 275) / 25) ** 2)
    Rc = 2 * np.sqrt(Cbp ** 7 / (Cbp ** 7 + 25 ** 7))
    Sl = 1 + 0.015 * (Lbp - 50) ** 2 / np.sqrt(20 + (Lbp - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cbp, 1 + 0.015 * Cbp * T
    Rt = -np.sin(np.radians(2 * dth)) * Rc
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))

# ------------------------------------------------------------------------------------------------ masks (desktop stand-in for the engine AI service)

_ai = None


def ai():
    global _ai
    if _ai is None:
        import ai_ref
        _ai = ai_ref
    return _ai


def compute_masks(neutral, cache_dir):
    """neutral: HxWx3 uint8 standard rendering (geometry orig, <= 1024 px). Returns dict of uint8 planes (HxW) + face list. Cached."""
    os.makedirs(cache_dir, exist_ok=True)
    cp = os.path.join(cache_dir, 'masks.npz'); fp = os.path.join(cache_dir, 'faces.json')
    if os.path.exists(cp) and os.path.exists(fp):
        z = np.load(cp); return {k: z[k] for k in z.files}, json.load(open(fp))
    H, W = neutral.shape[:2]
    im = Image.fromarray(neutral)
    A = ai()
    faces = [f for f in A.detect_faces(im, 0.6) if f['box'][2] * f['box'][3] > 0.0008 * W * H]
    faces = sorted(faces, key=lambda f: -f['box'][2] * f['box'][3])[:4]
    prob, cls, trusted, reason, gate = A.segment_person(im, len(faces))
    cls_full = np.asarray(Image.fromarray(cls).resize((W, H), Image.NEAREST))
    planes = {}
    up = lambda p: np.asarray(Image.fromarray((np.clip(p, 0, 1) * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR))
    if trusted:
        planes['person'] = up(prob[..., 1:5].sum(-1))
        planes['faceskin'] = up(prob[..., 3])
        planes['background'] = 255 - planes['person']
    try:
        d = A.depth(im); d = (d - d.min()) / max(1e-6, d.max() - d.min())   # relative inverse depth: 1 = near
        planes['depth'] = np.asarray(Image.fromarray(((1 - d) * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR))  # engine convention 0 near .. 255 far
    except Exception as e:  # depth model missing
        print('depth unavailable', e)
    planes["sky"] = sky_prior(neutral, planes.get("depth"), planes.get("person"))
    np.savez_compressed(cp, **planes)
    fl = [dict(score=f['score'], box=[float(v) for v in f['box']]) for f in faces]
    json.dump(dict(faces=fl, person_trusted=bool(trusted), gate=reason, size=[W, H]), open(fp, 'w'))
    return planes, json.load(open(fp))


def sky_prior(rgb, depth=None, person=None):
    lab = srgb8_to_lab(rgb); L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]; C = np.hypot(a, b)
    H, W = L.shape
    g = np.hypot(ndimage.sobel(ndimage.gaussian_filter(L, 1.5), 0), ndimage.sobel(ndimage.gaussian_filter(L, 1.5), 1))
    smooth = ndimage.uniform_filter(g, 9) < 6.0
    portrait = person is not None and (person > 128).mean() > 0.03
    # blue / overcast / sunset band; with a person in frame a bright neutral area is usually a backdrop or wall, not overcast sky
    colour = ((b < -10) & (L > 40)) | ((b < -5) & (L > 60)) | (((L > 75) & (C < 14)) & (not portrait)) | ((L > 45) & (b > 8) & (a > -5) & (np.arange(H)[:, None] < H * 0.4))
    cand = smooth & colour
    cand[int(H * 0.8):] = False
    if depth is not None: cand &= depth > 150
    if person is not None: cand &= ndimage.binary_dilation(person > 128, iterations=8) == False
    lab_, n = ndimage.label(cand)
    if n == 0: return np.zeros((H, W), np.uint8)
    top = set(np.unique(lab_[: max(2, H // 50)])) - {0}
    m = np.isin(lab_, list(top))
    if m.mean() < 0.01: return np.zeros((H, W), np.uint8)
    m = ndimage.binary_closing(m, iterations=3)
    return (ndimage.gaussian_filter(m.astype(np.float32), 2) * 255).astype(np.uint8)

# ------------------------------------------------------------------------------------------------ facts


def hour_of(ts):
    if not ts: return None
    try: return datetime.datetime.utcfromtimestamp(ts).hour   # EXIF DateTimeOriginal is local time stored as if UTC
    except Exception: return None


def measure(rgb, planes=None, faces=None):
    """Numbers of one rendering (uint8 sRGB). planes/faces at the same geometry (any size: resized)."""
    lab = srgb8_to_lab(rgb); L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]; C = np.hypot(a, b)
    H, W = L.shape
    pc = np.percentile(L, [1, 5, 50, 95, 99])
    mx, mn = rgb.max(-1), rgb.max(-1)
    out = dict(L_p1=pc[0], L_p5=pc[1], L_p50=pc[2], L_p95=pc[3], L_p99=pc[4], L_mean=float(L.mean()),
               clip_hi=float((rgb.max(-1) >= 254).mean()), clip_lo=float((rgb.max(-1) <= 4).mean()),
               C_mean=float(C.mean()), C_p90=float(np.percentile(C, 90)), C_over60=float((C > 60).mean()))
    neu = (C < 18) & (L > 20) & (L < 92)
    out['neutral_frac'] = float(neu.mean())
    out['cast_a'] = float(a[neu].mean()) if neu.sum() > 50 else float(a.mean())
    out['cast_b'] = float(b[neu].mean()) if neu.sum() > 50 else float(b.mean())
    out['gw_a'], out['gw_b'] = float(a.mean()), float(b.mean())
    rs = lambda p: np.asarray(Image.fromarray(p).resize((W, H), Image.BILINEAR)).astype(np.float32) / 255 if p.shape != (H, W) else p.astype(np.float32) / 255
    if planes:
        if 'sky' in planes:
            s = rs(planes['sky']) > 0.5
            out['sky_frac'] = float(s.mean())
            if s.sum() > 100:
                out['sky_L'] = float(L[s].mean()); out['sky_clip'] = float((rgb.max(-1)[s] >= 250).mean()); out['sky_b'] = float(b[s].mean())
        if 'person' in planes:
            p = rs(planes['person']) > 0.5; out['person_frac'] = float(p.mean())
        if 'faceskin' in planes:
            fs = rs(planes['faceskin']) > 0.5
            if fs.sum() > 50:
                out['skin_L'] = float(L[fs].mean()); out['skin_hue'] = float(np.degrees(np.arctan2(b[fs].mean(), a[fs].mean())) % 360)
                out['skin_C'] = float(np.hypot(a[fs].mean(), b[fs].mean())); out['skin_clip'] = float((rgb.max(-1)[fs] >= 250).mean())
    if faces:
        fw, fh = faces['size']; sx, sy = W / fw, H / fh; fl = []
        for f in faces['faces']:
            cx, cy, bw, bh = f['box']
            x0, x1 = int(max(0, (cx - bw * 0.3) * sx)), int(min(W, (cx + bw * 0.3) * sx)); y0, y1 = int(max(0, (cy - bh * 0.3) * sy)), int(min(H, (cy + bh * 0.3) * sy))
            if x1 - x0 < 2 or y1 - y0 < 2: continue
            fl.append(dict(L=float(L[y0:y1, x0:x1].mean()), p95=float(np.percentile(L[y0:y1, x0:x1], 95)), clip=float((rgb[y0:y1, x0:x1].max(-1) >= 250).mean()), area=bw * bh / (fw * fh)))
        out['faces'] = fl
    return {k: (round(v, 4) if isinstance(v, float) else v) for k, v in out.items()}


def cast_word(a, b):
    if abs(a) < 2.5 and abs(b) < 3.5: return 'neutral'
    w = []
    if b > 3.5: w.append('warm/yellow')
    if b < -3.5: w.append('cool/blue')
    if a > 2.5: w.append('magenta')
    if a < -2.5: w.append('green')
    return '+'.join(w)


def facts_text(m, exif, tone, pinned=(), auto_vals=None):
    """Compact English facts block (about 250-400 tokens)."""
    def ev(dL):  # L* difference to an approximate EV difference around mid-grey (L* 50 = 18 %: 1 EV ~ 13 L*)
        return dL / 13.0
    lines = []
    lines.append(f"Brightness (CIE L*, 0 black..100 white): p1 {m['L_p1']:.0f}, p5 {m['L_p5']:.0f}, median {m['L_p50']:.0f}, p95 {m['L_p95']:.0f}, p99 {m['L_p99']:.0f}. "
                 f"Engine tonal class: key {tone.get('key')}, range {tone.get('range')}.")
    lines.append(f"Clipping: {m['clip_hi'] * 100:.1f}% of pixels near white, {m['clip_lo'] * 100:.1f}% near black.")
    lines.append(f"Colour cast of near-neutral pixels ({m['neutral_frac'] * 100:.0f}% of frame): a* {m['cast_a']:+.1f}, b* {m['cast_b']:+.1f} -> {cast_word(m['cast_a'], m['cast_b'])}.")
    lines.append(f"Saturation: mean chroma C* {m['C_mean']:.0f}, p90 {m['C_p90']:.0f}, {m['C_over60'] * 100:.1f}% of pixels very saturated (C*>60).")
    if m.get('sky_frac', 0) > 0.02:
        lines.append(f"Sky (rough detector): {m['sky_frac'] * 100:.0f}% of frame, sky L* {m.get('sky_L', 0):.0f}, sky clipped {m.get('sky_clip', 0) * 100:.0f}%, sky b* {m.get('sky_b', 0):+.0f}.")
    else:
        lines.append("Sky: none detected.")
    nf = len(m.get('faces', []))
    if nf:
        fs = '; '.join(f"face {i + 1}: L* {f['L']:.0f} ({(f['L'] - m['L_p50']) / 13:+.1f} EV vs median), p95 {f['p95']:.0f}, {f['area'] * 100:.1f}% of frame" for i, f in enumerate(m['faces']))
        lines.append(f"People: {nf} face(s); {fs}.")
        if 'skin_hue' in m: lines.append(f"Skin: L* {m['skin_L']:.0f}, hue angle {m['skin_hue']:.0f} deg (typical skin 40-70), chroma {m['skin_C']:.0f}.")
    elif m.get('person_frac', 0) > 0.03:
        lines.append(f"People: a person covers {m['person_frac'] * 100:.0f}% of frame, no frontal face.")
    else:
        lines.append("People: none detected.")
    ex = []
    if exif.get('make') or exif.get('model'): ex.append(f"{exif.get('make', '')} {exif.get('model', '')}".strip())
    if exif.get('iso'): ex.append(f"ISO {exif['iso']}")
    if exif.get('shutter'): ex.append(f"1/{1 / exif['shutter']:.0f} s" if exif['shutter'] < 1 else f"{exif['shutter']:.1f} s")
    if exif.get('aperture'): ex.append(f"f/{exif['aperture']:.1f}")
    if exif.get('focalLength'): ex.append(f"{exif['focalLength']:.3g} mm")
    h = hour_of(exif.get('timestamp'))
    if h is not None: ex.append(f"taken at {h:02d}h local")
    if ex: lines.append('EXIF: ' + ', '.join(ex) + '.')
    if auto_vals: lines.append('Current state: ' + auto_vals)
    if pinned: lines.append('Pinned by the user (do not change): ' + ', '.join(pinned) + '.')
    return '\n'.join(lines)
