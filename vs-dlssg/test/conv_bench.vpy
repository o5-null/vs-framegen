"""Cost of turning the decoder's YUV into the RGB the engines take.

MODE=yuv    pass the decoded YUV frames through untouched (baseline)
MODE=rgb24  convert to 8-bit RGB (what a naive RGB-only graph costs)
MODE=rgbs   convert to RGBS float32 (what vdlssg/vxessfg/vvfg/vfsr4 require)
MODE=rgbs_dlss  RGBS + the DLSS 2x frame generation, for reference
"""

import os
import vapoursynth as vs

core = vs.core

MODE = os.environ.get("MODE", "rgbs")
CLIP = r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4"
PKG = r"E:\code\dlssg-vfi\vs-dlssg"

src = core.ffms2.Source(CLIP).std.Trim(90, 289)

if MODE == "yuv":
    out = src
elif MODE == "rgb24":
    out = src.resize.Bicubic(format=vs.RGB24, matrix_in_s="709")
elif MODE == "rgbs":
    out = src.resize.Bicubic(format=vs.RGBS, matrix_in_s="709")
elif MODE == "rgbs_dlss":
    core.std.LoadPlugin(PKG + r"\bin\vdlssg.dll")
    rgbs = src.resize.Bicubic(format=vs.RGBS, matrix_in_s="709")
    out = core.vdlssg.FrameGen(rgbs, multi=2, proxy_dir=PKG + r"\dlss45")
else:
    raise ValueError(MODE)

out.set_output()
