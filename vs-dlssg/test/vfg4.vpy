# NVIDIA Video Frame Generation smoke: 1080p source through vfg (Video Effects SDK 1.3).
# Run:  vspipe -p test\vfg4.vpy NUL
# Needs the in-process sm_89 -> sm_86 unlock, which vfg_session.cpp installs on the model DLL's IAT.
import os

import vapoursynth as vs

core = vs.core

MULTI = int(os.environ.get("VFG_MULTI", "4"))
# Model tier, the VFX SDK's NVVFXVIDEOFRAMEGENERATION_MODE: 0 low, 1 medium (SDK default), 2 high.
MODE = int(os.environ.get("VFG_MODE", "1"))


def find_pkg():
    """包根 = 含 bin\\vvfg.dll 的那一层。"""
    starts = [os.getcwd()]
    try:
        starts.append(os.path.dirname(os.path.abspath(__file__)))
    except NameError:
        pass
    for start in starts:
        d = start
        for _ in range(4):
            if os.path.exists(os.path.join(d, "bin", "vvfg.dll")):
                return d
            d = os.path.dirname(d)
    return os.environ.get("VDLSSG_PKG", r"E:\code\dlssg-vfi\vs-dlssg")


PKG = find_pkg()
core.std.LoadPlugin(path=os.path.join(PKG, "bin", "vvfg.dll"))

SRC = os.environ.get("VDLSSG_SRC", r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 113)   # 24 source frames -> 24*MULTI output frames

out = core.vvfg.FrameGen(
    src,
    multi=MULTI,
    mode=MODE,
    proxy_dir=os.path.join(PKG, "bin", "vfg"),
    log=1,
)
out.set_output()
