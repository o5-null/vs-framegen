"""Generate one tween frame with one engine, in a fresh process.

NGX initialises once per process, so DLSS-G and XeSS-FG cannot share a run; this script
is spawned once per engine by make_pv_compare.py.

Usage: python pv_engine_frame.py <engine> <frameA.png> <frameB.png> <out.png>
"""

import os
import sys

import numpy as np
import vapoursynth as vs
from vapoursynth import core

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "vs-dlssg", "bin")

ENGINES = {
    # name: (dll, namespace, pack, multi)
    "dlssg": ("vdlssg.dll", "vdlssg", "dlss45", 2),
    "xessfg": ("vxessfg.dll", "vxessfg", "xessfg", 2),
    "vfg": ("vvfg.dll", "vvfg", "vfg", 2),
    "fsr4": ("vfsr4.dll", "vfsr4", "fsr4", 2),
}


def main() -> int:
    engine, png_a, png_b, out = sys.argv[1:5]
    dll, ns, pack, multi = ENGINES[engine]
    core.std.LoadPlugin(os.path.join(BIN, dll))

    # imwri gives RGB24 (or whatever the PNG holds); the plugins take RGBS float.
    clip = core.imwri.Read([png_a, png_b])
    clip = core.resize.Bicubic(clip, format=vs.RGBS)
    fg = getattr(core, ns).FrameGen(clip, multi=multi, proxy_dir=os.path.join(BIN, pack), log=0)
    out_clip = core.resize.Bicubic(fg, format=vs.RGB24)

    frame = out_clip.get_frame(1)  # 0 = A, 1 = the tween, 2 = B
    planes = [np.asarray(frame[p]) for p in range(3)]
    rgb = np.stack(planes, axis=2)

    import cv2

    ok, buf = cv2.imencode(".png", rgb[:, :, ::-1])
    if not ok:
        raise SystemExit("cannot encode " + out)
    buf.tofile(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
