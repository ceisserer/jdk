#!/usr/bin/env python3
"""Merge startup timestamps into one timeline, relative to process start.

Input (stdin or file): output of a run linked with TIMING=1 (wasm/link.sh),
started with e.g. -Xlog:startuptime,os+thread:stderr:timemillis. Understands
  [timing] <epoch ms> <ms since process start> <label>   (timing-pre.js)
  [timing] <epoch ms> - <label>                          (from Java code)
  [<epoch ms>ms] <message>                               (HotSpot -Xlog ...:timemillis)
"""
import re, sys

rows, origin = [], None
for line in (open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin):
    m = re.match(r'\[timing\] ([0-9.]+) (\S+) (.*)', line)
    if m:
        t = float(m.group(1))
        if m.group(2) != '-' and origin is None:
            origin = t - float(m.group(2))
        rows.append((t, 'js/java', m.group(3)))
        continue
    m = re.match(r'\[(\d+)ms\] (.*)', line)
    if m:
        rows.append((float(m.group(1)), 'vm', m.group(2)))
rows.sort(key=lambda r: r[0])
if origin is not None:
    rows.insert(0, (origin, 'node', 'process start'))
t0 = rows[0][0]
prev = t0
for t, src, msg in rows:
    print(f'{t - t0:8.1f} ms  (+{t - prev:6.1f})  {src:8s} {msg[:100]}')
    prev = t
