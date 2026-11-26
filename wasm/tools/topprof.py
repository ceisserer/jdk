#!/usr/bin/env python3
"""Summarize Node CPU profiles (one per worker thread) written by
   node --cpu-prof --cpu-prof-dir=DIR java.js ...
Prints the busiest threads and their top self-time functions. Link with
DEBUG=1 (wasm/link.sh) so Wasm frames carry function names."""
import collections, glob, json, sys

IDLE = {'(idle)', '(program)', '(garbage collector)', 'emscripten_futex_wait'}

rows = []
for f in glob.glob(sys.argv[1] + '/*.cpuprofile'):
    p = json.load(open(f))
    nodes = {n['id']: n for n in p['nodes']}
    c = collections.Counter()
    for s, dt in zip(p['samples'], p['timeDeltas']):
        c[nodes[s]['callFrame']['functionName'] or '(anon)'] += dt
    work = sum(v for k, v in c.items() if k not in IDLE) / 1000
    rows.append((work, f, c))
rows.sort(key=lambda r: r[0], reverse=True)
for work, f, c in rows[:int(sys.argv[2]) if len(sys.argv) > 2 else 3]:
    print(f"== {f.split('/')[-1]}: work {work:.0f} ms, futex wait {c['emscripten_futex_wait'] / 1000:.0f} ms")
    for k, v in c.most_common(15):
        print(f"   {v / 1000:8.0f} ms  {k}")
