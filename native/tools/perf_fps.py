"""Frame time from the game log's 'PGR4 D3D: frames N' lines (one per 150 frames) in a window.

usage: perf_fps.py <game log> <start HH:MM:SS> [seconds=40]
"""
import datetime as dt
import re
import sys

log, start, secs = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
pat = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*PGR4 D3D: frames (\d+) \| per frame: calls \d+, draw calls [\d.]+, tap draws ([\d.]+)")
rows = []
for line in open(log, encoding="utf-8", errors="replace"):
    m = pat.match(line)
    if m:
        rows.append((dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f"), int(m.group(2)), float(m.group(3))))
day = rows[0][0].date()
t0 = dt.datetime.combine(day, dt.datetime.strptime(start, "%H:%M:%S").time())
t1 = t0 + dt.timedelta(seconds=secs)
sel = [(a, b) for a, b in zip(rows, rows[1:]) if a[0] >= t0 and b[0] <= t1]
frames = sum(b[1] - a[1] for a, b in sel)
ms = sum((b[0] - a[0]).total_seconds() for a, b in sel) * 1000
draws = sum(b[2] * (b[1] - a[1]) for a, b in sel) / max(frames, 1)
print(f"{log}: {len(sel)} windows, {frames} frames in {ms/1000:.1f} s -> {ms/max(frames,1):.2f} ms/frame "
      f"({1000*frames/max(ms,1):.1f} fps), tap draws {draws:.0f}/frame")
for a, b in sel:
    n = b[1] - a[1]
    print(f"  {b[0].time()} frame {b[1]}: {(b[0]-a[0]).total_seconds()*1000/n:6.2f} ms/frame, draws {b[2]:.0f}")
