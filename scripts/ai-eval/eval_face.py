import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
from eval_seg_lib import *
import math
OVAL = [10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109]
anch = []
for fm, k in ((16, 2), (8, 6)):
    for y in range(fm):
        for x in range(fm):
            for _ in range(k): anch.append(((x + .5) / fm, (y + .5) / fm))
anch = np.array(anch, np.float32)
det = make_runner('mp_blazeface_128', 4); mesh = make_runner('mp_facemesh_256', 4); tiles = []
for n in ['person', 'statue', 'fruit']:
    im = load(n); W, H = im.size; s = max(W, H)
    sq = Image.new('RGB', (s, s), (0, 0, 0)); sq.paste(im, ((s - W) // 2, (s - H) // 2)); ox, oy = (s - W) // 2, (s - H) // 2
    x = prep(sq, (128, 128), np.float32(127.5), np.float32(127.5), scale=1.0, nchw=False)
    raw, sc = det([x]); sc = sigmoid(np.clip(sc[0, :, 0], -100, 100)); i = int(sc.argmax())
    print(n, 'best face score', float(sc[i]))
    o = im.copy(); dr = ImageDraw.Draw(o)
    if sc[i] > 0.5:
        r = raw[0, i]; cx = (r[0] / 128 + anch[i, 0]) * s - ox; cy = (r[1] / 128 + anch[i, 1]) * s - oy; bw = r[2] / 128 * s; bh = r[3] / 128 * s
        le = ((r[4] / 128 + anch[i, 0]) * s - ox, (r[5] / 128 + anch[i, 1]) * s - oy); re = ((r[6] / 128 + anch[i, 0]) * s - ox, (r[7] / 128 + anch[i, 1]) * s - oy)
        ang = math.atan2(re[1] - le[1], re[0] - le[0]); size = max(bw, bh) * 1.5
        c, sn = math.cos(ang), math.sin(ang); k = size / 256
        coeff = (c * k, -sn * k, cx - 128 * k * c + 128 * k * sn, sn * k, c * k, cy - 128 * k * sn - 128 * k * c)
        crop = im.transform((256, 256), Image.AFFINE, coeff, Image.BILINEAR)
        lm, fs = mesh([prep(crop, (256, 256), scale=255.0, nchw=False)])[:2]
        pts = lm.reshape(-1, 3)[:, :2]; print(n, 'mesh face flag', fs.ravel()[:1])
        P = np.array([[coeff[0] * u + coeff[1] * v + coeff[2], coeff[3] * u + coeff[4] * v + coeff[5]] for u, v in pts])
        dr.rectangle([cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2], outline=(255, 255, 0), width=5)
        for px, py in P: dr.ellipse([px - 3, py - 3, px + 3, py + 3], fill=(0, 255, 255))
        dr.line([tuple(P[j]) for j in OVAL + [OVAL[0]]], fill=(255, 0, 80), width=5)
    o.thumbnail((DISP, DISP)); os.makedirs(os.path.join(OUT, 'mp_face'), exist_ok=True); o.save(os.path.join(OUT, 'mp_face', n + '.png')); tiles.append((n, o))
sheet('mp_face', tiles)
