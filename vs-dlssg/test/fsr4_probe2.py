import os
import sys
import traceback

import vapoursynth as vs

core = vs.core
PKG = r"E:\code\dlssg-vfi\vs-dlssg"
SRC = r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4"
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vfsr4.dll"))
print("plugin loaded", flush=True)

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 95)
print("source", src.width, src.height, src.num_frames, flush=True)

try:
    out = core.vfsr4.FrameGen(src, multi=2, proxy_dir=os.path.join(PKG, "engine_fsrfg"), log=1)
    print("graph built", out.num_frames, flush=True)
except Exception:
    traceback.print_exc()
    sys.exit(2)

try:
    for n in range(out.num_frames):
        frame = out.get_frame(n)
        print("frame", n, frame.width, frame.height, frame.format.name, flush=True)
except Exception:
    traceback.print_exc()
    sys.exit(3)
print("done", flush=True)
