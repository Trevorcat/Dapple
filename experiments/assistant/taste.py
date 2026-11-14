"""Personalisation: two synthetic photographers (rules + noise), their edit histories, the non-LLM taste predictor (arm B: kNN over
scene features) and the compact taste profile given to the VLM (arm D). The real test needs the app to export the history file whose
schema is in docs/research/ai-assistant-prototype.md (section 'Taste profile export'); `history` rows here use the same schema."""
import copy, json, random
import numpy as np

import tools

FEATS = ['L50', 'L5', 'L95', 'cast_a', 'cast_b', 'C_mean', 'sky', 'faces', 'person', 'clip_hi', 'night']
SCALE = {'L50': 15, 'L5': 10, 'L95': 10, 'cast_a': 4, 'cast_b': 6, 'C_mean': 8, 'sky': 0.15, 'faces': 0.7, 'person': 0.2, 'clip_hi': 0.02, 'night': 0.7}


def scene_type(f):
    if f['faces'] > 0 or f['person'] > 0.08: return 'portrait'
    if f['night'] > 0.5: return 'night'
    if f['sky'] > 0.08: return 'landscape'
    return 'other'


def clampi(v, lim): return int(max(-lim, min(lim, round(v))))


def rule_warm_film(f):
    """'暖调胶片' photographer: brighter, lifted shadows (low contrast), warm, Portra-style film with grain, lifts dark faces."""
    st = scene_type(f); e = tools.empty_edit()
    e['light'] = {'x': clampi(1 + (1 if f['L50'] < 45 else 0), 5), 'y': -2, 's': -1}
    e['color'] = {'x': clampi(2 + (1 if f['cast_b'] < -3 else 0), 5), 'y': -1 if st == 'portrait' else 0, 's': 0}
    e['texture'] = {'x': -1, 'y': 2, 's': 0}
    e['base'], e['base_strength'] = {'portrait': ('kodak_portra_400', 8), 'landscape': ('kodak_gold_200', 7), 'night': ('kodak_portra_400', 6)}.get(st, ('kodak_portra_400', 7))
    if st == 'portrait' and f['faces'] > 0 and f.get('face_L', 60) < 55: e['areas'] = [{'target': 'face', 'op': 'lift', 'amount': 2}]
    return e


def rule_clean_contrast(f):
    """'干净高反差' photographer: neutral white balance (cancels the measured cast), contrasty, crisp, clean modern rendering, darker skies."""
    st = scene_type(f); e = tools.empty_edit()
    e['light'] = {'x': -1 if f['L50'] > 65 else 0, 'y': clampi(2 + (1 if f['C_mean'] < 12 else 0), 5), 's': 1}
    e['color'] = {'x': clampi(-f['cast_b'] / 4.5, 3), 'y': 0 if st == 'portrait' else 1, 's': 0}
    e['texture'] = {'x': 2, 'y': -1, 's': 0}
    e['base'], e['base_strength'] = ('modern_vivid', 8) if st == 'landscape' else ('modern_clean', 10)
    e['hdr'] = 2 if (f['sky'] > 0.15 and f['clip_hi'] > 0.002) else 0
    if f['sky'] > 0.1: e['areas'] = [{'target': 'sky', 'op': 'darken', 'amount': 2}]
    return e


PHOTOGRAPHERS = {'warm_film': rule_warm_film, 'clean_contrast': rule_clean_contrast}
ALT_BASES = {'warm_film': ['kodak_gold_200', 'fujifilm_pro_400h', 'kodak_portra_160'], 'clean_contrast': ['modern_clean', 'modern_vivid', 'none']}


def noisy(e, who, rng, sigma=0.6, p_base=0.15):
    e = copy.deepcopy(e)
    for cat in ('light', 'color', 'texture'):
        for ax in ('x', 'y', 's'):
            lim = tools.MAXS if ax == 's' else tools.MAXSTEP
            e[cat][ax] = clampi(e[cat][ax] + rng.gauss(0, sigma), lim)
    if rng.random() < p_base: e['base'] = rng.choice(ALT_BASES[who])
    return e


