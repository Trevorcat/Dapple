"""Portrait evidence sheets from the reference dumps of ai_ref.py (same numbers as the phone: core/ai tests prove equality).
Run: tools/py-ai/Scripts/python.exe -I scripts/ai-eval/portrait_report.py [name ...]
Writes build/ai-eval/portraits/out/<name>_sheet.jpg (landmarks | parts | tap select | depth) and <name>_hair_edge.png (raw upsampled hair mask vs guided
filter refinement on a crop of the hair boundary)."""
import sys, os, json; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
from gf import fast_guided
from PIL import ImageDraw
REF = os.path.join(OUT, 'ai-ref'); OD = os.path.join(OUT, 'portraits', 'out'); os.makedirs(OD, exist_ok=True)
PAL = np.array([[0, 0, 0], [255, 0, 255], [255, 170, 0], [255, 235, 140], [0, 120, 255], [150, 150, 150]], np.float32)
OVAL = [10, 338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136, 172, 58, 132, 93, 234, 127, 162, 21, 54, 103, 67, 109]

def load(n):
    d = os.path.join(REF, n); b = open(os.path.join(d, 'image.rgb'), 'rb').read(); w, h = np.frombuffer(b[:8], np.uint32)
    return Image.fromarray(np.frombuffer(b[8:], np.uint8).reshape(h, w, 3)), json.load(open(os.path.join(d, 'ref.json'))), d

def label(im, text):
    d = ImageDraw.Draw(im); d.rectangle([0, 0, im.width, 18], fill=(0, 0, 0)); d.text((4, 3), text, fill=(255, 255, 0)); return im

def panel_landmarks(im, ref, d):
    o = im.copy(); dr = ImageDraw.Draw(o); n = len(ref['faces'])
    for i, f in enumerate(ref['faces']):
        cx, cy, w, h = f['box']; flag = ref['landmark_flags'][i]; col = (0, 255, 0) if flag > 0 else (255, 60, 60)
        dr.rectangle([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], outline=col, width=3)
        for k, (kx, ky) in enumerate(f['kp']): dr.ellipse([kx - 3, ky - 3, kx + 3, ky + 3], fill=(255, 255, 0))
        dr.text((cx - w / 2, cy - h / 2 - 12), f"score {f['score']:.2f} mesh {flag:.1f}", fill=col)
    p = os.path.join(d, 'landmarks.f32')
    if os.path.exists(p) and ref['landmark_flags'][0] > 0:
        a = np.fromfile(p, np.float32); xy = a[:956].reshape(478, 2)
        for x, y in xy: dr.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=(0, 255, 255))
        dr.line([tuple(xy[j]) for j in OVAL + [OVAL[0]]], fill=(255, 0, 80), width=2)
    return label(o, f"faces {n}, valid {ref['faces_valid']}")

def panel_parts(im, ref, d):
    cls = np.fromfile(os.path.join(d, 'person_class.u8'), np.uint8).reshape(256, 256)
    big = np.asarray(Image.fromarray(cls).resize(im.size, Image.NEAREST)); m = (big > 0)[..., None]
    a = np.asarray(im, np.float32); out = np.where(m, 0.45 * a + 0.55 * PAL[big], a)
    p = ref['person']; o = Image.fromarray(out.astype(np.uint8))
    if not p['trusted']:
        o = label(o, f"GATE: not trusted ({p['reason']}) -> map would be all background; network said person on {100 * p['person']:.0f}%")
    else:
        o = label(o, f"gate: {p['reason']} blob {100 * p['blob']:.0f}% hair {100 * p['hair']:.1f}% face {100 * p['face']:.1f}% body {100 * p['body']:.0f}%")
    return o

def panel_tap(im, ref, d):
    m = np.fromfile(os.path.join(d, 'tap_mt.f32'), np.float32).reshape(768, 768)
    mm = np.asarray(Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8)).resize(im.size, Image.BILINEAR))
    o = overlay(im, Image.fromarray(mm)); dr = ImageDraw.Draw(o); x, y = ref['tap']
    dr.ellipse([x - 9, y - 9, x + 9, y + 9], outline=(255, 255, 0), width=3)
    return label(o, 'MagicTouch tap at face centre (768 low-res -> image)')

def panel_depth(im, ref, d):
    dp = np.fromfile(os.path.join(d, 'depth.f32'), np.float32).reshape(518, 518)
    v = norm01(dp); r = np.clip(1.5 - abs(4 * v - 3), 0, 1); g = np.clip(1.5 - abs(4 * v - 2), 0, 1); b = np.clip(1.5 - abs(4 * v - 1), 0, 1)
    o = Image.fromarray((np.stack([r, g, b], -1) * 255).astype(np.uint8)).resize(im.size, Image.BILINEAR)
    return label(o, 'Depth Anything V2 Small (518 squashed -> image)')

def hair_edge(im, ref, d, name):
    prob = np.fromfile(os.path.join(d, 'person_prob.u8'), np.uint8).reshape(6, 256, 256).astype(np.float32) / 255
    hair = prob[1]
    if hair.sum() < 30: return False
    big = np.asarray(Image.fromarray(hair, 'F').resize(im.size, Image.BICUBIC)); big = np.clip(big, 0, 1)
    I = np.asarray(im, np.float32) / 255; ref_m = fast_guided(I, big, r=16, eps=1e-3, s=4)
    # crop on the top-most hair boundary: window of 300 px centred on the topmost row with hair > .5 (shown at 2x)
    ys, xs = np.nonzero(big > 0.5)
    if ys.size == 0: return False
    cy = int(np.percentile(ys, 3)); cx = int(np.median(xs[ys < cy + 40])) if (ys < cy + 40).any() else int(xs.mean())
    x0 = int(np.clip(cx - 150, 0, im.width - 300)); y0 = int(np.clip(cy - 100, 0, im.height - 300)); box = (x0, y0, x0 + 300, y0 + 300)
    def ov(m): return overlay(im, Image.fromarray((m * 255).astype(np.uint8)), color=(255, 0, 80), alpha=0.6).crop(box).resize((600, 600), Image.LANCZOS)
    tiles = [im.crop(box).resize((600, 600), Image.LANCZOS), ov(big), ov(ref_m)]
    W = Image.new('RGB', (1800, 618), 'white')
    for i, (t, txt) in enumerate(zip(tiles, ['photo', 'hair class, 256 px prob, bicubic x4', 'after guided filter (r16, s4)'])): W.paste(label(t, txt), (i * 600, 18))
    W.save(os.path.join(OD, name + '_hair_edge.png')); return True

if __name__ == '__main__':
    names = sys.argv[1:] or sorted(n for n in os.listdir(REF) if n.startswith('p'))
    for n in names:
        im, ref, d = load(n)
        ps = [panel_landmarks(im, ref, d), panel_parts(im, ref, d), panel_tap(im, ref, d), panel_depth(im, ref, d)]
        h = 480; ps = [p.resize((round(p.width * h / p.height), h), Image.LANCZOS) for p in ps]
        cw = max(p.width for p in ps); W2 = Image.new('RGB', (2 * cw, 2 * h), 'white')
        for i, p in enumerate(ps): W2.paste(p, ((i % 2) * cw, (i // 2) * h))
        W2.save(os.path.join(OD, n + '_sheet.jpg'), quality=88)
        he = hair_edge(im, ref, d, n); print(n, 'sheet written', 'hair edge written' if he else 'no hair crop')
