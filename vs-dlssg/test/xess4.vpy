# XeSS-FG smoke: 1080p source through engine45_xess (Engine=XeSS, MaxGeneratedFrames=3 -> 4x).
# Run:  vspipe -p test\xess4.vpy NUL
import os

import vapoursynth as vs

core = vs.core


def find_pkg():
    """包根 = 含 bin\\vxessfg.dll 的那一层。"""
    starts = [os.getcwd()]
    try:
        starts.append(os.path.dirname(os.path.abspath(__file__)))
    except NameError:
        pass
    for start in starts:
        d = start
        for _ in range(4):
            if os.path.exists(os.path.join(d, "bin", "vxessfg.dll")):
                return d
            d = os.path.dirname(d)
    return os.environ.get("VDLSSG_PKG", r"E:\code\dlssg-vfi\vs-dlssg")


PKG = find_pkg()
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vxessfg.dll"))

SRC = os.environ.get("VDLSSG_SRC", r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
MULTI = int(os.environ.get("XESS_MULTI", "4"))

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 0, 23)   # 24 source frames -> 24*MULTI output frames

out = core.vxessfg.FrameGen(
    src,
    multi=MULTI,
    proxy_dir=os.path.join(PKG, "engine45_xess"),
    log=1,
)
out.set_output()