def make_history(who, rows, seed=1):
    """rows: [(image_id, features)] -> history rows (export schema v1)."""
    rng = random.Random(seed * 1000 + sum(map(ord, who)))
    out = []
    for iid, f in rows:
        e = noisy(PHOTOGRAPHERS[who](f), who, rng)
        out.append({'photo': iid, 'scene': scene_type(f), 'facts': {k: round(float(f[k]), 4) for k in FEATS},
                    'pads': {c: e[c] for c in ('light', 'color', 'texture')}, 'base': e['base'], 'base_strength': e['base_strength'],
                    'hdr': e['hdr'], 'areas': e['areas'], 'kept': True})
    return out


def _vec(f): return np.array([float(f[k]) / SCALE[k] for k in FEATS])


def knn_predict(history, f, k=7):
    """Arm B: mean pad numbers of the k nearest past edits (scene features), majority base, areas used by >= half of them."""
    rows = [h for h in history if h.get('kept', True)]
    d = np.array([np.linalg.norm(_vec(h['facts']) - _vec(f)) for h in rows])
    nn = [rows[i] for i in np.argsort(d)[:k]]
    e = tools.empty_edit()
    for cat in ('light', 'color', 'texture'):
        for ax in ('x', 'y', 's'):
            e[cat][ax] = clampi(np.mean([h['pads'][cat][ax] for h in nn]), tools.MAXS if ax == 's' else tools.MAXSTEP)
    bases = [h['base'] for h in nn]; e['base'] = max(set(bases), key=bases.count)
    e['base_strength'] = int(round(np.median([h['base_strength'] for h in nn if h['base'] == e['base']])))
    e['hdr'] = int(round(np.mean([h['hdr'] for h in nn])))
    cnt = {}
    for h in nn:
        for a in h['areas']: cnt.setdefault((a['target'], a['op']), []).append(a['amount'])
    e['areas'] = [{'target': t, 'op': o, 'amount': int(np.median(v))} for (t, o), v in cnt.items() if len(v) >= (k + 1) // 2][:3]
    return e, nn


def profile_text(history, f, n_examples=5):
    """Arm D context (~200-300 tokens): typical offsets per scene type, favourite bases, nearest past edits as few-shot lines."""
    lines = []
    by = {}
    for h in history: by.setdefault(h['scene'], []).append(h)
    for st, hs in sorted(by.items()):
        med = {c: {ax: int(np.median([h['pads'][c][ax] for h in hs])) for ax in ('x', 'y', 's')} for c in ('light', 'color', 'texture')}
        bases = [h['base'] for h in hs]; fav = max(set(bases), key=bases.count)
        e = tools.empty_edit(); e.update(med); e['base'] = fav; e['base_strength'] = int(np.median([h['base_strength'] for h in hs]))
        ar = {}
        for h in hs:
            for a in h['areas']: ar[(a['target'], a['op'])] = ar.get((a['target'], a['op']), 0) + 1
        e['areas'] = [{'target': t, 'op': o, 'amount': 2} for (t, o), c in ar.items() if c >= len(hs) / 2]
        lines.append(f"- {st} ({len(hs)} edits): usually {tools.describe(e)}")
    _, nn = knn_predict(history, f, n_examples)
    lines.append('Most similar past photos and what this user did:')
    for h in nn:
        fx = h['facts']
        e = tools.empty_edit(); e.update({c: h['pads'][c] for c in ('light', 'color', 'texture')}); e['base'] = h['base']; e['base_strength'] = h['base_strength']; e['hdr'] = h['hdr']; e['areas'] = h['areas']
        lines.append(f"- {h['scene']}, median L* {fx['L50']:.0f}, cast b* {fx['cast_b']:+.0f}, sky {fx['sky'] * 100:.0f}%, faces {int(fx['faces'])}: {tools.describe(e)}")
    return '\n'.join(lines)


def edit_distance(a, b):
    """Pad L1 distance in steps (9 numbers + hdr) and whether the base matches."""
    va, vb = np.array(tools.pad_vector(a)), np.array(tools.pad_vector(b))
    steps = np.abs(va - vb) / np.array([tools.STEP] * 2 + [tools.S_STEP] + [tools.STEP] * 2 + [tools.S_STEP] + [tools.STEP] * 2 + [tools.S_STEP] + [0.1])
    return float(steps[:9].mean()), a['base'] == b['base']
