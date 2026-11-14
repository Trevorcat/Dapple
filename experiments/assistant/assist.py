"""The assistant loop: propose -> render -> (show the new preview + facts) -> refine, at most 3 rounds, with deterministic guards."""
import copy, json, re, time
import numpy as np

import llm, tools, vision

SYSTEM = """You are the photo assistant of a RAW editor. An automatic correction has already set exposure and tone; you look at the photo and decide small, tasteful changes that make THIS photo its best version, by calling apply_edit.
Rules: values are OFFSETS from the automatic edit, 0 = keep. Prefer small moves (1-2 steps); use 3-5 only when the photo clearly needs it. Trust the measured facts for brightness, clipping and colour cast; use your eyes for content and mood (sunset warmth, night, portrait). Never brighten a face that is already bright; protect highlights. Use local areas only for regions the facts say exist. Output only the JSON arguments of apply_edit, compact on one line.

Tool apply_edit:
"""


def build_messages(P, mode, schema_doc, taste_text=None, pinned=(), img_edge=768, unconstrained_schema=None, base_note=None):
    content = []
    if 'image' in mode: content.append({'type': 'image_url', 'image_url': {'url': 'data:image/jpeg;base64,' + llm.jpeg_b64(P.auto_img, img_edge)}})
    txt = []
    if 'facts' in mode: txt.append('Measured facts of the automatic edit:\n' + P.facts(pinned=pinned))
    else: txt.append('No measurements are available; judge from the photo.' if 'image' in mode else '')
    if taste_text: txt.append('This user\'s editing taste (from their past edits; follow it unless the photo clearly needs otherwise):\n' + taste_text)
    if base_note: txt.append(base_note)
    if unconstrained_schema: txt.append('Answer with one JSON object matching this JSON schema:\n' + json.dumps(unconstrained_schema))
    txt.append('Call apply_edit now: return its JSON arguments.')
    content.append({'type': 'text', 'text': '\n\n'.join(t for t in txt if t)})
    return [{'role': 'system', 'content': SYSTEM + schema_doc}, {'role': 'user', 'content': content}]


def parse(txt):
    try: return json.loads(txt), True
    except Exception:
        m = re.search(r'\{.*\}', txt or '', re.S)
        if m:
            try: return json.loads(m.group(0)), False
            except Exception: pass
    return None, False


def validate(obj, schema):
    """Strict check of a call against the schema (enum, ranges, required, extra keys). Returns list of problems."""
    probs = []
    def chk(v, s, path):
        t = s.get('type')
        if t == 'object':
            if not isinstance(v, dict): probs.append(path + ' not object'); return
            for k in s.get('required', []):
                if k not in v: probs.append(f'{path}.{k} missing')
            for k, vv in v.items():
                if k not in s['properties']: probs.append(f'{path}.{k} extra')
                else: chk(vv, s['properties'][k], f'{path}.{k}')
        elif t == 'array':
            if not isinstance(v, list): probs.append(path + ' not array'); return
            if len(v) > s.get('maxItems', 99) or len(v) < s.get('minItems', 0): probs.append(path + ' length')
            for i, x in enumerate(v): chk(x, s['items'], f'{path}[{i}]')
        elif t == 'integer':
            if not isinstance(v, int) or isinstance(v, bool) or v < s.get('minimum', -1e9) or v > s.get('maximum', 1e9): probs.append(f'{path}={v!r} bad int')
        elif t == 'string':
            if not isinstance(v, str): probs.append(path + ' not string'); return
            if 'enum' in s and v not in s['enum']: probs.append(f'{path}={v!r} not in enum')
            if len(v) > s.get('maxLength', 1e9): probs.append(path + ' too long')
    if obj is None: return ['unparseable']
    chk(obj, schema, '$')
    return probs

# ------------------------------------------------------------------------------------------------ guards


