"""Repeated vspipe benchmarks so the numbers are not a single noisy sample.

Each run goes to its own stderr file; the summary line and the plugin's per-frame
timing are parsed back out. Best-of-N is reported because the desktop compositor
takes a bite out of the GPU at unpredictable moments.
"""

import re
import statistics
import subprocess
import sys

VSPIPE = r"E:\code\VFI\.venv\Scripts\vspipe.exe"
TEST = r"E:\code\dlssg-vfi\test"

FRAME_MS = re.compile(
    r"\[vdlssg\] frame \d+ from \d+/\d+ slot \d+/\d+: ([\d.]+) ms")
TOTAL = re.compile(r"Output (\d+) frames in ([\d.]+) seconds \(([\d.]+) fps\)")


def bench(tag, script, start, end, runs):
    best = None
    for i in range(runs):
        err = rf"{TEST}\bench_{tag}.err"
        with open(err, "wb") as f:
            subprocess.run(
                [VSPIPE, "--start", str(start), "--end", str(end), script, "-"],
                stdout=subprocess.DEVNULL, stderr=f, check=False)
        text = open(err, "rb").read().decode("utf-8", "replace")
        times = [float(m.group(1)) for m in FRAME_MS.finditer(text)]
        total = TOTAL.search(text)
        if not total:
            print(f"{tag} run {i + 1}: no summary (see {err})")
            continue
        fps = float(total.group(3))
        ev = statistics.fmean(times) if times else float("nan")
        print(f"{tag} run {i + 1}: {total.group(1)} frames in {total.group(2)} s = "
              f"{fps:.2f} fps, evaluate mean {ev:.2f} ms, generated {len(times)}")
        if best is None or fps > best[0]:
            best = (fps, ev)
    if best:
        print(f"{tag} BEST: {best[0]:.2f} fps, evaluate mean {best[1]:.2f} ms")


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "4k"):
        bench("4k", rf"{TEST}\perf4k.py", 400, 520, 3)
    if which in ("all", "1080p"):
        bench("1080p", rf"{TEST}\smoke.py", 100, 140, 3)
