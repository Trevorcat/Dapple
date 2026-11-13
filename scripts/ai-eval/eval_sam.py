import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
from eval_seg_lib import *
import statistics
TAPS = {'person': [(0.45, 0.39)], 'fruit': [(0.52, 0.45), (0.17, 0.46)], 'sunset': [(0.77, 0.44)],
        'baretree': [(0.48, 0.75)], 'statue': [(0.50, 0.53)], 'trees': [(0.52, 0.06)]}
which = sys.argv[1:] or ['mobilesam', 'slimsam']
res = {}
def tapname(n, k): return f'{n}_t{k}'

if 'mobilesam' in which:
    enc = make_runner('mobilesam_enc', 4)
    dec = ort_session(os.path.join(MD, 'onnx/mobilesam_dec.onnx'), 1); tiles = []; dts = []
    for n in IMAGES:
        im = load(n); l = lng(im, 1024); w, h = l.size
        emb = enc([np.asarray(l, np.float32)])[0]
        for k, (tx, ty) in enumerate(TAPS[n]):
            pc = np.array([[[tx * w, ty * h], [0, 0]]], np.float32); pl = np.array([[1, -1]], np.float32)
            feed = dict(image_embeddings=emb, point_coords=pc, point_labels=pl, mask_input=np.zeros((1, 1, 256, 256), np.float32),
                        has_mask_input=np.zeros(1, np.float32), orig_im_size=np.array([h, w], np.float32))
            t0 = time.perf_counter(); out = dec.run(None, feed); dts.append((time.perf_counter() - t0) * 1e3)
            m = sigmoid(out[0][0, 0]); mm = Image.fromarray((m * 255).astype(np.uint8)).resize(im.size, Image.BILINEAR)
            tn = tapname(n, k); o = save_tap('mobilesam', tn, im, np.asarray(mm, np.float32) / 255, (tx, ty), out[1].ravel()[0]); tiles.append((tn + f' iou={out[1].ravel()[0]:.2f}', o))
            # keep 256x256 low-res logits for refinement experiments
            np.save(os.path.join(OUT, 'raw', f'mobilesam_lowres__{tn}.npy'), out[2][0, 0])
    sheet('mobilesam', tiles); print('mobilesam decoder 1-thread ms median', statistics.median(dts), 'max', max(dts))

if 'slimsam' in which:
    for ename in ['slimsam_enc', 'slimsam_enc_q8']:
        enc = make_runner(ename, 4); dec = ort_session(os.path.join(MD, 'onnx/slimsam_dec.onnx'), 1); tiles = []; dts = []
        for n in IMAGES:
            im = load(n); l = lng(im, 1024); w, h = l.size
            eo = enc(M[ename][2](im)); emb, pos = eo[0], eo[1]
            for k, (tx, ty) in enumerate(TAPS[n]):
                feed = dict(input_points=np.array([[[[tx * w, ty * h]]]], np.float32), input_labels=np.array([[[1]]], np.int64), image_embeddings=emb, image_positional_embeddings=pos)
                t0 = time.perf_counter(); out = dec.run(None, feed); dts.append((time.perf_counter() - t0) * 1e3)
                iou = out[0][0, 0]; b = int(iou.argmax()); lg = out[1][0, 0, b]           # 256x256 of the padded 1024 frame
                cw, ch = round(w / 4), round(h / 4); lg = lg[:ch, :cw]
                m = sigmoid(np.asarray(Image.fromarray(lg.astype(np.float32)).resize(im.size, Image.BILINEAR)))
                tn = tapname(n, k); o = save_tap(ename, tn, im, m, (tx, ty), iou[b]); tiles.append((tn + f' iou={iou[b]:.2f}', o))
        sheet(ename, tiles); print(ename, 'decoder 1-thread ms median', statistics.median(dts))
