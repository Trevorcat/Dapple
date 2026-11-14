"""Side-by-side sheet: python -I compare.py out.jpg <config/model> ... -- <ids...> (first column is always arm A)."""
import os, sys
from PIL import Image, ImageDraw, ImageFont
OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'assistant'))
a = sys.argv[1:]; out = a[0]; k = a.index('--'); cols = ['A/engine'] + a[1:k]; ids = a[k + 1:]
W = 360; H = 260; S = Image.new('RGB', (W * len(cols), (H + 20) * len(ids) + 20), 'white'); d = ImageDraw.Draw(S)
try: font = ImageFont.truetype('arial.ttf', 14)
except Exception: font = ImageFont.load_default()
for c, cm in enumerate(cols): d.text((c * W + 4, 2), cm, fill='black', font=font)
for r, i in enumerate(ids):
    for c, cm in enumerate(cols):
        p = os.path.join(OUT, 'renders', *cm.split('/'), i + '.jpg')
        if os.path.exists(p):
            im = Image.open(p); im.thumbnail((W - 4, H)); S.paste(im, (c * W + 2, 20 + r * (H + 20)))
    d.text((4, 20 + r * (H + 20) + H + 2), i[:60], fill='black', font=font)
S.save(os.path.join(OUT, out), quality=88)
