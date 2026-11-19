r"""How often the default scene-cut threshold fires on ordinary motion in this PV.

vdlssg's detector compares the two bracket frames of a pair with an 8-pixel-sampled mean absolute
difference over the three RGBS planes and holds the pair when the value exceeds scene_threshold
(0.15 by default). That threshold was tuned on gentle content; this PV moves a lot between adjacent
frames, so the useful number is how much of a normal 30 fps run sits above it.

Usage: python test\hsr_cutrate.py [start] [frames] [threshold]
Prints the gap-1 difference distribution and the share of frames the detector would flag, and cross
checks a few flagged frames against the plugin's own _DLSSGSceneCut flag.
"""

import os
import sys

import numpy as np
import vapoursynth as vs

ROOT = r"E:\code\dlssg-vfi"
SRC = os.path.join(ROOT, "测试数据",
                   "《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")
START = int(sys.argv[1]) if len(sys.argv) > 1 else 3500
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 300
THRESHOLD = float(sys.argv[3]) if len(sys.argv) > 3 else 0.15
CHECK = 4  # how many flagged frames to run through the plugin


def diff(a, b):
    """Same measure as isSceneCut: every 8th pixel, all three planes, mean absolute difference."""
    total = 0.0
    n = 0
    for pa, pb in zip(a, b):
        sa, sb = pa[::8, ::8], pb[::8, ::8]
        total += float(np.abs(sa - sb).sum())
        n += sa.size
    return total / n


def planes(frame):
    return [np.asarray(frame[p], dtype=np.float32) for p in range(3)]


def main():
    core = vs.core
    core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
    clip = core.resize.Bicubic(core.ffms2.Source(SRC), format=vs.RGBS, matrix_in_s="709")

    prev = planes(clip.get_frame(START))
    diffs = []
    for i in range(1, COUNT + 1):
        cur = planes(clip.get_frame(START + i))
        diffs.append(diff(prev, cur))
        prev = cur
        print("  frame %d/%d  diff %.4f" % (i, COUNT, diffs[-1]), end="\r", flush=True)
    print()

    arr = np.asarray(diffs)
    flagged = [START + i + 1 for i, d in enumerate(arr) if d > THRESHOLD]
    print("frames %d..%d, gap 1, threshold %.2f" % (START, START + COUNT, THRESHOLD))
    print("  diff mean %.4f  p50 %.4f  p90 %.4f  p99 %.4f  max %.4f"
          % (arr.mean(), np.percentile(arr, 50), np.percentile(arr, 90),
             np.percentile(arr, 99), arr.max()))
    for t in (0.15, 0.20, 0.25, 0.30, 0.35, 0.50):
        share = 100.0 * float((arr > t).mean())
        print("  threshold %.2f -> %.1f%% of pairs held" % (t, share))
    print("  at %.2f: %d/%d = %.1f%% held" % (THRESHOLD, len(flagged), COUNT,
                                               100.0 * len(flagged) / COUNT))

    if flagged and not os.environ.get("VDLSSG_NOCHECK"):
        # Cross check: the plugin should report the same verdict for those pairs.
        idx = [i for i, d in enumerate(arr) if d > THRESHOLD][:CHECK]
        base = START + idx[0]
        gen = core.vdlssg.FrameGen(core.std.Trim(clip, base - 1, base + CHECK + 1), multi=2,
                                   scene_threshold=THRESHOLD,
                                   proxy_dir=os.path.join(ROOT, ""vs-dlssg", "bin", "dlss45"), log=0)
        for i in idx[:CHECK]:
            frame = gen.get_frame((START + i - (base - 1)) * 2 + 1)
            print("  plugin says frame +%d: scene_cut=%s fallback=%s"
                  % (i, frame.props.get("_DLSSGSceneCut"), frame.props.get("_DLSSGFallback")))


if __name__ == "__main__":
    main()
