"""The tool surface of the assistant: one `apply_edit` call over the app's high-level parameters, as OFFSETS from the deterministic auto.
Pads follow app/lib/model/pad_model.dart padRecipe (ported 1:1 for light / colour / texture); integers are pad steps so the model emits
few, exact tokens. Pinned parameters are removed from the schema. Everything is clamped; the guards live in loop.py."""
import copy, json

STEP = 0.15          # one pad step (pad range -1..1); +-5 steps = +-0.75 of the pad
S_STEP = 0.1         # one strength step around the middle (0.5)
MAXSTEP, MAXS = 5, 3

REASONS = ['underexposed', 'overexposed', 'face_dark', 'face_bright', 'sky_bright', 'highlights_clipped', 'shadows_crushed', 'flat_contrast',
           'harsh_contrast', 'warm_cast', 'cool_cast', 'green_cast', 'magenta_cast', 'dull_colour', 'oversaturated', 'high_iso_noise',
           'keep_warm_mood', 'keep_cool_mood', 'portrait', 'landscape', 'night', 'backlit', 'high_key', 'low_key', 'already_good', 'user_taste']
TARGETS = ['face', 'person', 'background', 'sky', 'top_gradient']
OPS = ['lift', 'darken', 'warm', 'cool', 'pop', 'soften']
MODERN = ['modern_clean', 'modern_vivid', 'modern_soft']


def tone_strength_of(a, d=60.0):
    return d * a * 2 if a <= 0.5 else d + (100 - d) * (a * 2 - 1)


def pad_recipe(cat, x, y, a):
    boost, low = max(0.0, a * 2 - 1), min(1.0, a * 2)
    if cat == 'light':
        return {'exposure': x * 16, 'contrast': y * 55, 'highlights': -x * 40 - y * 15, 'shadows': x * 20 - y * 15, 'whites': y * 15,
                'blacks': -y * 15, 'toneStrength': tone_strength_of(a)}
    if cat == 'color':
        return {'temp': x * 30, 'tint': 0, 'vibrance': y * 55 + boost * 20, 'saturation': y * 25 + boost * 8, 'lookStrength': 100 * low}
    if cat == 'texture':
        k = a * 2 if a <= 0.5 else 1 + boost * 0.8
        clean = max(0.0, -y)
        return {'clarity': x * 40, 'texture': x * 20, 'sharpen': min(150.0, max(0.0, (40 + x * 40) * k)),
                'nrLuma': (clean * 60 + boost * 25) * (low if a <= 0.5 else 1), 'nrColor': min(100.0, max(0.0, (25 + clean * 40) * k)),
                'grain': max(0.0, y) * 45, 'sharpRadius': 10}
    raise KeyError(cat)


# engine defaults (core/src/edit_params.cpp; JSON domain) and the non-additive keys of EditParams::operator+=
DEFAULTS = {'exposure': 0, 'contrast': 0, 'highlights': 0, 'shadows': 0, 'whites': 0, 'blacks': 0, 'temp': 0, 'tint': 0, 'vibrance': 0,
            'saturation': 0, 'clarity': 0, 'texture': 0, 'sharpen': 40, 'sharpRadius': 10, 'nrLuma': 0, 'nrColor': 25, 'grain': 0, 'hdr': 0,
            'lookStrength': 100, 'toneStrength': 60, 'film_strength': 100}
ABSOLUTE = {'toneStrength', 'lookStrength', 'film_strength', 'sharpRadius', 'film_grain', 'film_halation', 'look', 'film', 'local'}


def empty_edit():
    return {'light': {'x': 0, 'y': 0, 's': 0}, 'color': {'x': 0, 'y': 0, 's': 0}, 'texture': {'x': 0, 'y': 0, 's': 0},
            'base': 'none', 'base_strength': 10, 'hdr': 0, 'areas': []}


