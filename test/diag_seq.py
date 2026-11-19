"""Does a second FrameGen filter in the same process produce garbage?

Two hypotheses:
  A) sequential clips (previous filter released -> NgxSession destroyed -> NGX shutdown, then a
     fresh init) leaves the feature unable to write its output
  B) something about the shared state (colour texture retention, frame ids) across clips

Case 1 keeps both filters alive at once (the session is shared, refcount 2, no shutdown between).
Case 2 releases the first clip before building the second, so the session is torn down and
re-initialised. If case 1 is fine and case 2 is flat, A is confirmed.
"""

import gc
import os
import sys

import numpy as np
import vapoursynth as vs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")

TAGS = ["t01", "t05", "t07"]


def build(tag):
    src = core.resize.Bicubic(core.ffms2.Source(os.path.join(TRI, tag + ".mkv")), format=vs.RGBS)
    return core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)


def report(label, tag, clip):
    f1 = clip.get_frame(1)
    p0 = np.asarray(f1[0])
    print("TEST %s %s: slot1 p0 mean=%.4f std=%.4f fallback=%s" % (
        label, tag, p0.mean(), p0.std(), f1.props.get("_DLSSGFallback")))


mode = sys.argv[1] if len(sys.argv) > 1 else "coexist"

if mode == "coexist":
    clips = {tag: build(tag) for tag in TAGS}
    for tag in TAGS:
        report("coexist", tag, clips[tag])
else:
    for tag in TAGS:
        clip = build(tag)
        report("sequential", tag, clip)
        del clip
        gc.collect()