def guard(P, e, scale=None, max_iter=3):
    """Render, check against the automatic edit's numbers, shrink the offending offsets (not re-asked). Returns (img, measures, scale, events)."""
    scale = dict(scale or {}); events = []
    ma = P.m_auto
    for it in range(max_iter + 1):
        img, notes = P.render(e, scale=scale)
        m = vision.measure(img, P.planes, P.faces)
        viol = []
        for fa, fb in zip(m.get('faces', []), ma.get('faces', [])):
            if (fa['p95'] > 97 and fa['p95'] > fb['p95'] + 1) or fa['clip'] > max(0.02, fb['clip'] + 0.01): viol.append('face_clip'); break
        if m['clip_hi'] > ma['clip_hi'] + 0.015: viol.append('highlights')
        if m['clip_lo'] > ma['clip_lo'] + 0.03: viol.append('shadows')
        if m['C_p90'] > max(ma['C_p90'] * 1.35, ma['C_p90'] + 12): viol.append('chroma')
        if e['base'] == 'none' and (abs(m['cast_b'] - ma['cast_b']) > 12 or abs(m['cast_a'] - ma['cast_a']) > 8): viol.append('wb_shift')
        if not viol or it == max_iter: return img, m, scale, events + notes
        events.append(f'round{it}:' + '+'.join(viol))
        if 'face_clip' in viol:
            scale['light'] = scale.get('light', 1) * 0.5
            if any(a['target'] in ('face', 'person') and a['op'] in ('lift', 'pop') for a in e['areas']): scale['areas'] = 0
        if 'highlights' in viol or 'shadows' in viol: scale['light'] = scale.get('light', 1) * 0.5; scale['hdr'] = scale.get('hdr', 1) * 0.5
        if 'chroma' in viol or 'wb_shift' in viol: scale['color'] = scale.get('color', 1) * 0.5
        if it >= 1 and e['base'] != 'none': scale['base'] = scale.get('base', 1) * 0.5   # the base itself moves white / chroma
        if it >= 2: scale['areas'] = 0
    return img, m, scale, events


def effective(e, scale):
    """The edit after the guards' scaling (pad offsets as floats in steps)."""
    f = copy.deepcopy(e)
    for cat in ('light', 'color', 'texture'):
        for ax in ('x', 'y', 's'): f[cat][ax] = e[cat][ax] * scale.get(cat, 1.0)
    f['hdr'] = e['hdr'] * scale.get('hdr', 1.0)
    f['base_strength'] = e['base_strength'] * scale.get('base', 1.0)
    if scale.get('areas', 1.0) == 0: f['areas'] = []
    return f

# ------------------------------------------------------------------------------------------------ one assistant run


def run(client, P, mode='image+facts', rounds=1, temperature=0.0, seed=7, taste_text=None, pinned=(), semantic_only=False,
        numbers_from=None, constrained=True, img_edge=768):
    """Returns a record with the final edit, per-round calls, costs, guard events. numbers_from: edit whose pad numbers are used
    when semantic_only (arm F: from A = zeros or B = taste predictor)."""
    sch = tools.schema(P.stocks, pinned, semantic_only)
    doc = tools.tool_doc(P.stocks, pinned, semantic_only)
    note = None
    if semantic_only: note = 'The light / colour / texture numbers are set by the engine (not by you); choose only the rendering base, hdr and local areas.'
    msgs = build_messages(P, mode, doc, taste_text, pinned, img_edge, None if constrained else sch, note)
    rec = dict(mode=mode, rounds=[], temperature=temperature, semantic_only=semantic_only, constrained=constrained)
    e_prev = None
    for r in range(rounds):
        txt, cost = client.chat(msgs, sch if constrained else None, temperature, seed + r)
        obj, clean = parse(txt)
        probs = validate(obj, sch)
        e = tools.normalise(obj or {}, pinned)
        if semantic_only and numbers_from is not None:
            for cat in ('light', 'color', 'texture'): e[cat] = dict(numbers_from[cat])
        img, m, scale, events = guard(P, e)
        rec['rounds'].append(dict(raw=txt, valid=not probs, problems=probs[:5], edit=e, scale=scale, guards=events, cost=cost,
                                  reasons=(obj or {}).get('reasons', []), reason_zh=(obj or {}).get('reason_zh', '')))
        rec.update(edit=e, eff=effective(e, scale), img=img, m=m, scale=scale)
        if e_prev is not None and tools.describe(e) == tools.describe(e_prev): break
        e_prev = e
        if r + 1 < rounds:
            msgs.append({'role': 'assistant', 'content': txt})
            content = []
            if 'image' in mode: content.append({'type': 'image_url', 'image_url': {'url': 'data:image/jpeg;base64,' + llm.jpeg_b64(img, img_edge)}})
            fb = 'This is the photo after your edit' + (' (the guards reduced: ' + ', '.join(events) + ')' if events else '') + '.'
            if 'facts' in mode: fb += '\nMeasured facts now:\n' + vision.facts_text(m, P.info, P.tone, pinned, 'after your offsets: ' + tools.describe(e))
            fb += '\nIf it is right, return the same arguments with reasons ["already_good"]. Otherwise return revised arguments: TOTAL offsets from the automatic edit (not increments).'
            content.append({'type': 'text', 'text': fb})
            msgs.append({'role': 'user', 'content': content})
    rec['n_rounds'] = len(rec['rounds'])
    rec['reason_zh'] = rec['rounds'][-1]['reason_zh']; rec['reasons'] = rec['rounds'][-1]['reasons']
    return rec

