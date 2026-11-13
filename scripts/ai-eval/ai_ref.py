"""Desktop reference of the core/ai task wrappers (detectFaces, faceLandmarks, segmentPerson + gate, tapSelect, depth).
The C++ in core/ai/src/tasks_*.cpp mirrors this file line by line; core/ai/tests compares its outputs with the files written here.
Run: tools/py-ai/Scripts/python.exe -I scripts/ai-eval/ai_ref.py [image ...]     (names under build/ai-eval/src and build/ai-eval/portraits)
Writes build/ai-eval/ai-ref/<name>/ (image.rgb, faces.json, landmarks.json, person.json, person_class.u8, person_prob.u8, tap_mt.f32, tap_sam.f32, depth.f32)."""
import sys, os, math; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
import models
import zipfile


def rs(im, size):
    """PIL Image.resize(BILINEAR) with the passes spelled out: horizontal first, then vertical (PIL itself picks the pass order per size, by a rule
    that differs by one grey level on a few percent of the pixels). core/ai/src/imageops.cpp reproduces exactly this."""
    w, h = im.size
    if tuple(size) == (w, h): return im.copy()
    if size[0] != w and size[1] != h: im = im.resize((size[0], h), Image.BILINEAR)
    return im.resize(tuple(size), Image.BILINEAR)
def prep(im, size, mean=None, std=None, scale=255.0, nchw=True):
    a = np.asarray(rs(im, size), np.float32) / scale
    if mean is not None: a = (a - mean) / std
    return a.transpose(2, 0, 1)[None].copy() if nchw else a[None].copy()
def lng(im, n):
    w, h = im.size; s = n / max(w, h); return rs(im, (round(w * s), round(h * s)))
def i_dav2(im): return [prep(im, (518, 518), IMN_M, IMN_S)]
REF = os.path.join(OUT, 'ai-ref')
PORT = os.path.join(OUT, 'portraits')
MODELS = dict(
    face_detector=('tflite', 'mp_x/face_landmarker__face_detector.tflite'),
    face_landmarks=('tflite', 'mp_x/face_landmarker__face_landmarks_detector.tflite'),
    person_parts=('tflite', 'mp/selfie_multiclass_256x256.tflite'),
    tap_encoder=('tflite', 'mp_x/magic_touch__interactive_segmentation_encoder.int8.tflite'),
    tap_decoder=('tflite', 'mp_x/magic_touch__interactive_segmentation_decoder.int8.tflite'),
    depth=('onnx', 'onnx/dav2s_fp32.onnx'),
    sam_encoder=('onnx', 'onnx/mobilesam_enc.onnx'),
    sam_decoder=('onnx', 'onnx/mobilesam_dec.onnx'),
)
_cache = {}
def runner(name, threads=4):
    if name not in _cache:
        rt, rel = MODELS[name]; p = os.path.join(MD, rel)
        if rt == 'onnx':
            s = ort_session(p, threads); names = [i.name for i in s.get_inputs()]; _cache[name] = (lambda xs: s.run(None, dict(zip(names, xs))), names)
        else:
            it = tfl(p, threads); _cache[name] = (lambda xs: tfl_run(it, xs), None)
    return _cache[name]

# ---- anchors (BlazeFace short range: 16x16 x2 and 8x8 x6 anchors, centres at (x+.5)/fm, fixed size) -------------------------------
def anchors():
    a = []
    for fm, k in ((16, 2), (8, 6)):
        for y in range(fm):
            for x in range(fm):
                for _ in range(k): a.append(((x + .5) / fm, (y + .5) / fm))
    return np.array(a, np.float32)
ANCH = anchors()

def iou(a, b):
    ax0, ay0, ax1, ay1 = a[0] - a[2] / 2, a[1] - a[3] / 2, a[0] + a[2] / 2, a[1] + a[3] / 2
    bx0, by0, bx1, by1 = b[0] - b[2] / 2, b[1] - b[3] / 2, b[0] + b[2] / 2, b[1] + b[3] / 2
    iw = max(0.0, min(ax1, bx1) - max(ax0, bx0)); ih = max(0.0, min(ay1, by1) - max(ay0, by0)); inter = iw * ih
    u = a[2] * a[3] + b[2] * b[3] - inter
    return inter / u if u > 0 else 0.0

