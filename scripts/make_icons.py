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

# ---- mark with transparent background (petals + dot), monochrome layer, launch screens ----
import math

def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))

CREAM, ORANGE = (0xFC, 0xF1, 0xE2), (0xFF, 0xAB, 0x7A)
big = src.resize((1024, 1024), Image.LANCZOS)
mark = Image.new('RGBA', big.size)
mono = Image.new('RGBA', big.size)
mp, np_, sp = mark.load(), mono.load(), big.load()
dc, do = dist(CREAM, bg), dist(ORANGE, bg)
x0 = y0 = 1024
x1 = y1 = 0
for y in range(1024):
    for x in range(1024):
        p = sp[x, y]
        col, ref = (CREAM, dc) if dist(p, CREAM) <= dist(p, ORANGE) else (ORANGE, do)
        a = max(0.0, min(1.0, (dist(p, bg) / ref - 0.12) / 0.76))
        ai = round(a * 255)
        mp[x, y] = (col[0], col[1], col[2], ai)
        np_[x, y] = (255, 255, 255, ai)
        if ai > 24:
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
mark.save(os.path.join(ROOT, 'design', 'icon', 'dapple_mark.png'))

# monochrome layer for themed icons: same placement as the adaptive foreground
for name, k in dens.items():
    n = round(108 * k)
    m = round(n * 0.80)
    layer = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    layer.alpha_composite(mono.resize((m, m), Image.LANCZOS), ((n - m) // 2, (n - m) // 2))
    layer.save(os.path.join(res, 'mipmap-' + name, 'ic_launcher_monochrome.png'))
with open(os.path.join(res, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@color/ic_launcher_background"/>\n'
            '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
            '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome"/>\n</adaptive-icon>\n')

# launch mark: the artwork cropped square around its bounding box
cx, cy, half = (x0 + x1) / 2, (y0 + y1) / 2, max(x1 - x0, y1 - y0) / 2 * 1.04
crop = mark.crop((round(cx - half), round(cy - half), round(cx + half), round(cy + half)))
for name, k in dens.items():
    d = os.path.join(res, 'drawable-' + name)
    os.makedirs(d, exist_ok=True)
    n = round(144 * k)
    crop.resize((n, n), Image.LANCZOS).save(os.path.join(d, 'launch_mark.png'))
launch = ('<?xml version="1.0" encoding="utf-8"?>\n<layer-list xmlns:android="http://schemas.android.com/apk/res/android">\n'
          '    <item android:drawable="@color/ic_launcher_background" />\n'
          '    <item>\n        <bitmap android:gravity="center" android:src="@drawable/launch_mark" />\n    </item>\n</layer-list>\n')
for d in ('drawable', 'drawable-v21'):
    with open(os.path.join(res, d, 'launch_background.xml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(launch)
# Android 12+: the system splash shows an icon on a colour; use the adaptive foreground on the same colour
for d, parent in (('values-v31', '@android:style/Theme.Light.NoTitleBar'), ('values-night-v31', '@android:style/Theme.Black.NoTitleBar')):
    os.makedirs(os.path.join(res, d), exist_ok=True)
    with open(os.path.join(res, d, 'styles.xml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                '    <style name="LaunchTheme" parent="%s">\n'
                '        <item name="android:windowBackground">@drawable/launch_background</item>\n'
                '        <item name="android:windowSplashScreenBackground">@color/ic_launcher_background</item>\n'
                '        <item name="android:windowSplashScreenAnimatedIcon">@mipmap/ic_launcher_foreground</item>\n'
                '    </style>\n'
                '    <style name="NormalTheme" parent="%s">\n'
                '        <item name="android:windowBackground">?android:colorBackground</item>\n'
                '    </style>\n</resources>\n' % (parent, parent))

# iOS launch screen: the mark on the same colour
li = os.path.join(ROOT, 'app', 'ios', 'Runner', 'Assets.xcassets', 'LaunchImage.imageset')
for fn, k in (('LaunchImage.png', 1), ('LaunchImage@2x.png', 2), ('LaunchImage@3x.png', 3)):
    crop.resize((168 * k, 168 * k), Image.LANCZOS).save(os.path.join(li, fn))
sb = os.path.join(ROOT, 'app', 'ios', 'Runner', 'Base.lproj', 'LaunchScreen.storyboard')
t = open(sb, encoding='utf-8').read()
t = re.sub(r'<color key="backgroundColor" red="[\d.]+" green="[\d.]+" blue="[\d.]+" alpha="1" colorSpace="custom" customColorSpace="sRGB"/>',
           '<color key="backgroundColor" red="%.4f" green="%.4f" blue="%.4f" alpha="1" colorSpace="custom" customColorSpace="sRGB"/>' % tuple(c / 255 for c in bg), t)
t = re.sub(r'<image name="LaunchImage" width="\d+" height="\d+"/>', '<image name="LaunchImage" width="168" height="168"/>', t)
open(sb, 'w', encoding='utf-8', newline='\n').write(t)
print('mark bbox', x0, y0, x1, y1)
