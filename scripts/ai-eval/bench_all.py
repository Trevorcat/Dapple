import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import subprocess, sys, json, os
from models import *
PY = sys.executable
res = []
for n in M:
    for t in (1, 4):
        r = subprocess.run([PY, '-I', os.path.join(os.path.dirname(__file__), 'bench_one.py'), n, str(t)], capture_output=True, text=True, cwd=os.path.dirname(__file__), env={**os.environ, 'PYTHONPATH': os.path.dirname(__file__)})
        line = [l for l in r.stdout.splitlines() if l.startswith('{')]
        if line: res.append(json.loads(line[-1])); print(line[-1], flush=True)
        else: print('FAIL', n, t, r.stderr[-300:], flush=True)
json.dump(res, open(os.path.join(OUT, 'bench.json'), 'w'), indent=1)
