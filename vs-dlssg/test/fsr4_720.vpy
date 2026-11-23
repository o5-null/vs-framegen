# Timing probe: same graph as fsr4.vpy but at 720p, to tell a bandwidth-bound readback
# from a fixed cross-device sync cost.
import os
import vapoursynth as vs

core = vs.core
PKG = r"E:\code\dlssg-vfi\vs-dlssg"
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vfsr4.dll"))
SRC = r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4"

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, width=1280, height=720, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 95)
out = core.vfsr4.FrameGen(src, multi=2, proxy_dir=os.path.join(PKG, "bin", "fsr4"), log=1)
out.set_output()