def detect_faces(im, min_score=0.5):
    """returns list of dict(score, box=(cx,cy,w,h), kp=[6][2]) in image pixels, sorted by score (weighted NMS, IoU 0.3)"""
    W, H = im.size; s = max(W, H); ox, oy = (s - W) // 2, (s - H) // 2
    sq = Image.new('RGB', (s, s), (0, 0, 0)); sq.paste(im, (ox, oy))
    x = prep(sq, (128, 128), np.float32(127.5), np.float32(127.5), scale=1.0, nchw=False)
    raw, sc = runner('face_detector')[0]([x]); sc = sigmoid(np.clip(sc[0, :, 0], -100, 100)); raw = raw[0]
    cand = []
    for i in np.nonzero(sc >= min_score)[0]:
        r = raw[i]; ax, ay = ANCH[i]
        box = [(r[0] / 128 + ax) * s - ox, (r[1] / 128 + ay) * s - oy, r[2] / 128 * s, r[3] / 128 * s]
        kp = [[(r[4 + 2 * k] / 128 + ax) * s - ox, (r[5 + 2 * k] / 128 + ay) * s - oy] for k in range(6)]
        cand.append(dict(score=float(sc[i]), box=box, kp=kp))
    cand.sort(key=lambda c: -c['score']); out = []
    while cand:
        best = cand[0]; grp = [c for c in cand if iou(best['box'], c['box']) > 0.3]   # includes best
        cand = [c for c in cand if iou(best['box'], c['box']) <= 0.3]
        w = np.array([c['score'] for c in grp], np.float64); wsum = w.sum()
        box = [float(sum(c['box'][j] * c['score'] for c in grp) / wsum) for j in range(4)]
        kp = [[float(sum(c['kp'][k][j] * c['score'] for c in grp) / wsum) for j in range(2)] for k in range(6)]
        out.append(dict(score=best['score'], box=box, kp=kp))
    return out

def face_landmarks(im, face):
    cx, cy, bw, bh = face['box']; le, re = face['kp'][0], face['kp'][1]
    ang = math.atan2(re[1] - le[1], re[0] - le[0]); size = max(bw, bh) * 1.5
    c, sn = math.cos(ang), math.sin(ang); k = size / 256
    coeff = (c * k, -sn * k, cx - 128 * k * c + 128 * k * sn, sn * k, c * k, cy - 128 * k * sn - 128 * k * c)
    crop = im.transform((256, 256), Image.AFFINE, coeff, Image.BILINEAR)
    outs = runner('face_landmarks')[0]([prep(crop, (256, 256), scale=255.0, nchw=False)])
    lm = outs[0].reshape(-1, 3); flag = float(outs[1].ravel()[0])
    xy = np.array([[coeff[0] * u + coeff[1] * v + coeff[2], coeff[3] * u + coeff[4] * v + coeff[5]] for u, v, _ in lm], np.float32)
    return dict(valid=flag > 0, flag=flag, xy=xy, z=(lm[:, 2] * k).astype(np.float32), crop=list(coeff))

# ---- person parts ---------------------------------------------------------------------------------------------------------------
def segment_person_raw(im):
    o = runner('person_parts')[0]([prep(im, (256, 256), scale=255.0, nchw=False)])[0][0]   # 256x256x6 probabilities
    return o

def largest_blob(mask):
    """4-connected largest component (area in pixels) of a bool array; returns (area, label array, label)"""
    from scipy import ndimage
    lab, n = ndimage.label(mask)
    if n == 0: return 0, lab, 0
    sizes = ndimage.sum(mask, lab, range(1, n + 1)); k = int(np.argmax(sizes)) + 1
    return int(sizes[k - 1]), lab, k

