"""Fast colour guided filter prototype (He & Sun 2015) in numpy.  Research prototype, not product code."""
import numpy as np
from PIL import Image

def box(x, r):
    """mean over (2r+1)^2 window, edge-normalised; x: HxW or HxWxC"""
    k = 2 * r + 1
    def ax(a, axis):
        c = np.cumsum(a, axis=axis, dtype=np.float64)
        pad = [(0, 0)] * a.ndim; pad[axis] = (1, 0); c = np.pad(c, pad)
        n = a.shape[axis]
        hi = np.minimum(np.arange(n) + r + 1, n); lo = np.maximum(np.arange(n) - r, 0)
        sl = lambda idx: np.take(c, idx, axis=axis)
        cnt = (hi - lo).astype(np.float64); shape = [1] * a.ndim; shape[axis] = n
        return (sl(hi) - sl(lo)) / cnt.reshape(shape)
    return ax(ax(x, 0), 1).astype(np.float32)

def guided_color(I, p, r, eps):
    """I: HxWx3 float 0..1, p: HxW float. Returns q."""
    h, w, _ = I.shape
    mI = box(I, r); mp = box(p, r)
    mIp = box(I * p[..., None], r); covIp = mIp - mI * mp[..., None]
    var = np.empty((h, w, 3, 3), np.float32)
    for i in range(3):
        for j in range(i, 3):
            v = box(I[..., i] * I[..., j], r) - mI[..., i] * mI[..., j]; var[..., i, j] = v; var[..., j, i] = v
    var += eps * np.eye(3, dtype=np.float32)
    a = np.linalg.solve(var, covIp[..., None])[..., 0]
    b = mp - (a * mI).sum(-1)
    return a, b

def fast_guided(I, p, r=8, eps=1e-3, s=4):
    """I full-res guide, p full-res (already upsampled) mask. Solve at 1/s resolution, apply a,b at full res."""
    h, w, _ = I.shape
    if s > 1:
        sw, sh = w // s, h // s
        Is = np.asarray(Image.fromarray((I * 255).astype(np.uint8)).resize((sw, sh), Image.BILINEAR), np.float32) / 255
        ps = np.asarray(Image.fromarray(p.astype(np.float32), 'F').resize((sw, sh), Image.BILINEAR))
        a, b = guided_color(Is, ps, max(r // s, 1), eps)
        a = np.stack([np.asarray(Image.fromarray(a[..., k], 'F').resize((w, h), Image.BILINEAR)) for k in range(3)], -1)
        b = np.asarray(Image.fromarray(b, 'F').resize((w, h), Image.BILINEAR))
    else:
        a, b = guided_color(I, p, r, eps)
    return np.clip((a * I).sum(-1) + b, 0, 1)
