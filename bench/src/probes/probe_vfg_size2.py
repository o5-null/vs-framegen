"""Follow-up on probe_vfg_size: does the 1600x900 / 2240x1260 slow reading at 40 frames
reproduce, and where is the smallest accepted size? Same conditions as run_throughput
(40 frames, 3 passes, median)."""
import os
import statistics
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import common as C
import smv_run

SIZES = [(960, 540), (1000, 600), (1024, 576), (1280, 720),
         (1600, 900), (1920, 1080), (2240, 1260), (2560, 1440)]
FRAMES = 40


def med(w, h, frames, passes=3):
    raw = C.ensure_raw(w, h, "probe%d" % w, frames)
    out = []
    for _ in range(passes):
        info = smv_run.run(["--nvvfg"], raw, w, h, frames)
        if info["exit"] != 0 or not info["pairs"]:
            return None, info
        out.append(info["ms_per_pair"])
    return out, None


def main():
    for w, h in SIZES:
        got, bad = med(w, h, FRAMES)
        if got is None:
            print("%4dx%-4d FAILED  %s" % (w, h, bad["log"].strip().splitlines()[-1][:80]), flush=True)
            continue
        got12, bad12 = med(w, h, 12, passes=1)
        print("%4dx%-4d 40f: %7.2f ms/pair (%s)   12f: %s"
              % (w, h, statistics.median(got), [round(x, 2) for x in got],
                 "%.2f" % got12[0] if got12 else "FAILED"), flush=True)


if __name__ == "__main__":
    main()