GATE = dict(min_blob=0.03, min_hair=0.004, min_face_skin=0.004, min_body=0.01)
def gate_region(cls):
    """person gate without a validated face: the largest blob of person classes (hair/body-skin/face-skin/clothes) must be person-sized
    (>= 3% of the frame), contain hair (>= 0.4%) and either face skin (>= 0.4%) or body skin / clothes (>= 1%). Network hallucinations on
    fruit or statues have skin/clothes classes but no hair; profile, back-of-head and small-face portraits have hair plus skin or clothes."""
    n = cls.size; pm = (cls >= 1) & (cls <= 4); area, lab, k = largest_blob(pm)
    blob = lab == k if k else pm
    frac = lambda m: float((m & blob).sum()) / n
    f = dict(blob=area / n, face=frac(cls == 3), hair=frac(cls == 1), body=frac((cls == 2) | (cls == 4)), person=float(pm.sum()) / n)
    ok = f['blob'] >= GATE['min_blob'] and f['hair'] >= GATE['min_hair'] and (f['face'] >= GATE['min_face_skin'] or f['body'] >= GATE['min_body'])
    return ok, f

def segment_person(im, faces_valid):
    prob = segment_person_raw(im); cls = prob.argmax(-1).astype(np.uint8)
    okr, f = gate_region(cls)
    if faces_valid > 0: trusted, reason = True, 'face'
    elif okr: trusted, reason = True, 'region'
    else: trusted, reason = False, 'none'
    return prob, cls, trusted, reason, f

# ---- tap select (MagicTouch: disk of radius 12 px in channel 0 of the 768x768x3 click tensor) -----------------------------------------
YY, XX = np.mgrid[0:768, 0:768]
def prepare_tap_mt(im):
    return runner('tap_encoder', 4)[0]([prep(im, (768, 768), scale=255.0, nchw=False)])
def tap_mt(feats, x, y, W, H):
    v = np.zeros((1, 768, 768, 3), np.float32); tx, ty = x / W * 768, y / H * 768
    v[0, ..., 0] = ((XX - tx) ** 2 + (YY - ty) ** 2 < 12 ** 2)
    return runner('tap_decoder', 4)[0]([v] + feats)[0][0, :, :, 0]

# ---- tap select (MobileSAM: long edge 1024, point + padding point; low-res 256x256 logits are returned) ---------------------------------
def prepare_tap_sam(im):
    l = lng(im, 1024); emb = runner('sam_encoder', 4)[0]([np.asarray(l, np.float32)])[0]; return emb, l.size
def tap_sam(emb, size, x, y, W, H, positive=True):
    w, h = size; names = runner('sam_decoder', 1)[1]
    feed = dict(image_embeddings=emb, point_coords=np.array([[[x / W * w, y / H * h], [0, 0]]], np.float32), point_labels=np.array([[1 if positive else 0, -1]], np.float32),
                mask_input=np.zeros((1, 1, 256, 256), np.float32), has_mask_input=np.zeros(1, np.float32), orig_im_size=np.array([h / 4, w / 4], np.float32))
    return runner('sam_decoder', 1)[0]([feed[k] for k in names])   # [masks (1,1,h/4,w/4), iou, low_res]

# ---- depth ------------------------------------------------------------------------------------------------------------------------
def depth(im): return runner('depth', 4)[0](i_dav2(im))[0][0]

# ---- main: dump per-image references --------------------------------------------------------------------------------------------
def load_any(name):
    for d in (SRC, PORT):
        p = os.path.join(d, name + '.jpg')
        if os.path.exists(p): return Image.open(p).convert('RGB')
    raise FileNotFoundError(name)

