"""Shared helpers for the AI-model evaluation (research tooling, not product code).
Run with: tools/py-ai/Scripts/python.exe -I scripts/ai-eval/<script>.py
"""
import os, sys, time, json
import numpy as np
from PIL import Image
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MD = os.path.join(ROOT, 'tools', '_dl', 'ai-models')
SRC = os.path.join(ROOT, 'build', 'ai-eval', 'src')
OUT = os.path.join(ROOT, 'build', 'ai-eval')
IMAGES = ['person', 'fruit', 'sunset', 'baretree', 'statue', 'trees']
IMN_M = np.array([0.485, 0.456, 0.406], np.float32)
IMN_S = np.array([0.229, 0.224, 0.225], np.float32)

def load(name):
    return Image.open(os.path.join(SRC, name + '.jpg')).convert('RGB')

def prep(im, size, mean=None, std=None, scale=255.0, nchw=True):
    """resize to size=(w,h) bilinear, /scale, (x-mean)/std"""
    a = np.asarray(im.resize(size, Image.BILINEAR), np.float32) / scale
    if mean is not None: a = (a - mean) / std
    return a.transpose(2, 0, 1)[None].copy() if nchw else a[None].copy()

def sigmoid(x): return 1.0 / (1.0 + np.exp(-x))

def norm01(x):
    x = x.astype(np.float32); lo, hi = x.min(), x.max()
    return (x - lo) / max(hi - lo, 1e-9)

def mask_to_img(m, size):
    """m float 0..1 HxW -> PIL L at size (bilinear)"""
    return Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8)).resize(size, Image.BILINEAR)

def overlay(im, m, color=(255, 0, 80), alpha=0.55):
    """m: PIL L mask at im size"""
    a = np.asarray(im, np.float32); mm = np.asarray(m, np.float32)[..., None] / 255.0
    c = np.array(color, np.float32)
    return Image.fromarray((a * (1 - alpha * mm) + c * alpha * mm).clip(0, 255).astype(np.uint8))

def ort_session(path, threads):
    import onnxruntime as ort
    so = ort.SessionOptions()
    so.intra_op_num_threads = threads; so.inter_op_num_threads = 1
    so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    so.log_severity_level = 3
    return ort.InferenceSession(path, so, providers=['CPUExecutionProvider'])

def tfl(path, threads):
    from ai_edge_litert.interpreter import Interpreter
    it = Interpreter(model_path=path, num_threads=threads); it.allocate_tensors(); return it

def tfl_run(it, inputs):
    for d, x in zip(it.get_input_details(), inputs): it.set_tensor(d['index'], x.astype(d['dtype']))
    it.invoke()
    return [it.get_tensor(d['index']) for d in it.get_output_details()]
