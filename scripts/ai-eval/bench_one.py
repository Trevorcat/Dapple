import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sys, statistics, psutil
from models import *
name, threads = sys.argv[1], int(sys.argv[2])
pr = psutil.Process(); base = pr.memory_info().peak_wset
im = load('fruit'); rt, rel, fn = M[name]
xs = fn(im)
t0 = time.perf_counter(); run = make_runner(name, threads); load_ms = (time.perf_counter() - t0) * 1e3
t0 = time.perf_counter(); run(xs); first = (time.perf_counter() - t0) * 1e3
ts = []
n = 3 if first > 1500 else (6 if first > 400 else 15)
for _ in range(n):
    t0 = time.perf_counter(); run(xs); ts.append((time.perf_counter() - t0) * 1e3)
peak = pr.memory_info().peak_wset
print(json.dumps(dict(name=name, threads=threads, size_mb=round(os.path.getsize(os.path.join(MD, rel)) / 1e6, 2),
  in_shape=[list(x.shape) for x in xs][:1], load_ms=round(load_ms), first_ms=round(first, 1), median_ms=round(statistics.median(ts), 1), min_ms=round(min(ts), 1),
  runs=n, peak_delta_mb=round((peak - base) / 1e6))))
