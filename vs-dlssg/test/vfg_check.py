"""Sanity check for the VFG backend: every frame must be finite, non-black, and neighbouring
interpolated frames must be closer to each other than the source pair is far apart."""
import os
import sys

import numpy as np
import vapoursynth as vs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
core = vs.core

PKG = r"E:\code\dlssg-vfi\vs-dlssg"
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vvfg.dll"))
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 95)

MULTI = 2
out = core.vvfg.FrameGen(src, multi=MULTI, proxy_dir=os.path.join(PKG, "vfg"), log=0)
out.set_output()


def stats(n):
    frame = out.get_frame(n)
    planes = [np.asarray(frame[i], dtype=np.float32) for i in range(3)]
    ok = all(np.isfinite(p).all() for p in planes)
    mean = float(np.mean([p.mean() for p in planes]))
    std = float(np.mean([p.std() for p in planes]))
    return planes, ok, mean, std


print("frames:", len(out))
prev = None
for n in range(min(8, len(out))):
    planes, ok, mean, std = stats(n)
    diff = ""
    if prev is not None:
        d = float(np.mean([np.abs(planes[i] - prev[i]).mean() for i in range(3)]))
        diff = " delta=%.4f" % d
    print("frame %d ok=%s mean=%.4f std=%.4f%s" % (n, ok, mean, std, diff))
    prev = planes
