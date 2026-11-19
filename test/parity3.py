r"""Quality check for multi>2: subsample the source by `multi`, then ask the plugin to rebuild
the frames it dropped. With multi=3 the pair (3k, 3k+3) brackets real frames 3k+1 and 3k+2, which
sit exactly at 1/3 and 2/3 of the interval - the positions multiFrameIndex 1 and 2 claim.

Usage: python parity3.py [multi] [start] [count]
"""

import os
import sys

import numpy as np
import vapoursynth as vs

MEDIA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "media")
ROOT = os.path.dirname(MEDIA)
SRC = os.path.join(MEDIA, "sintel_trailer_1080p.mp4")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")

MULTI = int(sys.argv[1]) if len(sys.argv) > 1 else 3
START = int(sys.argv[2]) if len(sys.argv) > 2 else 300
COUNT = int(sys.argv[3]) if len(sys.argv) > 3 else 8


def planes(frame):
    return [np.asarray(frame[p], dtype=np.float32) for p in range(3)]


def mae(x, y):
    """x, y are per-plane lists of float arrays."""
    total = 0.0
    for px, py in zip(x, y):
        total += float(np.abs(px - py).mean())
    return total / len(x)


def main():
    # Reference: every real frame in the window, untouched by the plugin.
    ref_clip = core.resize.Bicubic(core.ffms2.Source(SRC), format=vs.RGBS, matrix_in_s="709")
    ref = {n: planes(ref_clip.get_frame(n)) for n in range(START, START + COUNT * MULTI)}

    # Input: only every MULTI-th frame, so the gaps are the frames the plugin must invent.
    thin = core.std.SelectEvery(ref_clip, MULTI, 0)
    thin = core.std.Trim(thin, START // MULTI, START // MULTI + COUNT - 1)
    out = core.vdlssg.FrameGen(thin, multi=MULTI, proxy_dir=PROXY, log=0)

    slot_err = [[] for _ in range(MULTI)]
    slot_blend = [[] for _ in range(MULTI)]
    dup = []
    for j in range(COUNT):
        base = START + j * MULTI
        a = ref[base]
        b = ref[base + MULTI] if base + MULTI in ref else None
        got = []
        for slot in range(MULTI):
            frame = planes(out.get_frame(j * MULTI + slot))
            got.append(frame)
            if slot == 0:
                continue
            gt = ref[base + slot]
            slot_err[slot].append(mae(frame, gt))
            if b is not None:
                alpha = slot / MULTI
                blended = [(1.0 - alpha) * x + alpha * y for x, y in zip(a, b)]
                slot_blend[slot].append(mae(blended, gt))
        for slot in range(1, MULTI):
            for other in range(slot + 1, MULTI):
                dup.append(mae(got[slot], got[other]))

    print("multi=%d start=%d groups=%d" % (MULTI, START, COUNT))
    total_err = []
    total_blend = []
    for slot in range(1, MULTI):
        e = float(np.mean(slot_err[slot]))
        bl = float(np.mean(slot_blend[slot])) if slot_blend[slot] else float("nan")
        total_err.append(e)
        total_blend.append(bl)
        print("  slot %d (alpha=%.2f): gen-vs-gt=%.6f blend-vs-gt=%.6f  gain=%.2fx"
              % (slot, slot / MULTI, e, bl, (bl / e) if e > 0 else float("inf")))
    if dup:
        print("  distinctness: mean MAE between generated slots=%.6f min=%.6f"
              % (float(np.mean(dup)), float(np.min(dup))))
    print("  all slots: gen-vs-gt=%.6f  blend-vs-gt=%.6f  gain=%.2fx"
          % (float(np.mean(total_err)), float(np.mean(total_blend)),
             float(np.mean(total_blend)) / float(np.mean(total_err))))


if __name__ == "__main__":
    main()
