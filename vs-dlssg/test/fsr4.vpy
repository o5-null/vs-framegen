import os
import vapoursynth as vs

core = vs.core


def find_pkg():
    starts = [os.getcwd()]
    try:
        starts.append(os.path.dirname(os.path.abspath(__file__)))
    except NameError:
        pass
    for start in starts:
        d = start
        for _ in range(4):
            if os.path.exists(os.path.join(d, "bin", "vfsr4.dll")):
                return d
            d = os.path.dirname(d)
    return os.environ.get("VDLSSG_PKG", r"E:\code\dlssg-vfi\vs-dlssg")


PKG = find_pkg()
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vfsr4.dll"))
SRC = os.environ.get("VDLSSG_SRC", r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
# The FSR bridge generates one midpoint per frame pair, so multi is fixed at 2.
MULTI = int(os.environ.get("FSR4_MULTI", "2"))
LOG = int(os.environ.get("FSR4_LOG", "1"))

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 95)
out = core.vfsr4.FrameGen(src, multi=MULTI, proxy_dir=os.path.join(PKG, "fsr4"), log=LOG)
out.set_output()
