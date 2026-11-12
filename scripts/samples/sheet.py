"""sheet.py <out_dir> : contact sheets (engine output | embedded preview) from results.jsonl. python -I"""
import json, os, sys
from PIL import Image, ImageDraw
out = sys.argv[1]
recs = [json.loads(l) for l in open(os.path.join(out, "results.jsonl"), encoding="utf-8")]
recs = [r for r in recs if os.path.exists(os.path.join(out, r["tag"] + ".jpg"))]
W, H = 330, 220
per_sheet, cols = 12, 2
for si in range(0, len(recs), per_sheet):
    chunk = recs[si:si + per_sheet]
    rows = (len(chunk) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (2 * W + 8), rows * (H + 18)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, r in enumerate(chunk):
        x0, y0 = (i % cols) * (2 * W + 8), (i // cols) * (H + 18)
        for k, suffix in enumerate(("", "_prev")):
            p = os.path.join(out, r["tag"] + suffix + ".jpg")
            if os.path.exists(p):
                im = Image.open(p).convert("RGB")
                im.thumbnail((W, H))
                sheet.paste(im, (x0 + k * W + (W - im.width) // 2, y0 + 16 + (H - im.height) // 2))
        d.text((x0 + 2, y0 + 2), r["rel"].split("/", 1)[1][:75] + "   [engine | camera preview]", fill=(255, 255, 0))
    sheet.save(os.path.join(out, "sheet_%d.jpg" % (si // per_sheet + 1)), quality=88)
    print("sheet", si // per_sheet + 1, len(chunk))
