import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
from models import lng
from PIL import ImageDraw
DISP = 1024
os.makedirs(os.path.join(OUT, 'raw'), exist_ok=True)
def save(model, name, im, m):
    d = os.path.join(OUT, model); os.makedirs(d, exist_ok=True)
    mi = mask_to_img(m, im.size); mi.save(os.path.join(OUT, 'raw', f'{model}__{name}.png'))
    v = im.copy(); v.thumbnail((DISP, DISP)); mv = mi.resize(v.size, Image.BILINEAR)
    o = overlay(v, mv); o.save(os.path.join(d, name + '.png')); return o
def save_tap(model, name, im, m, tap, iou=None):
    o = save(model, name, im, m); dr = ImageDraw.Draw(o); x, y = tap[0] * o.width, tap[1] * o.height
    dr.ellipse([x - 9, y - 9, x + 9, y + 9], outline=(255, 255, 0), width=3); o.save(os.path.join(OUT, model, name + '.png')); return o
def sheet(model, tiles, cols=3):
    rows = (len(tiles) + cols - 1) // cols
    W = Image.new('RGB', (cols * 480, rows * 360), 'white'); dr = ImageDraw.Draw(W)
    for k, (n, o) in enumerate(tiles):
        t = o.copy(); t.thumbnail((476, 340)); x, y = (k % cols) * 480, (k // cols) * 360; W.paste(t, (x, y)); dr.text((x + 3, y + 3), n, fill='yellow')
    W.save(os.path.join(OUT, f'sheet_{model}.jpg'), quality=88)