def normalise(call, pinned=()):
    """Clamp a (possibly partial) apply_edit call into a full edit; pinned keys keep the auto value (0 / none)."""
    e = empty_edit()
    if not isinstance(call, dict): call = {}
    for cat in ('light', 'color', 'texture'):
        for ax in ('x', 'y', 's'):
            sub = call.get(cat)
            v = sub.get(ax, 0) if isinstance(sub, dict) else 0
            lim = MAXS if ax == 's' else MAXSTEP
            try: v = int(round(float(v)))
            except Exception: v = 0
            if f'{cat}.{ax}' in pinned or cat in pinned: v = 0
            e[cat][ax] = max(-lim, min(lim, v))
    if 'base' not in pinned:
        b = (call or {}).get('base', 'none')
        e['base'] = b if isinstance(b, str) else 'none'
        try: e['base_strength'] = max(0, min(10, int((call or {}).get('base_strength', 10))))
        except Exception: e['base_strength'] = 10
    if 'hdr' not in pinned:
        try: e['hdr'] = max(0, min(6, int((call or {}).get('hdr', 0))))
        except Exception: e['hdr'] = 0
    if 'areas' not in pinned:
        ar = []
        ar_in = call.get('areas')
        for it in (ar_in if isinstance(ar_in, list) else [])[:3]:
            if isinstance(it, dict) and it.get('target') in TARGETS and it.get('op') in OPS:
                try: amt = max(1, min(5, int(it.get('amount', 2))))
                except Exception: amt = 2
                ar.append({'target': it['target'], 'op': it['op'], 'amount': amt})
        e['areas'] = ar
    return e


def schema(stocks, pinned=(), semantic_only=False, with_reason=True):
    """JSON schema of the apply_edit arguments (llama.cpp turns it into a grammar). Pinned fields are deleted."""
    ints = lambda lo, hi: {'type': 'integer', 'minimum': lo, 'maximum': hi}
    props = {'reasons': {'type': 'array', 'items': {'type': 'string', 'enum': REASONS}, 'minItems': 1, 'maxItems': 4}}
    if not semantic_only:
        for cat in ('light', 'color', 'texture'):
            if cat in pinned: continue
            sub = {ax: ints(-(MAXS if ax == 's' else MAXSTEP), MAXS if ax == 's' else MAXSTEP) for ax in ('x', 'y', 's') if f'{cat}.{ax}' not in pinned}
            if sub: props[cat] = {'type': 'object', 'properties': sub, 'required': list(sub), 'additionalProperties': False}
    if 'base' not in pinned:
        props['base'] = {'type': 'string', 'enum': ['none'] + MODERN + [s['id'] for s in stocks]}
        props['base_strength'] = ints(3, 10)
    if 'hdr' not in pinned: props['hdr'] = ints(0, 6)
    if 'areas' not in pinned:
        props['areas'] = {'type': 'array', 'maxItems': 3, 'items': {'type': 'object', 'properties': {
            'target': {'type': 'string', 'enum': TARGETS}, 'op': {'type': 'string', 'enum': OPS}, 'amount': ints(1, 5)},
            'required': ['target', 'op', 'amount'], 'additionalProperties': False}}
    if with_reason: props['reason_zh'] = {'type': 'string', 'minLength': 8, 'maxLength': 90}
    return {'type': 'object', 'properties': props, 'required': list(props), 'additionalProperties': False}


def tool_doc(stocks, pinned=(), semantic_only=False):
    """What the model is told about the tool (kept short: ~350 tokens)."""
    t = ["Call apply_edit with OFFSETS from the automatic edit (0 = keep the automatic value). Integers are pad steps."]
    if not semantic_only:
        if 'light' not in pinned:
            t.append("light.x: darker(-) / brighter(+), 1 step ~ +0.12 EV with highlight protection. light.y: less(-) / more(+) contrast. "
                     "light.s: strength of the automatic tone correction (-3..+3).")
        if 'color' not in pinned:
            t.append("color.x: cooler(-) / warmer(+), 1 step ~ 4.5 temperature units. color.y: paler(-) / richer(+) colour. color.s: style strength / extra vividness.")
        if 'texture' not in pinned:
            t.append("texture.x: softer(-) / crisper(+) (clarity, sharpening). texture.y: cleaner(-, noise reduction) / grainier(+).")
    if 'base' not in pinned:
        t.append("base: rendering base. 'none' = standard; modern_clean 通透 (clean, full range), modern_vivid 鲜明 (rich, contrasty), modern_soft 柔和 (soft, gentle); "
                 "or a film stock (negative films: gentle, warm print look; slides: saturated, contrasty). base_strength 3..10 = 30..100 %.")
    if 'hdr' not in pinned: t.append("hdr 0..6: single-shot HDR (x10 %), only for scenes whose range exceeds the screen (bright sky + dark ground).")
    if 'areas' not in pinned:
        t.append("areas (max 3): local actions on detected regions: target face|person|background|sky|top_gradient, op lift|darken|warm|cool|pop|soften, amount 1..5. "
                 "Only use targets that the facts say exist.")
    t.append("reasons: 1-4 ids of what you saw. reason_zh: exactly two short Chinese sentences that explain the change using the measured facts; never invent numbers.")
    return '\n'.join(t)


