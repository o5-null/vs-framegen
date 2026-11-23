# 6x smoke: 1080p source through dlss45 (MaxGeneratedFrames=5 -> 5 interpolated frames).
# Run:  cmd /c "E:\code\dlssg-vfi\.venv\Scripts\vspipe.exe -p test\mf6.vpy NUL"
# The DLSS-G model preset comes from VDLSSG_PRESET: "a" (default model), "c" (FG Lite, faster and
# slightly softer), or "b" (refused - it needs a HUD-less image plus a UI plane the plugin cannot
# hand over). The plugin writes the value into <proxy_dir>\dlssg_sm86.ini before it pins it.
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
# The engine pack (INI, version.dll, runtime) lives in bin\dlss45, not next to the DLL itself.
PROXY = os.path.join(PKG, "bin", "dlss45")
PRESET = os.environ.get("VDLSSG_PRESET", "a")

src = core.ffms2.Source(SRC)
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 0, 47)   # 48 source frames -> 288 output frames

out = core.vdlssg.FrameGen(
    src,
    multi=6,
    proxy_dir=PROXY,
    preset=PRESET,
    log=1,
)
out.set_output()
