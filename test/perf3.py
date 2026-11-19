"""Throughput harness: size and multi come from the environment, since vspipe passes no argv.

VDLSSG_MULTI and VDLSSG_SIZE ("1080p" or "4k") select the configuration.
"""

import os

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\vs-dlssg\bin\vdlssg.dll")

MULTI = int(os.environ.get("VDLSSG_MULTI", "3"))
SIZE = os.environ.get("VDLSSG_SIZE", "4k")
W, H = (1920, 1080) if SIZE == "1080p" else (3840, 2160)

src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
if (W, H) == (1920, 1080):
    clip = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
else:
    clip = core.resize.Bicubic(src, width=W, height=H, format=vs.RGBS, matrix_in_s="709")
out = core.vdlssg.FrameGen(clip, multi=MULTI, proxy_dir=r"E:\code\dlssg-vfi\work", log=1)
out.set_output()
