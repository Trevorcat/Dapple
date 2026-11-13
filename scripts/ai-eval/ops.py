import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *
import onnx, collections, glob
sets = {}
for f in ['dav2s_fp32', 'mobilesam_enc', 'mobilesam_dec', 'u2netp', 'modnet_fp32', 'midas_small', 'slimsam_enc', 'slimsam_dec', 'isnet-general-use']:
    m = onnx.load(os.path.join(MD, 'onnx', f + '.onnx'), load_external_data=False)
    sets[f] = collections.Counter(n.op_type for n in m.graph.node); print(f, 'opset', [o.version for o in m.opset_import if o.domain in ('', 'ai.onnx')], len(sets[f]), 'op types')
chosen = ['dav2s_fp32', 'mobilesam_enc', 'mobilesam_dec', 'u2netp', 'modnet_fp32']
u = set().union(*[set(sets[k]) for k in chosen]); print('UNION of chosen (%d):' % len(u), sorted(u))
from ai_edge_litert.interpreter import Interpreter
t = set()
for f in glob.glob(os.path.join(MD, 'mp', '*.tflite')) + glob.glob(os.path.join(MD, 'mp_x', '*.tflite')):
    it = Interpreter(model_path=f); t |= {d['op_name'] for d in it._get_ops_details()}
print('TFLITE union of all MediaPipe models (%d):' % len(t), sorted(t))
