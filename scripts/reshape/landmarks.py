"""Anchor points for the portrait-reshaping evidence (docs/engine/pipeline-architecture.md section 45): the 54 Face Mesh anchors of every face
(core/include/dapple/reshape.h order), the 33 BlazePose landmarks of the main person, the person plane (background protection).
Run: F:/projects/sony-render/tools/py-ai/Scripts/python.exe -I F:/projects/sony-render/scripts/reshape/landmarks.py [name ...]
Inputs: build/ai-eval/portraits/<p*>.jpg (CC0, docs/research/ai-test-images.md) and build/ai-eval/body/<b*>.jpg.
Writes build/reshape/in/<name>/ (photo.png <= 2048, person.png at 1024, meta.json) and build/reshape/in/<name>_pts.jpg (points drawn).
Pose model: MediaPipe Pose Landmarker full (Apache-2.0, model card "BlazePose GHUM 3D"), tools/_dl/ai-models/mp_pose/x_full/."""
import sys, os, math, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'ai-eval'))
sys.path.insert(0, os.path.join(HERE, '..', 'retouch'))
import numpy as np
from PIL import Image, ImageDraw
from ai_ref import lng, segment_person_raw
from common import ROOT, tfl, tfl_run
from make_masks import all_faces

ANCHOR_MESH = [10, 109, 67, 103, 54, 21, 162, 127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361,
               323, 454, 356, 389, 251, 284, 332, 297, 338, 468, 473, 33, 133, 362, 263, 159, 145, 386, 374, 64, 294, 1, 168, 61, 291, 13, 14]
PORT = os.path.join(ROOT, 'build', 'ai-eval', 'portraits')
BODY = os.path.join(ROOT, 'build', 'ai-eval', 'body')
OUT = os.path.join(ROOT, 'build', 'reshape', 'in')
POSE = os.path.join(ROOT, 'tools', '_dl', 'ai-models', 'mp_pose', 'x_full')
NAMES = ['p1_glasses_flag', 'p2_dark_skin_dreads', 'p3_curly_glasses_outdoor', 'p5_headscarf', 'p6_asian_woman_grey', 'p9_three_people_frontal',
         'b1_seated_beach', 'b2_standing_parliament', 'b3_standing_hands_pockets', 'b4_cadet_leaning_wall', 'b5_couple_wall', 'b6_three_women_street']

_it = {}
def interp(name):
    if name not in _it: _it[name] = tfl(os.path.join(POSE, name), 4)
    return _it[name]


def pose_anchors():
    """SSD anchors of the BlazePose detector: strides 8, 16, 32 x3 -> 28x28x2, 14x14x2, 7x7x6 = 2254, fixed size, centres (x + .5) / fm"""
    a = []
    for fm, k in ((28, 2), (14, 2), (7, 6)):
        for y in range(fm):
            for x in range(fm):
                for _ in range(k): a.append(((x + .5) / fm, (y + .5) / fm))
    return np.array(a, np.float32)
PANCH = pose_anchors()


def detect_pose(im):
    """BlazePose detector (224, letterboxed, input in -1..1): the best detection -> its keypoints (0 = mid hip, 1 = the full-body circle point)"""
    W, H = im.size; s = max(W, H); ox, oy = (s - W) // 2, (s - H) // 2
    sq = Image.new('RGB', (s, s), (0, 0, 0)); sq.paste(im, (ox, oy))
    x = np.asarray(sq.resize((224, 224), Image.BILINEAR), np.float32)[None] / 127.5 - 1.0
    raw, sc = tfl_run(interp('pose_detector.tflite'), [x])
    sc = 1 / (1 + np.exp(-np.clip(sc[0, :, 0], -100, 100))); raw = raw[0]
    i = int(np.argmax(sc))
    if sc[i] < 0.5: return None
    ax, ay = PANCH[i]; r = raw[i]
    kp = [((r[4 + 2 * k] / 224 + ax) * s - ox, (r[5 + 2 * k] / 224 + ay) * s - oy) for k in range(4)]
    return dict(score=float(sc[i]), kp=kp)


