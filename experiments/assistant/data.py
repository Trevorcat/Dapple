"""Image sets and per-image preparation (session, auto, analysis, AI masks, facts), cached under build/assistant/cache/<id>/."""
import glob, json, os, re, time
import numpy as np
from PIL import Image

import engine, vision, tools

ROOT = engine.ROOT
OUT = os.path.join(ROOT, 'build', 'assistant')
CACHE = os.path.join(OUT, 'cache')
RAWEXT = ('.arw', '.cr2', '.cr3', '.nef', '.raf', '.rw2', '.orf', '.pef', '.dng')


def image_sets():
    raws = sorted(p for p in glob.glob(os.path.join(ROOT, 'build', 'rawsamples', '**', '*'), recursive=True) if p.lower().endswith(RAWEXT))
    ports = sorted(glob.glob(os.path.join(ROOT, 'build', 'ai-eval', 'portraits', '*.jpg')))
    fk = os.path.join(OUT, 'fivek', 'subset.txt')
    five = []
    if os.path.exists(fk):
        for n in open(fk).read().split():
            p = os.path.join(OUT, 'fivek', 'dng', n + '.dng')
            if os.path.exists(p) and os.path.exists(os.path.join(OUT, 'fivek', 'thmb_c', n + '.jpg')): five.append(p)
    return {'raw': usable(raws), 'portrait': usable(ports), 'fivek': usable(five)}


def image_id(path):
    rel = os.path.relpath(path, ROOT).replace('\\', '/')
    rel = re.sub(r'^build/(rawsamples/|ai-eval/portraits/|assistant/fivek/dng/)', lambda m: {'rawsamples/': 'raw_', 'ai-eval/portraits/': 'por_', 'assistant/fivek/dng/': 'fk_'}[m.group(1)], rel)
    return re.sub(r'[^A-Za-z0-9_.-]+', '_', rel)


_stocks = None


def stocks():
    global _stocks
    if _stocks is None: _stocks = engine.film_stocks()
    return _stocks


class Prepared:
    """Everything the arms need for one photo. Keeps the engine session open (close() when done)."""

    def __init__(self, path, work_edge=1536, preview_edge=1024):
        self.path, self.id = path, image_id(path)
        self.dir = os.path.join(CACHE, self.id); os.makedirs(self.dir, exist_ok=True)
        t0 = time.time()
        self.s = engine.Session(path, work_edge)
        self.info = self.s.info()
        self.auto = self.s.auto(0.6)
        self.corr = self.auto['correction']
        self.tone = self.s.analyze() or {}
        self.preview_edge = preview_edge
        neutral = self.s.render({}, 1024)
        self.planes, self.faces = vision.compute_masks(neutral, self.dir)
        pid = 'ai:' + self.id[:40]
        self.rasters = {}
        for key, plane in self.planes.items():
            if key == 'depth': continue
            if plane.astype(np.float32).mean() < 255 * 0.005: continue
            rid = f'{pid}:{key}'; engine.set_raster(rid, plane); self.rasters[key] = rid
        if self.faces['faces'] and 'faceskin' in self.rasters: self.rasters['face'] = self.rasters.pop('faceskin')
        else: self.rasters.pop('faceskin', None)
        self.stocks = stocks(); self.stocks_by_id = {s['id']: s for s in self.stocks}
        self.auto_edit = tools.empty_edit()
        self.auto_img = self.render(self.auto_edit)[0]
        Image.fromarray(self.auto_img).save(os.path.join(self.dir, 'auto.jpg'), quality=92)
        self.m_auto = vision.measure(self.auto_img, self.planes, self.faces)
        self.prep_s = time.time() - t0

    def params(self, e, scale=None):
        return tools.to_params(e, self.corr, self.stocks_by_id, self.rasters, scale)

    def render(self, e, edge=None, scale=None):
        p, notes = self.params(e, scale)
        return self.s.render(p, edge or self.preview_edge), notes

    def facts(self, e=None, m=None, pinned=()):
        state = 'automatic edit (all offsets 0)' if e is None else 'after your offsets: ' + tools.describe(e)
        return vision.facts_text(m or self.m_auto, self.info, self.tone, pinned, state)

    def features(self, m=None):
        """Scene features for the taste predictor (arm B) and the synthetic photographers."""
        m = m or self.m_auto
        h = vision.hour_of(self.info.get('timestamp'))
        return {'L50': m['L_p50'], 'L5': m['L_p5'], 'L95': m['L_p95'], 'cast_a': m['cast_a'], 'cast_b': m['cast_b'], 'C_mean': m['C_mean'],
                'sky': m.get('sky_frac', 0.0), 'faces': len(m.get('faces', [])), 'person': m.get('person_frac', 0.0),
                'clip_hi': m['clip_hi'], 'face_L': (m['faces'][0]['L'] if m.get('faces') else 60.0), 'iso': float(self.info.get('iso') or 0), 'night': float(m['L_p50'] < 28 and m['L_p95'] < 75)}

    def close(self): self.s.close()


def usable(paths):
    """Drop files whose preparation failed (recorded in features.json, e.g. a FiveK DNG LibRaw cannot unpack)."""
    fp = os.path.join(OUT, 'features.json')
    bad = {k for k, v in json.load(open(fp)).items() if 'error' in v} if os.path.exists(fp) else set()
    return [p for p in paths if image_id(p) not in bad]