# ------------------------------------------------------------------------------------------------ rationale check (metric iv)


def check_rationale(text, ids, m):
    """Claims in the Chinese reason / reason ids vs the measured facts. Returns dict(claims, wrong, details)."""
    nf = len(m.get('faces', [])); people = nf > 0 or m.get('person_frac', 0) > 0.03
    sky = m.get('sky_frac', 0) > 0.02
    dark = m['L_p50'] < 40 or any(f['L'] < 42 for f in m.get('faces', []))
    bright = m['L_p50'] > 66 or m['clip_hi'] > 0.02
    cw = vision.cast_word(m['cast_a'], m['cast_b'])
    claims, wrong, det = 0, 0, []
    def c(name, said, ok):
        nonlocal claims, wrong
        if said:
            claims += 1
            if not ok: wrong += 1; det.append(name)
    t = text or ''
    c('people', bool(re.search('人脸|面部|脸|人物|肤色|皮肤|人像', t)) or 'portrait' in ids or 'face_dark' in ids or 'face_bright' in ids, people)
    c('sky', '天空' in t or 'sky_bright' in ids, sky)
    c('dark', bool(re.search('偏暗|欠曝|暗了|太暗|较暗|昏暗', t)) or 'underexposed' in ids, dark or m['L_p50'] < 48)
    c('bright', bool(re.search('偏亮|过曝|太亮|过亮', t)) or 'overexposed' in ids, bright or m['L_p50'] > 60)
    c('warm_cast', bool(re.search('偏暖|偏黄|发黄|暖色调偏', t)) or 'warm_cast' in ids, 'warm' in cw)
    c('cool_cast', bool(re.search('偏冷|偏蓝|发蓝', t)) or 'cool_cast' in ids, 'cool' in cw)
    c('green_cast', bool(re.search('偏绿|发绿', t)) or 'green_cast' in ids, 'green' in cw)
    c('magenta_cast', bool(re.search('偏紫|偏品|偏洋红', t)) or 'magenta_cast' in ids, 'magenta' in cw)
    c('clipped', bool(re.search('高光溢出|过曝溢出|死白|溢出', t)) or 'highlights_clipped' in ids, m['clip_hi'] > 0.005 or m.get('sky_clip', 0) > 0.01)
    c('night', '夜' in t or 'night' in ids, m['L_p50'] < 45)
    sents = [s for s in re.split('[。！？!?]', t) if s.strip()]
    return dict(claims=claims, wrong=wrong, details=det, sentences=len(sents), chinese=bool(re.search('[一-鿿]', t)))
