import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
from eval_seg_lib import *
import statistics
TAPS = {'person': [(0.45, 0.39)], 'fruit': [(0.52, 0.45), (0.17, 0.46)], 'sunset': [(0.77, 0.44)], 'baretree': [(0.48, 0.75)], 'statue': [(0.50, 0.53)], 'trees': [(0.52, 0.06)]}
enc = tfl(os.path.join(MD, 'mp_x/magic_touch__interactive_segmentation_encoder.int8.tflite'), 4)
decs = {t: tfl(os.path.join(MD, 'mp_x/magic_touch__interactive_segmentation_decoder.int8.tflite'), t) for t in (1, 4)}
yy, xx = np.mgrid[0:768, 0:768]; tiles = []; dt = {1: [], 4: []}
for n in IMAGES:
    im = load(n); feats = tfl_run(enc, [prep(im, (768, 768), scale=255.0, nchw=False)])
    for k, (tx, ty) in enumerate(TAPS[n]):
        v = np.zeros((768, 768, 3), np.float32); v[..., 0] = ((xx - tx * 768) ** 2 + (yy - ty * 768) ** 2 < 12 ** 2)
        for t in (1, 4):
            t0 = time.perf_counter(); o = tfl_run(decs[t], [v[None]] + feats)[0][0, :, :, 0]; dt[t].append((time.perf_counter() - t0) * 1e3)
        tn = f'{n}_t{k}'; tiles.append((tn, save_tap('mp_magictouch', tn, im, o, (tx, ty))))
        np.save(os.path.join(OUT, 'raw', f'magictouch_768__{tn}.npy'), o)
sheet('mp_magictouch', tiles)
print('magictouch decoder median ms: 1t %.1f  4t %.1f' % (statistics.median(dt[1]), statistics.median(dt[4])))
