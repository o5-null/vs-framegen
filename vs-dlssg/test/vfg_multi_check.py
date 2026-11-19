"""Checks that a multi=4 run really produces four different in-betweens per pair.

Each frame of the generated sequence should differ from the one before it: a repeated frame shows up as a
near-zero delta, which is what the model used to return for every in-between after the first.
"""
import os
import sys

import numpy as np
import vapoursynth as vs

core = vs.core


def find_pkg():
    here = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.exists(os.path.join(here, "bin", "vvfg.dll")):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("bin/vvfg.dll not found above " + __file__)
        here = parent


pkg = find_pkg()
multi = int(os.environ.get("VFG_MULTI", "4"))
core.std.LoadPlugin(path=os.path.join(pkg, "bin", "vvfg.dll"))
src = core.ffms2.Source(os.path.join(pkg, "..", "media", "sintel_trailer_1080p.mp4"))
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 93)
clip = core.vvfg.FrameGen(src, multi=multi, proxy_dir=os.path.join(pkg, "vfg"), log=1)

n = len(clip)
prev = None
zeros = 0
for i in range(n):
    frame = np.asarray(clip.get_frame(i))
    if frame.dtype != np.float32:
        raise SystemExit("unexpected dtype %s" % frame.dtype)
    if prev is not None:
        delta = float(np.abs(frame - prev).mean())
        print("frame %2d delta %.5f %s" % (i, delta, "<- repeat" if delta < 1e-4 else ""))
        if delta < 1e-4:
            zeros += 1
    prev = frame

print("%d frames, %d repeats" % (n, zeros))
sys.exit(1 if zeros else 0)
