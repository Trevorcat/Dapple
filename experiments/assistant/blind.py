"""Blind contact sheet for the owner (metric vi, the decisive one).
  python -I experiments/assistant/blind.py build <arm>=<config>/<model> [...]   e.g. A=A/engine L=C_if1/qwen3vl-4b F=F_A/qwen3vl-4b
  python -I experiments/assistant/blind.py serve [port]      # open http://127.0.0.1:8765/ ; votes are written to build/assistant/blind/votes.json
  python -I experiments/assistant/blind.py tally             # votes + key.json -> per-arm wins
Writes build/assistant/blind/{index.html, img/, key.json (answer key: open only after voting), sheet.png (static, columns in the same
random order per row, labelled 1..N only)}."""
import http.server, json, os, random, shutil, sys
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')), 'build', 'assistant')
BL = os.path.join(OUT, 'blind')

# 24 varied photos of the general set (no near-duplicate files, no colour-chart frames)
PICK = ['raw_Apple_iPhone-12-Pro_IMG_1361.DNG', 'raw_Apple_iPhone-XS_IMG_1105.dng', 'raw_Canon_EOS-6D-Mark-II_6DII-RAW.CR2',
        'raw_Canon_EOS-R10_IMG_4361.CR3', 'raw_Canon_EOS-R5_Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3',
        'raw_Canon_EOS-R6-Mark-II_canon-r6m2-img_0346-raw-normal-full.cr3', 'raw_Fujifilm_X-A7_DSCF1595.RAF', 'raw_Fujifilm_X-T50_DSCF0118.RAF',
        'raw_Google_Pixel-6-Pro_PXL_20220910_093206982.dng', 'raw_Google_Pixel-7-Pro_PXL_20221204_063131635.dng',
        'raw_Google_Pixel-8-Pro_PXL_20240415_103400204.RAW-02.ORIGINAL.dng', 'raw_Leica_M10_f5381888.dng', 'raw_Leica_Q2_L1000750.DNG',
        'raw_Nikon_D850_Nikon-D850-14bit-lossless-compressed.NEF', 'raw_Nikon_Z-8_Nikon_Z8_raw_14_bit_lossless_compression.NEF',
        'raw_OnePlus_A6003_IMG_20201019_093659.dng', 'raw_Panasonic_DC-GH6_1-1.RW2', 'raw_Pentax_K-1-Mark-II__IMG2390.DNG',
        'raw_Samsung_Galaxy-S23_20230715_154550.dng', 'raw_Sony_ILCE-7RM4_DSC00396.ARW', 'raw_Xiaomi_M2010J19CG_IMG_20201224_141323.dng',
        'por_p2_dark_skin_dreads.jpg', 'por_p3_curly_glasses_outdoor.jpg', 'por_p8_three_girls_busy_bg.jpg']

PAGE = r"""<!doctype html><html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Blind sheet</title><style>
:root{--bg:#f6f6f4;--fg:#1d1d1f;--mut:#6e6e73;--acc:#0a66d8;--bad:#c62828;--card:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#eee;--mut:#999;--acc:#5aa2ff;--bad:#ff6b6b;--card:#1c1c1e}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 -apple-system,system-ui,"PingFang SC","Microsoft YaHei",sans-serif}
header{position:sticky;top:0;background:var(--bg);padding:10px 16px;border-bottom:1px solid #8883;display:flex;gap:16px;align-items:center;flex-wrap:wrap;z-index:2}
header b{font-size:17px}.mut{color:var(--mut)}button{font:inherit;padding:6px 12px;border-radius:8px;border:1px solid #8886;background:var(--card);color:var(--fg);cursor:pointer}
#grid{display:grid;gap:10px;padding:12px 16px}.cell{background:var(--card);border-radius:10px;padding:6px;border:3px solid transparent;cursor:pointer;position:relative}
.cell img{width:100%;display:block;border-radius:6px}.cell .n{position:absolute;top:12px;left:12px;background:#000a;color:#fff;border-radius:6px;padding:2px 9px;font-weight:600}
.cell.best{border-color:var(--acc)}.cell.worst{border-color:var(--bad)}.tag{position:absolute;top:12px;right:12px;border-radius:6px;padding:2px 8px;color:#fff;font-weight:600}
.best .tag{background:var(--acc)}.worst .tag{background:var(--bad)}#zoom{position:fixed;inset:0;background:#000e;display:none;z-index:5;align-items:center;justify-content:center}
#zoom img{max-width:100vw;max-height:100vh}</style></head><body>
<header><b id="pos"></b><span class="mut">数字键 1-N = 最好；Shift+数字 = 最差（可选）；0 = 差不多；←/→ 翻页；Z 放大当前悬停；结果自动保存</span>
<button onclick="go(-1)">上一张</button><button onclick="go(1)">下一张</button><button onclick="dl()">下载结果 JSON</button><span id="st" class="mut"></span></header>
<div id="grid"></div><div id="zoom" onclick="this.style.display='none'"><img id="zimg"></div>
<script>
const D=__DATA__;let i=0,hover=null;let V={};try{V=JSON.parse(localStorage.getItem('blindVotes')||'{}')}catch(e){}
function save(){try{localStorage.setItem('blindVotes',JSON.stringify(V))}catch(e){}
fetch('save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(V)}).then(r=>{st.textContent=r.ok?'已保存 votes.json':'仅保存在浏览器'}).catch(()=>{st.textContent='仅保存在浏览器（用“下载结果”）'})}
function render(){const it=D[i];const n=it.imgs.length;grid.style.gridTemplateColumns=`repeat(${innerWidth<700?1:Math.min(n,2)},1fr)`;
pos.textContent=`${i+1} / ${D.length}`+(V[it.row]?'  ✓':'');grid.innerHTML='';const v=V[it.row]||{};
it.imgs.forEach((src,k)=>{const c=document.createElement('div');c.className='cell'+(v.best===k+1?' best':'')+(v.worst===k+1?' worst':'');
c.innerHTML=`<img src="${src}"><span class="n">${k+1}</span>`+(v.best===k+1?'<span class="tag">最好</span>':v.worst===k+1?'<span class="tag">最差</span>':'');
c.onclick=()=>vote(k+1,false);c.onmouseenter=()=>hover=src;grid.appendChild(c)})}
function vote(k,worst){const it=D[i];const v=V[it.row]||{};if(k===0){v.best=0}else if(worst){v.worst=v.worst===k?undefined:k}else{v.best=k}
v.t=Date.now();V[it.row]=v;save();render()}
function go(d){i=Math.max(0,Math.min(D.length-1,i+d));render();scrollTo(0,0)}
function dl(){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(V,null,1)],{type:'application/json'}));a.download='votes.json';a.click()}
addEventListener('keydown',e=>{if(e.key==='ArrowRight')go(1);else if(e.key==='ArrowLeft')go(-1);else if(e.key==='z'&&hover){zimg.src=hover;zoom.style.display='flex'}
else if(e.key==='Escape')zoom.style.display='none';else{const m=e.code.match(/^Digit(\d)$/);if(m){const k=+m[1];if(k<=D[i].imgs.length)vote(k,e.shiftKey)}}});
addEventListener('resize',render);render();
</script></body></html>"""


