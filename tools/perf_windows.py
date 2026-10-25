"""Group the XBE_PERF seconds of one or more game logs (logs/mc3_xbe.log) by draws per frame, so runs with
different scenes compare. See docs/PERFORMANCE.md.

usage: perf_windows.py LABEL=mc3_xbe.log [LABEL=mc3_xbe.log ...]
"""
import re
import sys
from collections import defaultdict

FRAME = re.compile(r"perf: ([\d.]+) fps ([\d.]+) ms/frame \| host ([\d.]+) \(draw ([\d.]+)")
TITLE = re.compile(r"perf: title thread per frame: recording ([\d.]+) ms, waiting ([\d.]+) ms")
DRAWS = re.compile(r"perf: per frame (\d+) draws")
BUCKET = 500

rows = {}
for arg in sys.argv[1:]:
    label, path = arg.split("=", 1)
    frame = title = None
    buckets = defaultdict(list)
    for line in open(path, errors="replace"):
        if m := FRAME.search(line):
            frame = m
        elif m := TITLE.search(line):
            title = m
        elif (m := DRAWS.search(line)) and frame and title:
            draws = int(m.group(1))
            fps, ms, host, draw = map(float, frame.groups())
            # a second with a load or a shader compile is not a steady frame
            if ms < 200:
                buckets[draws // BUCKET * BUCKET].append((ms, host, float(title.group(1)), float(title.group(2)), draws))
            frame = title = None
    rows[label] = buckets

keys = sorted({k for b in rows.values() for k in b})
print(f"{'draws':>11} | " + " | ".join(f"{label:^42}" for label in rows))
print(f"{'':>11} | " + " | ".join(f"{'n':>3} {'ms/frm':>7} {'host':>6} {'rec':>6} {'wait':>6} {'ms/1kd':>7}" for _ in rows))
for k in keys:
    cells = []
    for label, b in rows.items():
        v = b.get(k)
        if not v:
            cells.append(" " * 42)
            continue
        n = len(v)
        avg = [sum(x[i] for x in v) / n for i in range(5)]
        cells.append(f"{n:>3} {avg[0]:>7.2f} {avg[1]:>6.2f} {avg[2]:>6.2f} {avg[3]:>6.2f} {avg[0] / avg[4] * 1000:>7.2f}")
    print(f"{k:>5}-{k + BUCKET - 1:<5} | " + " | ".join(cells))
