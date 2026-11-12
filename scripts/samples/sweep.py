"""sweep.py <samples_root> <probe.exe> <out_dir> : run rawprobe on every sample (one process at a time), add DNG opcode info and the
colour sanity check against the embedded preview, write <out_dir>/results.jsonl.  Run with python -I (inputs are untrusted data)."""
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

root, probe, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
EXT = {".arw", ".raf", ".dng", ".cr2", ".cr3", ".nef", ".rw2", ".orf", ".pef", ".srw", ".iiq", ".3fr", ".erf", ".nrw"}


def srgb_to_lab(a):
    a = a.astype(np.float64) / 255.0
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]])
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def luma(a):
    a = a.astype(np.float64) / 255.0
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return lin @ np.array([0.2126, 0.7152, 0.0722])


def corner_ratio(img):
    y = luma(img)
    h, w = y.shape
    ch, cw = int(h * 0.12), int(w * 0.12)
    corners = np.mean([y[:ch, :cw].mean(), y[:ch, -cw:].mean(), y[-ch:, :cw].mean(), y[-ch:, -cw:].mean()])
    c = y[int(h * 0.4):int(h * 0.6), int(w * 0.4):int(w * 0.6)].mean()
    return float(corners / c) if c > 1e-6 else 0.0


def compare(eng_path, prev_path):
    e = Image.open(eng_path).convert("RGB")
    p = Image.open(prev_path).convert("RGB")
    res = {"aspect_engine": round(e.width / e.height, 4), "aspect_preview": round(p.width / p.height, 4)}
    L = 320
    s = L / max(e.width, e.height)
    size = (max(1, round(e.width * s)), max(1, round(e.height * s)))
    ea = np.asarray(e.resize(size, Image.BOX))
    pa = np.asarray(p.resize(size, Image.BOX))  # preview squeezed onto the engine's frame when the crops differ
    res["aspect_mismatch"] = abs(res["aspect_engine"] / res["aspect_preview"] - 1) > 0.03
    le, lp = srgb_to_lab(ea), srgb_to_lab(pa)
    res["mean_dE76"] = round(float(np.sqrt(((le - lp) ** 2).sum(-1)).mean()), 2)
    # chroma-only distance (brightness-independent): how far apart the colours are once L is ignored
    res["mean_dE_ab"] = round(float(np.sqrt(((le[..., 1:] - lp[..., 1:]) ** 2).sum(-1)).mean()), 2)
    res["mean_luma_engine"] = round(float(ea.mean()), 1)
    res["mean_luma_preview"] = round(float(pa.mean()), 1)
    res["mean_dL"] = round(float((le[..., 0] - lp[..., 0]).mean()), 2)
    res["mean_ab_engine"] = [round(float(le[..., 1].mean()), 2), round(float(le[..., 2].mean()), 2)]
    res["mean_ab_preview"] = [round(float(lp[..., 1].mean()), 2), round(float(lp[..., 2].mean()), 2)]
    res["corner_centre_engine"] = round(corner_ratio(ea), 3)
    res["corner_centre_preview"] = round(corner_ratio(pa), 3)
    return res


files = []
for d, _, fs in os.walk(root):
    for f in fs:
        if os.path.splitext(f)[1].lower() in EXT:
            files.append(os.path.join(d, f))
files.sort()
only = sys.argv[4:]  # optional substrings to restrict
if only:
    files = [f for f in files if any(o in f for o in only)]
resf = open(os.path.join(out, "results.jsonl"), "a" if only else "w", encoding="utf-8")
for path in files:
    rel = os.path.relpath(path, root).replace("\\", "/")
    tag = rel.replace("/", "__").rsplit(".", 1)[0]
    t0 = time.time()
    rec = {"rel": rel, "tag": tag, "size": os.path.getsize(path)}
    try:
        p = subprocess.run([probe, path, out, tag], capture_output=True, timeout=600)
        rec["exit"] = p.returncode
        line = p.stdout.decode("utf-8", "replace").strip().splitlines()
        if line:
            try:
                rec.update(json.loads(line[-1]))
            except Exception as ex:
                rec["probe_json_error"] = str(ex)
        if p.returncode != 0:
            rec["crash"] = "exit code %d; stderr: %s" % (p.returncode, p.stderr.decode("utf-8", "replace")[-300:])
    except subprocess.TimeoutExpired:
        rec["crash"] = "timeout 600 s"
    rec["wall_s"] = round(time.time() - t0, 1)
    if os.path.splitext(path)[1].lower() == ".dng":
        q = subprocess.run(["python", "-I", os.path.join(here, "dng_tags.py"), path], capture_output=True, timeout=120)
        try:
            rec["dng"] = json.loads(q.stdout.decode())
        except Exception as ex:
            rec["dng"] = {"error": "dng_tags failed: %s" % ex}
    ep, pp = os.path.join(out, tag + ".jpg"), os.path.join(out, tag + "_prev.jpg")
    if os.path.exists(ep) and os.path.exists(pp):
        try:
            rec["cmp"] = compare(ep, pp)
        except Exception as ex:
            rec["cmp_error"] = str(ex)
    resf.write(json.dumps(rec) + "\n")
    resf.flush()
    print(rel, "ok" if "engine_error" not in rec and "crash" not in rec else "ERR", rec["wall_s"], flush=True)
