import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
from gf import fast_guided
from PIL import ImageDraw
def up(p, size): return np.asarray(Image.fromarray(p.astype(np.float32), 'F').resize(size, Image.BICUBIC)).clip(0, 1)
def cases():
    for n, t in [('trees', 0), ('sunset', 0), ('baretree', 0)]:
        im = load(n); w, h = lng(im, 1024).size; lg = np.load(os.path.join(OUT, 'raw', f'mobilesam_lowres__{n}_t{t}.npy'))
        lg = lg[:round(h / 4), :round(w / 4)]; yield f'mobilesam256_{n}', im, sigmoid(lg)
    n = 'person'; im = load(n); p = np.asarray(Image.open(os.path.join(OUT, 'raw', 'mp_selfie_multiclass_256__person.png')).resize((256, 256), Image.BILINEAR), np.float32) / 255
    yield 'multiclass256_person', im, p     # note: png was written at full size from 256 probs; re-squashed to 256 here (aspect distorted, as the model sees it)
CROP = 300
for name, im, p in cases():
    W, H = im.size; I = np.asarray(im, np.float32) / 255
    if p.shape[0] == 256 and 'multiclass' in name: pu = up(p, (W, H))
    else: pu = up(p, (W, H))
    res = {}
    for s in (4, 1):
        t0 = time.perf_counter(); q = fast_guided(I, pu, r=16 if s == 4 else 16, eps=1e-3, s=s); res[s] = (q, (time.perf_counter() - t0) * 1e3)
    print(name, 'GF ms: s=4 %.0f  s=1 %.0f  (image %dx%d)' % (res[4][1], res[1][1], W, H))
    # pick the crop with most uncertain (0.15..0.85) mask pixels in bilinear version, on a coarse grid
    unc = ((pu > .15) & (pu < .85)).astype(np.float32); best = (-1, 0, 0)
    for y in range(0, H - CROP, 100):
        for x in range(0, W - CROP, 100):
            v = unc[y:y + CROP, x:x + CROP].sum(); best = max(best, (v, x, y))
    _, x, y = best
    if 'person' in name: x, y = 330, 280   # hair against grass (manual choice)
    box = (x, y, x + CROP, y + CROP)
    bg = np.array([255, 0, 255], np.float32)
    def comp(m): return Image.fromarray((I * m[..., None] * 255 + bg * (1 - m[..., None])).astype(np.uint8)).crop(box)
    tiles = [im.crop(box), comp(pu), comp(res[4][0]), comp(res[1][0])]
    labels = ['image', 'bicubic upsample of net mask', 'guided filter s=4 (fast)', 'guided filter s=1 (exact)']
    Wt = Image.new('RGB', (4 * CROP, CROP + 14), 'white'); d = ImageDraw.Draw(Wt)
    for k, t in enumerate(tiles): Wt.paste(t, (k * CROP, 14)); d.text((k * CROP + 3, 1), labels[k], fill='black')
    Wt = Wt.resize((Wt.width * 3 // 2, Wt.height * 3 // 2), Image.NEAREST)
    Wt.save(os.path.join(OUT, f'gf_{name}.png')); print('crop', box)