def build(specs, seed=20261114):
    arms = dict(s.split('=', 1) for s in specs)
    if os.path.exists(BL): shutil.rmtree(BL)
    os.makedirs(os.path.join(BL, 'img'))
    rng = random.Random(seed); rows, key = [], {}
    for r, iid in enumerate(PICK):
        srcs = {a: os.path.join(OUT, 'renders', *cm.split('/'), iid + '.jpg') for a, cm in arms.items()}
        if not all(os.path.exists(p) for p in srcs.values()):
            print('skip (missing render)', iid, [a for a, p in srcs.items() if not os.path.exists(p)]); continue
        order = list(arms); rng.shuffle(order); imgs = []
        for k, a in enumerate(order):
            name = f'{r:02d}_{rng.getrandbits(32):08x}.jpg'
            shutil.copy(srcs[a], os.path.join(BL, 'img', name)); imgs.append('img/' + name)
        rid = f'r{r:02d}'; rows.append({'row': rid, 'imgs': imgs}); key[rid] = {'photo': iid, 'order': order}
    open(os.path.join(BL, 'index.html'), 'w', encoding='utf-8').write(PAGE.replace('__DATA__', json.dumps(rows)))
    json.dump({'arms': arms, 'rows': key}, open(os.path.join(BL, 'key.json'), 'w'), indent=1)
    sheet(rows)
    print('built', len(rows), 'rows ->', os.path.join(BL, 'index.html'))


def sheet(rows, cell=420):
    n = max(len(r['imgs']) for r in rows); pad = 8; lab = 26
    H = len(rows) * (cell * 2 // 3 + pad + lab) + pad
    S = Image.new('RGB', (n * (cell + pad) + pad + 50, H), (246, 246, 244)); d = ImageDraw.Draw(S)
    try: font = ImageFont.truetype('arial.ttf', 20)
    except Exception: font = ImageFont.load_default()
    y = pad
    for r in rows:
        d.text((8, y + 40), r['row'][1:], fill=(0, 0, 0), font=font)
        for k, src in enumerate(r['imgs']):
            im = Image.open(os.path.join(BL, src)); im.thumbnail((cell, cell * 2 // 3))
            x = 50 + pad + k * (cell + pad); S.paste(im, (x, y + lab)); d.text((x, y + 2), str(k + 1), fill=(0, 0, 0), font=font)
        y += cell * 2 // 3 + pad + lab
    S.save(os.path.join(BL, 'sheet.png'))


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=BL, **k)
    def do_POST(self):
        if self.path.rstrip('/').endswith('save'):
            n = int(self.headers.get('Content-Length', 0)); body = self.rfile.read(n)
            json.loads(body); open(os.path.join(BL, 'votes.json'), 'wb').write(body)
            self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
        else: self.send_response(404); self.end_headers()


def tally():
    key = json.load(open(os.path.join(BL, 'key.json'))); votes = json.load(open(os.path.join(BL, 'votes.json')))
    best, worst, ties = {}, {}, 0
    for rid, v in votes.items():
        order = key['rows'][rid]['order']
        if v.get('best'): best[order[v['best'] - 1]] = best.get(order[v['best'] - 1], 0) + 1
        elif v.get('best') == 0: ties += 1
        if v.get('worst'): worst[order[v['worst'] - 1]] = worst.get(order[v['worst'] - 1], 0) + 1
    print(json.dumps(dict(best=best, worst=worst, ties=ties, rows_voted=len(votes)), indent=1))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'build': build(a[1:])
    elif a[0] == 'serve':
        port = int(a[1]) if len(a) > 1 else 8765
        print(f'http://127.0.0.1:{port}/'); http.server.ThreadingHTTPServer(('127.0.0.1', port), H).serve_forever()
    elif a[0] == 'tally': tally()