def dump_image(name, long_edge=1024, sam=True):
    im = lng(load_any(name), long_edge); W, H = im.size; d = os.path.join(REF, name); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'image.rgb'), 'wb') as f:
        f.write(np.array([W, H], np.uint32).tobytes()); f.write(np.asarray(im, np.uint8).tobytes())
    res = dict(w=W, h=H)
    open(os.path.join(d, 'pil_resize256.rgb'), 'wb').write(np.asarray(rs(im, (256, 256)), np.uint8).tobytes())
    open(os.path.join(d, 'pil_resize518.rgb'), 'wb').write(np.asarray(rs(im, (518, 518)), np.uint8).tobytes())
    faces = detect_faces(im); res['faces'] = faces
    lms = []; nvalid = 0
    for fc in faces:
        lm = face_landmarks(im, fc); nvalid += lm['valid']; lms.append(lm)
    if lms:  # the first face's landmarks go to a binary file
        c = lms[0]['crop']; open(os.path.join(d, 'pil_affine.rgb'), 'wb').write(np.asarray(im.transform((256, 256), Image.AFFINE, tuple(c), Image.BILINEAR), np.uint8).tobytes())
        np.concatenate([lms[0]['xy'].ravel(), lms[0]['z'], np.array(lms[0]['crop'], np.float32), np.array([lms[0]['flag']], np.float32)]).astype(np.float32).tofile(os.path.join(d, 'landmarks.f32'))
    res['landmark_flags'] = [lm['flag'] for lm in lms]; res['faces_valid'] = nvalid
    prob, cls, trusted, reason, feat = segment_person(im, nvalid)
    cls.tofile(os.path.join(d, 'person_class.u8')); np.clip(np.round(prob.transpose(2, 0, 1) * 255), 0, 255).astype(np.uint8).tofile(os.path.join(d, 'person_prob.u8'))
    res['person'] = dict(trusted=trusted, reason=reason, **feat)
    # taps: the first valid face centre when there is one, else the image centre
    tx, ty = (faces[0]['box'][0], faces[0]['box'][1]) if faces else (W / 2, H / 2); res['tap'] = [tx, ty]
    feats = prepare_tap_mt(im); m = tap_mt(feats, tx, ty, W, H); m.astype(np.float32).tofile(os.path.join(d, 'tap_mt.f32'))
    if sam:
        emb, size = prepare_tap_sam(im); out = tap_sam(emb, size, tx, ty, W, H)
        res['sam'] = dict(size=list(size), iou=float(out[1].ravel()[0]), mask_shape=list(out[0].shape))
        out[0][0, 0].astype(np.float32).tofile(os.path.join(d, 'tap_sam.f32')); sigmoid(out[2][0, 0]).astype(np.float32).tofile(os.path.join(d, 'tap_sam_lowres.f32'))
    dp = depth(im); dp.astype(np.float32).tofile(os.path.join(d, 'depth.f32')); res['depth'] = dict(lo=float(dp.min()), hi=float(dp.max()))
    json.dump(res, open(os.path.join(d, 'ref.json'), 'w'), indent=1)
    with open(os.path.join(d, 'ref.txt'), 'w') as t:   # trivially parseable twin of ref.json for the C++ tests
        t.write(f"size {W} {H}\nfaces {len(faces)} valid {nvalid}\n")
        for fc, fl in zip(faces, res['landmark_flags']):
            t.write('face %.6f %s %s flag %.6f\n' % (fc['score'], ' '.join('%.5f' % v for v in fc['box']), ' '.join('%.5f' % v for kp in fc['kp'] for v in kp), fl))
        p = res['person']; t.write("person %d %s blob %.6f face %.6f hair %.6f body %.6f person %.6f\n" % (p['trusted'], p['reason'], p['blob'], p['face'], p['hair'], p['body'], p['person']))
        t.write("tap %.5f %.5f\n" % (tx, ty))
        if sam: t.write("sam %d %d %.6f\n" % (size[0], size[1], res['sam']['iou']))
        t.write("depth %.6f %.6f\n" % (res['depth']['lo'], res['depth']['hi']))
    return res

if __name__ == '__main__':
    names = sys.argv[1:] or IMAGES + sorted(os.path.splitext(f)[0] for f in os.listdir(PORT) if f.endswith('.jpg'))
    for n in names:
        r = dump_image(n)
        print(f"{n:28s} faces={len(r['faces'])} valid={r['faces_valid']} flags={[round(f, 1) for f in r['landmark_flags']]} person={r['person']['reason']:7s} "
              f"blob={r['person']['blob']:.3f} face={r['person']['face']:.4f} hair={r['person']['hair']:.4f} body={r['person']['body']:.3f} sam_iou={r.get('sam', {}).get('iou', 0):.2f}")
