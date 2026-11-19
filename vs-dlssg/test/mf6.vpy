# 6x smoke: 1080p source through dlss45 (MaxGeneratedFrames=5 -> 5 interpolated frames).
# Run:  vspipe -p test\mf6.vpy NUL
import os

import vapoursynth as vs

core = vs.core


def find_pkg():
    """包根 = 含 bin\\vdlssg.dll 的那一层；vspipe 给的 __file__ 是不可靠的相对路径，所以向上找。"""
    starts = [os.getcwd()]
    try:
        starts.append(os.path.dirname(os.path.abspath(__file__)))
    except NameError:
        pass
    for start in starts:
        d = start
        for _ in range(4):
            if os.path.exists(os.path.join(d, "bin", "vdlssg.dll")):
                return d
            d = os.path.dirname(d)
    return os.environ.get("VDLSSG_PKG", r"E:\code\dlssg-vfi\vs-dlssg")


PKG = find_pkg()
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vdlssg.dll"))

SRC = os.environ.get("VDLSSG_SRC", r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 0, 47)   # 48 source frames -> 288 output frames

out = core.vdlssg.FrameGen(
    src,
    multi=6,
    proxy_dir=os.path.join(PKG, "dlss45"),
    log=1,
)
out.set_output()
