r"""Pick a cut-free, high-motion window from the HSR PV for frame-generation tests.

Scans the whole clip on a small gray proxy, prints scene cuts and the busiest windows,
so the quality/throughput segment is chosen from data instead of a guess.

Usage: python test\hsr_scan.py [mkv]
"""

import os
import sys

import numpy as np
import vapoursynth as vs

SRC = sys.argv[1] if len(sys.argv) > 1 else (
    r"E:\code\dlssg-vfi\测试数据\《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")

core = vs.core
gray = core.resize.Bilinear(core.ffms2.Source(SRC), 480, 204, format=vs.GRAYS)
n = gray.num_frames
fps = gray.fps_num / gray.fps_den

diffs = np.zeros(n, dtype=np.float32)
prev = None
for i in range(n):
    cur = np.asarray(gray.get_frame(i)[0], dtype=np.float32)
    if prev is not None:
        diffs[i] = float(np.abs(cur - prev).mean())
    prev = cur

cuts = np.where(diffs > 12.0)[0]
print("frames=%d fps=%.3f duration=%.1fs" % (n, fps, n / fps))
print("cuts (frame@sec): " + ", ".join("%d@%.2f" % (c, c / fps) for c in cuts[:40])
      + (" ..." if len(cuts) > 40 else ""))

win = int(round(fps))  # one second
score = np.convolve(diffs[1:], np.ones(win) / win, mode="valid")  # score[k] = mean diff for k+1..k+win
order = np.argsort(score)[::-1]
seen = []
print("\nbusiest 1 s windows (frame range, sec range, mean luma diff, cuts inside):")
for idx in order:
    start = int(idx) + 1
    if any(abs(start - s) < 3 * win for s in seen):
        continue
    seen.append(start)
    inside = int(((cuts >= start) & (cuts < start + win)).sum())
    print("  %5d-%5d  %6.2f-%6.2f s  diff=%.2f  cuts=%d"
          % (start, start + win - 1, start / fps, (start + win) / fps, score[idx], inside))
    if len(seen) >= 12:
        break

# Also report a 4 s window around the best cut-free start, for longer throughput runs.
for cand in seen:
    lo = cand
    hi = min(cand + 4 * win, n)
    if ((cuts >= lo) & (cuts < hi)).sum() == 0 and (hi - lo) == 4 * win:
        print("\ncut-free 4 s window: %d..%d (%.2f..%.2f s), mean diff=%.2f"
              % (lo, hi, lo / fps, hi / fps, float(diffs[lo:hi].mean())))
        break
