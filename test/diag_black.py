"""Why does every clip after the first come back black?

Prints the mean value of each plane and the plugin's own properties for one clip in a fresh
process, so a session-reuse artefact shows up as "first clip fine, second clip black".
"""

import os
import sys

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(ROOT, "media", "frames")
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")

tags = sys.argv[1:] or ["t00", "t01", "t03"]
for tag in tags:
    clip = os.path.join(TRI, tag + ".mkv")
    src = core.resize.Bicubic(core.ffms2.Source(clip), format=vs.RGBS)
    out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=1)

    f0 = out.get_frame(0)
    f1 = out.get_frame(1)
    a = np.fromfile(os.path.join(FRAMES, tag + "_a.rgb"), dtype=np.uint8).reshape(1080, 1920, 3)
    mean_a = float(a.mean()) / 255.0
    print("DIAG %s: a mean=%.4f | slot0 plane0 mean=%.4f | slot1 plane0 mean=%.4f" % (
        tag, mean_a, float(np.asarray(f0[0]).mean()), float(np.asarray(f1[0]).mean())))
    for key in ("_DLSSGGenerated", "_DLSSGFallback", "_DLSSGSceneCut"):
        print("DIAG    %s=%s" % (key, f1.props.get(key)))
