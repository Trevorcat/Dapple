"""ctypes binding of the Dapple C API (core/include/dapple/dapple_c.h) used as the tool executor of the assistant experiment.
Uses a SNAPSHOT of the desktop build in build/assistant/bin (copied from build/check-desktop/bin; x64 libomp/libc++ from the toolchain).
Mirrors what the app does (app/lib/state/app_state.dart wireParams): engine auto correction (minus `exposure`) + the user's wire params,
numeric keys added."""
import ctypes, json, os, threading
import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BIN = os.path.join(ROOT, 'build', 'assistant', 'bin')
FILM_PACK = os.path.join(ROOT, 'app', 'assets', 'film', 'dapple.dfp')


class DImage(ctypes.Structure):
    _fields_ = [('width', ctypes.c_int32), ('height', ctypes.c_int32), ('rgba', ctypes.POINTER(ctypes.c_uint8))]


_lib = None
_lock = threading.Lock()


def lib():
    global _lib
    if _lib is None:
        os.add_dll_directory(BIN)
        L = ctypes.CDLL(os.path.join(BIN, 'libdapple.dll'))
        vp, cp, i32, f32 = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.c_float
        for n, res, args in [
            ('dapple_last_error', cp, []), ('dapple_free_string', None, [vp]), ('dapple_free_image', None, [ctypes.POINTER(DImage)]),
            ('dapple_session_open', vp, [cp, i32, i32]), ('dapple_session_close', None, [vp]),
            ('dapple_session_info_json', vp, [vp]), ('dapple_session_auto_json', vp, [vp, f32]),
            ('dapple_session_analyze_json', vp, [vp]), ('dapple_session_analyze_params_json', vp, [vp, cp]),
            ('dapple_session_render', i32, [vp, cp, cp, i32, ctypes.POINTER(DImage)]),
            ('dapple_film_load_pack', i32, [cp]), ('dapple_film_list_json', vp, []), ('dapple_looks_json', vp, []),
            ('dapple_mask_raster_set', i32, [cp, i32, i32, ctypes.POINTER(ctypes.c_uint8), i32]),
            ('dapple_mask_raster_has', i32, [cp]), ('dapple_params_schema_json', vp, []),
            ('dapple_gpu_set_enabled', i32, [i32]), ('dapple_gpu_status_json', vp, []),
        ]:
            f = getattr(L, n); f.restype = res; f.argtypes = args
        if L.dapple_film_load_pack(FILM_PACK.encode()) != 0:
            raise RuntimeError('film pack: ' + L.dapple_last_error().decode())
        _lib = L
    return _lib


def _str(p):
    if not p: return None
    s = ctypes.cast(p, ctypes.c_char_p).value.decode('utf-8', 'replace')
    lib().dapple_free_string(p)
    return s


def film_stocks():
    d = json.loads(_str(lib().dapple_film_list_json()))
    return [s for s in d['stocks'] if s.get('type') in ('negative', 'slide')]


def set_raster(mid, mask_u8):
    m = np.ascontiguousarray(mask_u8, dtype=np.uint8)
    h, w = m.shape
    r = lib().dapple_mask_raster_set(mid.encode(), w, h, m.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)), w)
    if r != 0: raise RuntimeError('raster: ' + lib().dapple_last_error().decode())


class Session:
    def __init__(self, path, long_edge=2048):
        self.path = path
        self.h = lib().dapple_session_open(path.encode('utf-8'), 0, long_edge)
        if not self.h: raise RuntimeError(f'open {path}: ' + lib().dapple_last_error().decode())

    def close(self):
        if self.h: lib().dapple_session_close(self.h); self.h = None

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def info(self): return json.loads(_str(lib().dapple_session_info_json(self.h)))
    def auto(self, strength=0.6): return json.loads(_str(lib().dapple_session_auto_json(self.h, strength)))
    def analyze(self, params=None):
        if params is None: return json.loads(_str(lib().dapple_session_analyze_json(self.h)) or 'null')
        return json.loads(_str(lib().dapple_session_analyze_params_json(self.h, json.dumps(params).encode())) or 'null')

    def render(self, params, long_edge=1024):
        img = DImage()
        r = lib().dapple_session_render(self.h, json.dumps(params).encode(), b'', long_edge, ctypes.byref(img))
        if r != 0: raise RuntimeError('render: ' + lib().dapple_last_error().decode())
        a = np.ctypeslib.as_array(img.rgba, shape=(img.height, img.width, 4)).copy()
        lib().dapple_free_image(ctypes.byref(img))
        return a[..., :3]


def wire(correction, user):
    """app_state.dart wireParams: auto correction (without legacy `exposure`) as the base, user's numeric keys added."""
    out = {k: v for k, v in (correction or {}).items() if k != 'exposure'}
    for k, v in user.items():
        b = out.get(k)
        if isinstance(v, (int, float)) and isinstance(b, (int, float)) and not isinstance(v, bool): out[k] = b + v
        else: out[k] = v
    return out