def pose_landmarks(im, det):
    """the landmark model on the rotated square crop (side 2.5 x |kp1 - kp0|, up along kp0 -> kp1), 256 px, input 0..1"""
    (x0, y0), (x1, y1) = det['kp'][0], det['kp'][1]
    d = np.array([x1 - x0, y1 - y0]); L = float(np.hypot(*d)); d /= max(L, 1e-6)
    size = 2 * L * 1.25
    th = math.atan2(d[0], -d[1])
    c, sn = math.cos(th), math.sin(th); k = size / 256
    coeff = (c * k, -sn * k, x0 - 128 * k * c + 128 * k * sn, sn * k, c * k, y0 - 128 * k * sn - 128 * k * c)
    crop = im.transform((256, 256), Image.AFFINE, coeff, Image.BILINEAR)
    xin = np.asarray(crop, np.float32)[None] / 255.0
    outs = tfl_run(interp('pose_landmarks_detector.tflite'), [xin])
    lm = np.asarray(outs[0]).reshape(39, 5); flag = float(np.asarray(outs[1]).ravel()[0])
    sig = lambda v: 1 / (1 + math.exp(-float(v)))
    pts = [(coeff[0] * u + coeff[1] * v + coeff[2], coeff[3] * u + coeff[4] * v + coeff[5], sig(vis) * sig(pres)) for u, v, z, vis, pres in lm[:33]]
    return dict(flag=flag, pts=pts)


SKEL = [(11, 12), (11, 13), (13, 15), (12, 14), (14, 16), (11, 23), (12, 24), (23, 24), (23, 25), (25, 27), (24, 26), (26, 28), (27, 31), (28, 32), (7, 8), (0, 7), (0, 8)]


def process(name):
    src = os.path.join(PORT if name.startswith('p') else BODY, name + '.jpg')
    full = Image.open(src).convert('RGB'); W0, H0 = full.size
    d = os.path.join(OUT, name); os.makedirs(d, exist_ok=True)
    s = min(1.0, 2048 / max(W0, H0)); full.resize((round(W0 * s), round(H0 * s)), Image.LANCZOS).save(os.path.join(d, 'photo.png'))
    im = lng(full, 1024); w, h = im.size
    prob = segment_person_raw(im)
    person = 1 - np.asarray(Image.fromarray(prob[..., 0].astype(np.float32), 'F').resize((w, h), Image.BILINEAR), np.float32).clip(0, 1)
    Image.fromarray(np.clip(np.rint(person * 255), 0, 255).astype(np.uint8), 'L').save(os.path.join(d, 'person.png'))
    faces = []
    for f, m in all_faces(im):
        if not m['valid'] or m['xy'].shape[0] < 478: continue
        xy = m['xy'].astype(np.float64)
        fw = float(np.hypot(*(xy[454] - xy[234])))
        dxy = xy[454] - xy[234]; dz = float(m['z'][454] - m['z'][234])
        yaw = math.degrees(math.asin(max(-1, min(1, dz / max(1e-6, math.sqrt(dxy @ dxy + dz * dz))))))
        faces.append(dict(fw=fw, pts=[float(v) for k in ANCHOR_MESH for v in (xy[k][0] / w, xy[k][1] / h)],
                          iod=float(np.hypot(*(xy[473] - xy[468]))) / max(w, h), yaw=yaw))
    faces.sort(key=lambda f: -f['fw'])
    for n, f in enumerate(faces, 1): f['index'] = n; del f['fw']
    pose = None
    det = detect_pose(im)
    if det:
        pl = pose_landmarks(im, det)
        if pl['flag'] > 0.5:
            pose = dict(score=det['score'], flag=pl['flag'], pts=[float(v) for x, y, vis in pl['pts'] for v in (x / w, y / h, vis)])
    meta = dict(name=name, src=src.replace('\\', '/'), w=W0, h=H0, plane_w=w, plane_h=h, faces=faces, pose=pose)
    json.dump(meta, open(os.path.join(d, 'meta.json'), 'w'), indent=1)
    dr = im.copy(); g = ImageDraw.Draw(dr)
    for f in faces:
        for k in range(54):
            x, y = f['pts'][2 * k] * w, f['pts'][2 * k + 1] * h
            g.ellipse((x - 2, y - 2, x + 2, y + 2), fill=(255, 0, 0) if k < 36 else (0, 255, 0))
    if pose:
        P = [(pose['pts'][3 * k] * w, pose['pts'][3 * k + 1] * h, pose['pts'][3 * k + 2]) for k in range(33)]
        for a, b in SKEL:
            col = (0, 200, 255) if min(P[a][2], P[b][2]) >= 0.5 else (140, 140, 140)
            g.line((P[a][0], P[a][1], P[b][0], P[b][1]), fill=col, width=3)
    dr.save(os.path.join(OUT, name + '_pts.jpg'), quality=88)
    print(name, 'faces', len(faces), 'pose', None if not pose else round(pose['flag'], 2), 'yaw', [round(f['yaw'], 1) for f in faces])


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for n in (sys.argv[1:] or NAMES): process(n)