def base_params(e, stocks_by_id):
    b = e['base']
    if b == 'none' or (b not in MODERN and b not in stocks_by_id): return {}
    return {'film': b, 'film_strength': 10 * e['base_strength']}


def area_items(e, rasters):
    """Local adjustment items (pipeline-architecture 40.1) for the area actions. rasters: {'face': id, 'person': id, 'background': id, 'sky': id}."""
    items, notes = [], []
    for i, ar in enumerate(e['areas']):
        t, op, k = ar['target'], ar['op'], ar['amount']
        if t == 'top_gradient':
            mask = [{'kind': 'linear', 'x0': 0.5, 'y0': 0.0, 'x1': 0.5, 'y1': 0.45}]
        else:
            rid = rasters.get(t)
            if not rid:
                notes.append(f'area {t} unavailable: skipped'); continue
            mask = [{'kind': 'raster', 'ref': rid, 'refine': 50}]
        adj = {
            'lift': {'exposure': 2.4 * k, 'shadows': 4 * k},
            'darken': {'exposure': -2.4 * k, 'highlights': -6 * k},
            'warm': {'temp': 5 * k},
            'cool': {'temp': -5 * k},
            'pop': {'clarity': 8 * k, 'vibrance': 5 * k, 'exposure': 0.8 * k},
            'soften': {'clarity': -8 * k, 'texture': -6 * k},
        }[op]
        if t in ('sky', 'top_gradient') and op == 'darken': adj['saturation'] = 3 * k
        items.append({'id': f'as{i}', 'name': f'{t}:{op}', 'on': True, 'strength': 100, 'mask': mask, 'adjust': {kk: round(v, 2) for kk, v in adj.items()}})
    return items, notes


def to_params(e, corr, stocks_by_id, rasters, scale=None):
    """Engine params JSON: auto correction + pad recipes (as deltas from the defaults for additive keys) + base + hdr + local items.
    scale: optional per-category multipliers applied to the pad offsets by the guards."""
    scale = scale or {}
    user = {'look': 'std'}
    for cat in ('light', 'color', 'texture'):
        f = scale.get(cat, 1.0)
        rec = pad_recipe(cat, e[cat]['x'] * STEP * f, e[cat]['y'] * STEP * f, 0.5 + e[cat]['s'] * S_STEP * f)
        user.update(rec)
    out = {k: v for k, v in (corr or {}).items() if k != 'exposure'}
    for k, v in user.items():
        if k in ABSOLUTE or not isinstance(v, (int, float)): out[k] = v
        else: out[k] = out.get(k, DEFAULTS.get(k, 0)) + (v - DEFAULTS.get(k, 0))
    bp = base_params(e, stocks_by_id)
    if 'film_strength' in bp: bp['film_strength'] *= scale.get('base', 1.0)
    out.update(bp)
    out['hdr'] = out.get('hdr', 0) + 10 * e['hdr'] * scale.get('hdr', 1.0)
    items, notes = area_items(e, rasters) if scale.get('areas', 1.0) > 0 else ([], [])
    if items: out['local'] = {'v': 1, 'items': items}
    return out, notes


def pad_vector(e):
    """Numbers of an edit for distances: 9 pad offsets in pad units + hdr fraction."""
    v = []
    for cat in ('light', 'color', 'texture'):
        v += [e[cat]['x'] * STEP, e[cat]['y'] * STEP, e[cat]['s'] * S_STEP]
    return v + [e['hdr'] / 10]


def describe(e):
    parts = []
    for cat in ('light', 'color', 'texture'):
        p = e[cat]
        if any(p.values()): parts.append(f"{cat} x{p['x']:+d} y{p['y']:+d} s{p['s']:+d}")
    if e['base'] != 'none': parts.append(f"base {e['base']} {e['base_strength'] * 10}%")
    if e['hdr']: parts.append(f"hdr {e['hdr'] * 10}")
    for a in e['areas']: parts.append(f"{a['target']}:{a['op']}{a['amount']}")
    return ', '.join(parts) or 'no change'
