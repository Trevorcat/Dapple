"""Pushes the core/ai test binary, runtime libraries, model directory and reference data to the Redmi (/data/local/tmp/dapple-ai/t only) and runs it.
  phone_ai_tests.py [--images a,b] [--only face,person,...] [--threads N] [--bin build/ai-android/dapple_ai_tests]
Needs: build/ai-android (cmake, see core/ai/CMakeLists.txt), tools/ai-rt/ai-models (model directory), build/ai-eval/ai-ref (scripts/ai-eval/ai_ref.py)."""
import sys, os, argparse; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phone_run import adb, push_if_new, ROOT, DEV
T = DEV + '/t'
ap = argparse.ArgumentParser(); ap.add_argument('--images', default='person,fruit,p1_glasses_flag,p4_profile_bw,p9_three_people_frontal'); ap.add_argument('--only', default='pil,face,person,tap,sam,depth,api')
ap.add_argument('--threads', default='0'); ap.add_argument('--extra', default=''); ap.add_argument('--bin', default='build/ai-android/dapple_ai_tests'); ap.add_argument('--ort', default='tools/ai-rt/ort-build-min2/MinSizeRel/libonnxruntime.so')
a = ap.parse_args()
adb('shell', f'mkdir -p {T}/models {T}/ref')
adb('push', os.path.join(ROOT, a.bin), f'{T}/dapple_ai_tests')   # always: it changes with every build
adb('shell', f'chmod 755 {T}/dapple_ai_tests')
push_if_new(os.path.join(ROOT, 'tools/ai-rt/dl/litert_1_4/jni/arm64-v8a/libtensorflowlite_jni.so'), f'{T}/libtensorflowlite_jni.so')
push_if_new(os.path.join(ROOT, a.ort), f'{T}/libonnxruntime.so')
md = os.path.join(ROOT, 'tools/ai-rt/ai-models')
for f in os.listdir(md): push_if_new(os.path.join(md, f), f'{T}/models/{f}')
for n in a.images.split(','):
    d = os.path.join(ROOT, 'build/ai-eval/ai-ref', n); adb('shell', f'mkdir -p {T}/ref/{n}')
    for f in os.listdir(d): adb('push', os.path.join(d, f), f'{T}/ref/{n}/{f}')   # content changes without size changes: always push
print(adb('shell', f'cd {T} && LD_LIBRARY_PATH={T} ./dapple_ai_tests --ref {T}/ref --models {T}/models --images {a.images} --only {a.only} --threads {a.threads} {a.extra} 2>&1'))
