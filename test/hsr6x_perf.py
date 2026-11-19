"""Throughput harness for the HSR PV: configuration comes from the environment (vspipe has no argv).

VDLSSG_SRC    source mkv (default: the HSR PV in 测试数据)
VDLSSG_START  first source frame (default 3510)
VDLSSG_END    last source frame, exclusive (default START + 30 * MULTI)
VDLSSG_MULTI  output multiplier (default 6)
VDLSSG_W      optional output width; the source is 3840x1632, so 1920 halves it
"""

import os

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\plugin\vdlssg.dll")

MULTI = int(os.environ.get("VDLSSG_MULTI", "6"))
START = int(os.environ.get("VDLSSG_START", "3510"))
END = int(os.environ.get("VDLSSG_END", str(START + 30 * MULTI)))
SRC = os.environ.get(
    "VDLSSG_SRC",
    r"E:\code\dlssg-vfi\测试数据\《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")

src = core.ffms2.Source(SRC)
w = int(os.environ.get("VDLSSG_W", "0"))
if w:
    src = core.resize.Bicubic(src, width=w, height=round(src.height * w / src.width),
                              format=vs.RGBS, matrix_in_s="709")
else:
    src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, START, END - 1)
out = core.vdlssg.FrameGen(src, multi=MULTI,
                           proxy_dir=r"E:\code\dlssg-vfi\dist\engine45", log=1)
out.set_output()
