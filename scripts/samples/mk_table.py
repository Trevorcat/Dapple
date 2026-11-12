import json, sys, statistics as st
rs=[json.loads(l) for l in open('build/rawsweep/out/results.jsonl',encoding='utf-8')]
def f(v,n=0):
    return '-' if v in (None,'') else (f'{v:.{n}f}' if isinstance(v,(int,float)) else str(v))
out=[]
out.append('| file | result | sensor | size (decoded -> 1600 px out) | black / white | as-shot WB R,B | engine CCT K / duv | camera or DNG-profile CCT K | decode ms (full / half) | dE76 / dE(ab) / dL vs preview | corner:centre engine / preview | DNG opcodes |')
out.append('|---|---|---|---|---|---|---|---|---|---|---|---|')
for r in rs:
    c=r.get('cmp',{})
    name=r['rel']
    err=r.get('engine_error') or r.get('crash')
    res='**FAIL** '+err[:70] if err else ('ok' + (' (no embedded preview)' if 'preview_error' in r else ''))
    sens=r.get('cfa') or ('mono' if r.get('lr_colors')==1 else '?')
    if r.get('lr_raw_count',1)>1: sens+=f" ({r['lr_raw_count']} frames in file)"
    size=f"{r.get('sensor_w','-')}x{r.get('sensor_h','-')} -> {r.get('out_w','-')}x{r.get('out_h','-')}" if not err else '-'
    bl=f"{r.get('lr_black',0)}" + (f" cb{r['lr_cblack'][4:6]}" if r.get('lr_black',0)==0 and any(r.get('lr_cblack',[0]*6)[:4]) is False and r.get('lr_cblack',[0]*6)[4] else '')
    bl=f"{r.get('lr_black')}/{r.get('lr_maximum')}" if not err else f"{r.get('lr_black')}/{r.get('lr_maximum')}"
    wb=f"{r['as_shot_wb'][0]:.3f},{r['as_shot_wb'][2]:.3f}" if r.get('as_shot_wb') else '-'
    ec = f"{r['eng_cct']:.0f} / {r.get('eng_duv',0):+.4f}" if r.get('eng_cct') else ('unknown' if not err else '-')
    cam = f"{r['cam_cct']:.0f} ({r['cam_cct_src'].replace('dng-colormatrix-pair','DNG dual-matrix').replace('preset-9','Fine-weather preset')}, duv {r.get('cam_duv',0):+.4f})" if r.get('cam_cct') and 'dng' in r['cam_cct_src'] else (f"{r['cam_cct']:.0f} (WB preset 'Fine weather')" if r.get('cam_cct') else '-')
    dec=f"{r.get('decode_full_ms',0):.0f} / {r.get('decode_preview_ms',0):.0f}" if r.get('decode_preview_ms') else (f"{r.get('decode_full_ms',0):.0f} / -")
    cm=f"{c['mean_dE76']} / {c['mean_dE_ab']} / {c['mean_dL']:+.1f}" if c else '-'
    cc=f"{c['corner_centre_engine']} / {c['corner_centre_preview']}" if c else (f"{'-'}")
    ops=', '.join(r.get('dng',{}).get('opcodes',[])).replace('OpcodeList','OL').replace('GainMap+GainMap+GainMap+GainMap','4xGainMap') if r.get('dng') else ''
    out.append(f"| {name} | {res} | {sens} | {size} | {bl} | {wb} | {ec} | {cam} | {dec} | {cm} | {cc} | {ops} |")
open('build/rawsweep/table.md','w',encoding='utf-8').write('\n'.join(out)+'\n')
# aggregates
g={}
for r in rs:
    c=r.get('cmp')
    if not c: continue
    mk=r['rel'].split('/')[0]
    g.setdefault(mk,[]).append(c['mean_dL'])
for k,v in sorted(g.items()): print(k, len(v), round(st.mean(v),1))
