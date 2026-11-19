"""Throughput for multi = 2/3/4 at 1080p and 4K, best of N.

Each run gets its own stderr file, because vspipe's raw output must never go to a log.
"""

import os
import re
import statistics
import subprocess
import sys

VSPIPE = r"E:\code\VFI\.venv\Scripts\vspipe.exe"
TEST = r"E:\code\dlssg-vfi\test"

FRAME_MS = re.compile(
    r"\[vdlssg\] frame \d+ from \d+/\d+ slot (\d+)/(\d+): ([\d.]+) ms")
TOTAL = re.compile(r"Output (\d+) frames in ([\d.]+) seconds \(([\d.]+) fps\)")
RUNS = 3


def bench(multi, size, start, end):
    times = []
    env = dict(os.environ, VDLSSG_MULTI=str(multi), VDLSSG_SIZE=size)
    for i in range(RUNS):
        err = rf"{TEST}\bench_{size}_m{multi}.err"
        with open(err, "wb") as f:
            subprocess.run([VSPIPE, "--start", str(start), "--end", str(end),
                            rf"{TEST}\perf3.py", "-"],
                           stdout=subprocess.DEVNULL, stderr=f, check=False, env=env)
        text = open(err, "rb").read().decode("utf-8", "replace")
        ev = [float(m.group(3)) for m in FRAME_MS.finditer(text)]
        total = TOTAL.search(text)
        if not total:
            print(f"multi={multi} {size} run {i + 1}: no summary (see {err})")
            continue
        times.append((float(total.group(3)), statistics.fmean(ev) if ev else float("nan")))
        print(f"multi={multi} {size} run {i + 1}: {total.group(1)} frames {total.group(2)} s "
              f"= {times[-1][0]:.2f} fps, evaluate mean {times[-1][1]:.2f} ms")
    if times:
        best = max(times, key=lambda t: t[0])
        src = best[0] / multi
        print(f"multi={multi} {size} BEST: {best[0]:.2f} fps out = {src:.2f} source frames/s, "
              f"evaluate mean {best[1]:.2f} ms\n")


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    for multi in (2, 3, 4):
        if which in ("all", "4k"):
            bench(multi, "4k", 400, 400 + 40 * multi)
        if which in ("all", "1080p"):
            bench(multi, "1080p", 300, 300 + 40 * multi)
