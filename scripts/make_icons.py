"""Generate the launcher icons for Android and iOS from one square source image.

Usage: python -I scripts/make_icons.py <source.png>
The source is kept as design/icon/dapple_icon.png. Android gets legacy mipmaps plus an adaptive icon (foreground scaled into the safe zone on the
image's own background colour); iOS gets every size in AppIcon.appiconset, without alpha.
"""
import os
import re
import sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = Image.open(sys.argv[1]).convert('RGB')
side = min(src.size)
src = src.crop(((src.width - side) // 2, (src.height - side) // 2, (src.width - side) // 2 + side, (src.height - side) // 2 + side))

# background colour: median of the four corners
px = [src.getpixel(p) for p in ((4, 4), (side - 5, 4), (4, side - 5), (side - 5, side - 5))]
bg = tuple(sorted(c[i] for c in px)[1] for i in range(3))
hexbg = '#%02X%02X%02X' % bg

os.makedirs(os.path.join(ROOT, 'design', 'icon'), exist_ok=True)
src.save(os.path.join(ROOT, 'design', 'icon', 'dapple_icon.png'))

res = os.path.join(ROOT, 'app', 'android', 'app', 'src', 'main', 'res')
dens = {'mdpi': 1, 'hdpi': 1.5, 'xhdpi': 2, 'xxhdpi': 3, 'xxxhdpi': 4}
for name, k in dens.items():
    d = os.path.join(res, 'mipmap-' + name)
    os.makedirs(d, exist_ok=True)
    src.resize((round(48 * k), round(48 * k)), Image.LANCZOS).save(os.path.join(d, 'ic_launcher.png'))
    # adaptive foreground: 108 dp canvas, the artwork (which fills about 70 % of the source) scaled so it stays inside the 66 dp safe circle
    n = round(108 * k)
    fg = Image.new('RGB', (n, n), bg)
    m = round(n * 0.80)
    fg.paste(src.resize((m, m), Image.LANCZOS), ((n - m) // 2, (n - m) // 2))
    fg.save(os.path.join(d, 'ic_launcher_foreground.png'))

os.makedirs(os.path.join(res, 'mipmap-anydpi-v26'), exist_ok=True)
with open(os.path.join(res, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@color/ic_launcher_background"/>\n'
            '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n</adaptive-icon>\n')
with open(os.path.join(res, 'values', 'ic_launcher_background.xml'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_background">%s</color>\n</resources>\n' % hexbg)

ios = os.path.join(ROOT, 'app', 'ios', 'Runner', 'Assets.xcassets', 'AppIcon.appiconset')
for fn in os.listdir(ios):
    m = re.match(r'Icon-App-([\d.]+)x[\d.]+@(\d)x\.png$', fn)
    if m:
        n = round(float(m.group(1)) * int(m.group(2)))
        src.resize((n, n), Image.LANCZOS).save(os.path.join(ios, fn))
print('background', hexbg)
