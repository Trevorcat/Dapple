import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
from PIL import ImageDraw
TAPS = {'person': [(0.45, 0.39)], 'fruit': [(0.52, 0.45), (0.17, 0.46)], 'sunset': [(0.77, 0.44)],
        'baretree': [(0.48, 0.75)], 'statue': [(0.50, 0.53)], 'trees': [(0.52, 0.06)]}
DISP = 1024
os.makedirs(os.path.join(OUT, 'raw'), exist_ok=True)

def save(model, name, im, m, extra=None):
    """m: float HxW 0..1 at any res. writes raw mask png + overlay png"""
    d = os.path.join(OUT, model); os.makedirs(d, exist_ok=True)
    mi = mask_to_img(m, im.size); mi.save(os.path.join(OUT, 'raw', f'{model}__{name}.png'))
    v = im.copy(); v.thumbnail((DISP, DISP)); mv = mi.resize(v.size, Image.BILINEAR)
    o = overlay(v, mv); o.save(os.path.join(d, name + '.png')); return o

def sheet(model, tiles):
    W = Image.new('RGB', (3 * 480, 2 * 360), 'white'); dr = ImageDraw.Draw(W)
    for k, (n, o) in enumerate(tiles):
        t = o.copy(); t.thumbnail((476, 340)); x, y = (k % 3) * 480, (k // 3) * 360; W.paste(t, (x, y)); dr.text((x + 3, y + 3), n, fill='yellow')
    W.save(os.path.join(OUT, f'sheet_{model}.jpg'), quality=88)

def depth_vis(d):
    d = norm01(d); r = np.clip(1.5 - abs(4 * d - 3), 0, 1); g = np.clip(1.5 - abs(4 * d - 2), 0, 1); b = np.clip(1.5 - abs(4 * d - 1), 0, 1)
    return Image.fromarray((np.stack([r, g, b], -1) * 255).astype(np.uint8))

def run_simple(model, post, label=None):
    run = make_runner(model, 4); fn = M[model][2]; tiles = []; ts = []
    for n in IMAGES:
        im = load(n); t0 = time.perf_counter(); out = run(fn(im)); ts.append(time.perf_counter() - t0)
        m = post(out, im, n)
        if isinstance(m, Image.Image):
            d = os.path.join(OUT, model); os.makedirs(d, exist_ok=True); v = m.copy(); v.thumbnail((DISP, DISP)); v.save(os.path.join(d, n + '.png')); tiles.append((n, v))
        else: tiles.append((n, save(model, n, im, m)))
    sheet(model, tiles); print(model, 'ok', 'mean ms', round(1e3 * np.mean(ts)))

which = sys.argv[1:] or ['all']
def want(k): return 'all' in which or k in which

if want('u2netp'): run_simple('u2netp', lambda o, im, n: norm01(o[0][0, 0]))
if want('isnet'): run_simple('isnet-general-use', lambda o, im, n: norm01(o[0][0, 0]))
if want('modnet'): run_simple('modnet_fp32', lambda o, im, n: o[0][0, 0])
if want('birefnet'): run_simple('birefnet_lite_fp32', lambda o, im, n: sigmoid(o[0][0, 0]))
if want('selfie'): run_simple('mp_selfie_segmenter_256', lambda o, im, n: (print(n, 'selfie range', o[0].min(), o[0].max()), o[0][0, :, :, 0])[1])
if want('hair'): run_simple('mp_hair_512', lambda o, im, n: (print(n, 'hair range', o[0].min(), o[0].max(), o[0].shape), sigmoid(o[0][0, :, :, 1] - o[0][0, :, :, 0]))[1])
if want('deeplab'):
    run_simple('mp_deeplabv3_257', lambda o, im, n: (o[0][0].argmax(-1) == 15).astype(np.float32))
if want('multiclass'):
    pal = np.array([[0, 0, 0], [255, 0, 255], [255, 170, 0], [255, 220, 120], [0, 120, 255], [150, 150, 150]], np.float32)
    def post(o, im, n):
        c = o[0][0].argmax(-1); cm = Image.fromarray(pal[c].astype(np.uint8)).resize(im.size, Image.NEAREST)
        a = np.asarray(im, np.float32); b = np.asarray(cm, np.float32); k = (np.asarray(Image.fromarray(c.astype(np.uint8)).resize(im.size, Image.NEAREST)) > 0)[..., None]
        out = np.where(k, 0.5 * a + 0.5 * b, a); 
        # also keep probability raw for refinement
        Image.fromarray((np.clip(1 - o[0][0][..., 0], 0, 1) * 255).astype(np.uint8)).resize(im.size, Image.BILINEAR).save(os.path.join(OUT, 'raw', f'mp_selfie_multiclass_256__{n}.png'))
        return Image.fromarray(out.astype(np.uint8))
    run_simple('mp_selfie_multiclass_256', post)
if want('depth'):
    for mdl in ['dav2s_fp32', 'dav2s_q8', 'midas_small']:
        def post(o, im, n, mdl=mdl):
            d = o[0]; d = d[0] if d.ndim == 3 else d[0, 0]; return depth_vis(d).resize(im.size, Image.BILINEAR)
        run_simple(mdl, post)
