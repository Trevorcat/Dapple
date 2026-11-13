#!/usr/bin/env python3
"""One-line-per-phase summary of analyze-trace.py. usage: trace-summary.py <dir> <device> <tag>..."""
import sys, json, subprocess
d, dev = sys.argv[1:3]
for tag in sys.argv[3:]:
    print('==', tag)
    import os; out = os.popen('python ' + __file__.replace('trace-summary', 'analyze-trace') + ' ' + d + ' ' + dev + ' ' + tag).read()
    for l in out.splitlines():
        if l.startswith('   {'):
            v = json.loads(l)
            wrongStock = v['kinds'].get('fallbackStock', 0)
            disc = wrongStock + v['genBack'] + v['tierChanges'] + v['lutNChanges']
            print('   latency med/p90/max %.0f/%.0f/%.0f ms (>2P: %d) | render med/p90/max %.1f/%.1f/%.0f | inter-frame gaps>2P %d | fallbackStock %d genBack %d tier %d lutN %d => discontinuities %d | stalePrint %d | kinds %s' % (
                v['latMed'], v['latP90'], v['latMax'], v['latOver2P'], v['med'], v['p90'], v['max'], v['gaps'], wrongStock, v['genBack'], v['tierChanges'], v['lutNChanges'], disc,
                v['kinds'].get('stale', 0) + v['kinds'].get('lagging', 0), v['kinds']))
        elif not l.startswith('     gap'):
            print(l)
