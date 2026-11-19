"""One clip per process: is the generated frame a real image or a constant?

Prints mean and std per plane plus a few sample values. A generated frame that is a flat
constant has std ~0; a real image does not. Run the same tag alone and after another tag to see
whether reusing the process (and the NGX session) changes the result.
"""

import os
import sys

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = MEDIA
FRAMES = os.path.join(MEDIA, "media", "frames")
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")

for tag in sys.argv[1:]:
    src = core.resize.Bicubic(core.ffms2.Source(os.path.join(TRI, tag + ".mkv")), format=vs.RGBS)
    out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)

    print("DIAG2 %s src frames=%d" % (tag, src.num_frames))
    for slot in (0, 1):
        f = out.get_frame(slot)
        line = []
        for p in range(3):
            arr = np.asarray(f[p])
            line.append("p%d mean=%.4f std=%.4f" % (p, arr.mean(), arr.std()))
        print("DIAG2   slot%d %s" % (slot, " | ".join(line)))
    arr = np.asarray(out.get_frame(1)[0])
    print("DIAG2   slot1 plane0 corners: %s" % [round(float(arr[y, x]), 3)
                                                for y, x in ((0, 0), (0, 1919), (1079, 0), (540, 960))])
