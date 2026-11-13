import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *

def lng(im, n):
    w, h = im.size; s = n / max(w, h); return im.resize((round(w * s), round(h * s)), Image.BILINEAR)

# name -> (runtime, relative path, input_fn(im) -> list of numpy arrays, note)
def i_u2netp(im):  return [prep(im, (320, 320), IMN_M, IMN_S)]
def i_isnet(im):   return [prep(im, (1024, 1024), np.array([.5, .5, .5], np.float32), np.array([1, 1, 1], np.float32))]
def i_modnet(im):  return [prep(im, (512, 512), np.float32(.5), np.float32(.5))]
def i_dav2(im):    return [prep(im, (518, 518), IMN_M, IMN_S)]
def i_midas(im):   return [prep(im, (256, 256), IMN_M, IMN_S)]
def i_biref(im):   return [prep(im, (1024, 1024), IMN_M, IMN_S)]
def i_mobsam_enc(im): return [np.asarray(lng(im, 1024), np.float32)]
def i_slim_enc(im):
    a = np.asarray(lng(im, 1024), np.float32) / 255.0; a = (a - IMN_M) / IMN_S
    p = np.zeros((1024, 1024, 3), np.float32); p[:a.shape[0], :a.shape[1]] = a
    return [p.transpose(2, 0, 1)[None].copy()]
def i_nhwc01(n): return lambda im: [prep(im, (n, n), scale=255.0, nchw=False)]
def i_deeplab(im): return [prep(im, (257, 257), np.float32(127.5), np.float32(127.5), scale=1.0, nchw=False)]
def i_hair(im):
    a = prep(im, (512, 512), scale=255.0, nchw=False); return [np.concatenate([a, np.zeros((1, 512, 512, 1), np.float32)], -1)]
def i_blaze(im): return [prep(im, (128, 128), np.float32(127.5), np.float32(127.5), scale=1.0, nchw=False)]
def i_mesh(im):  return [prep(im, (256, 256), scale=255.0, nchw=False)]
def i_mt_enc(im): return [prep(im, (768, 768), scale=255.0, nchw=False)]

M = {
 'u2netp':            ('onnx', 'onnx/u2netp.onnx', i_u2netp),
 'isnet-general-use': ('onnx', 'onnx/isnet-general-use.onnx', i_isnet),
 'modnet_fp32':       ('onnx', 'onnx/modnet_fp32.onnx', i_modnet),
 'modnet_q8':         ('onnx', 'onnx/modnet_q8.onnx', i_modnet),
 'dav2s_fp32':        ('onnx', 'onnx/dav2s_fp32.onnx', i_dav2),
 'dav2s_q8':          ('onnx', 'onnx/dav2s_q8.onnx', i_dav2),
 'midas_small':       ('onnx', 'onnx/midas_small.onnx', i_midas),
 'birefnet_lite_fp32':('onnx', 'onnx/birefnet_lite_fp32.onnx', i_biref),
 'mobilesam_enc':     ('onnx', 'onnx/mobilesam_enc.onnx', i_mobsam_enc),
 'slimsam_enc':       ('onnx', 'onnx/slimsam_enc.onnx', i_slim_enc),
 'slimsam_enc_q8':    ('onnx', 'onnx/slimsam_enc_q8.onnx', i_slim_enc),
 'mp_selfie_multiclass_256': ('tflite', 'mp/selfie_multiclass_256x256.tflite', i_nhwc01(256)),
 'mp_selfie_segmenter_256':  ('tflite', 'mp/selfie_segmenter.tflite', i_nhwc01(256)),
 'mp_hair_512':       ('tflite', 'mp/hair_segmenter.tflite', i_hair),
 'mp_deeplabv3_257':  ('tflite', 'mp/deeplab_v3.tflite', i_deeplab),
 'mp_blazeface_128':  ('tflite', 'mp_x/face_landmarker__face_detector.tflite', i_blaze),
 'mp_facemesh_256':   ('tflite', 'mp_x/face_landmarker__face_landmarks_detector.tflite', i_mesh),
 'mp_magictouch_enc_768': ('tflite', 'mp_x/magic_touch__interactive_segmentation_encoder.int8.tflite', i_mt_enc),
}

def make_runner(name, threads):
    rt, rel, fn = M[name]; p = os.path.join(MD, rel)
    if rt == 'onnx':
        s = ort_session(p, threads); names = [i.name for i in s.get_inputs()]
        return lambda xs: s.run(None, dict(zip(names, xs)))
    it = tfl(p, threads)
    return lambda xs: tfl_run(it, xs)
