"""Writes raw input tensors and desktop reference outputs for the on-device benchmark (core/ai/bench, scripts/ai-eval/phone_run.py).
Run: tools/py-ai/Scripts/python.exe -I scripts/ai-eval/phone_prep.py [image ...]
Layout: build/ai-eval/phone/{in,ref}/<case>.<n>.bin ; build/ai-eval/phone/cases.json (shapes/dtypes of each file)."""
import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from models import *
P = os.path.join(OUT, 'phone'); os.makedirs(P + '/in', exist_ok=True); os.makedirs(P + '/ref', exist_ok=True)
imgs = sys.argv[1:] or ['person', 'fruit']
cases = {}
def dump(kind, case, arrs):
    meta = []
    for i, a in enumerate(arrs):
        a = np.ascontiguousarray(a); a.tofile(f'{P}/{kind}/{case}.{i}.bin'); meta.append([str(a.dtype), list(a.shape)])
    return meta
MAIN = ['mp_blazeface_128', 'mp_facemesh_256', 'mp_selfie_multiclass_256', 'mp_selfie_segmenter_256', 'mp_hair_512', 'mp_magictouch_enc_768',
        'dav2s_fp32', 'dav2s_q8', 'mobilesam_enc', 'u2netp']
for n in imgs:
    im = load(n)
    for m in MAIN:
        rt, rel, fn = M[m]; xs = fn(im); run = make_runner(m, 4); outs = run(xs); case = f'{m}__{n}'
        cases[case] = dict(model=m, rt=rt, path=rel, img=n, ins=dump('in', case, xs), outs=dump('ref', case, outs))
    # decoders need the encoders' outputs
    xs = i_mt_enc(im); enc = tfl(os.path.join(MD, 'mp_x/magic_touch__interactive_segmentation_encoder.int8.tflite'), 4); feats = tfl_run(enc, xs)
    dec = tfl(os.path.join(MD, 'mp_x/magic_touch__interactive_segmentation_decoder.int8.tflite'), 4)
    yy, xx = np.mgrid[0:768, 0:768]; tx, ty = 0.5, 0.5
    v = np.zeros((1, 768, 768, 3), np.float32); v[0, ..., 0] = ((xx - tx * 768) ** 2 + (yy - ty * 768) ** 2 < 12 ** 2)
    case = f'mp_magictouch_dec__{n}'; ins = [v] + feats; outs = tfl_run(dec, ins)
    cases[case] = dict(model='mp_magictouch_dec', rt='tflite', path='mp_x/magic_touch__interactive_segmentation_decoder.int8.tflite', img=n, ins=dump('in', case, ins), outs=dump('ref', case, outs))
    l = lng(im, 1024); w, h = l.size; menc = make_runner('mobilesam_enc', 4); emb = menc([np.asarray(l, np.float32)])[0]
    dec = ort_session(os.path.join(MD, 'onnx/mobilesam_dec.onnx'), 4); names = [i.name for i in dec.get_inputs()]
    feed = dict(image_embeddings=emb, point_coords=np.array([[[0.5 * w, 0.5 * h], [0, 0]]], np.float32), point_labels=np.array([[1, -1]], np.float32),
                mask_input=np.zeros((1, 1, 256, 256), np.float32), has_mask_input=np.zeros(1, np.float32), orig_im_size=np.array([h, w], np.float32))
    ins = [feed[k] for k in names]; outs = dec.run(None, feed); case = f'mobilesam_dec__{n}'
    cases[case] = dict(model='mobilesam_dec', rt='onnx', path='onnx/mobilesam_dec.onnx', img=n, names=names, ins=dump('in', case, ins), outs=dump('ref', case, outs))
    print(n, 'done')
json.dump(cases, open(P + '/cases.json', 'w'), indent=1)
