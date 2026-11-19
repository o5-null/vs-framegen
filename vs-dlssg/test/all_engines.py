"""Renders a few frames with each engine plugin and checks the output is a real tween.

NGX is one instance per process (the first NVSDK_NGX_D3D12_Init fixes the proxy and its Engine= setting),
so vdlssg and vxessfg cannot share a script: with no argument this runs each engine in its own child
process, which is also how a user has to compare them.
"""
import os
import subprocess
import sys

import numpy as np
import vapoursynth as vs

core = vs.core

PLAN = [
    ("dlss", "vdlssg", 4, "engine45"),
    ("xess", "vxessfg", 4, "engine45_xess"),
    ("vfg", "vvfg", 4, "engine_nvvfx"),
    ("fsr4", "vfsr4", 2, "engine_fsrfg"),
]


def find_pkg():
    here = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.exists(os.path.join(here, "bin", "vfsr4.dll")):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("bin/vfsr4.dll not found above " + __file__)
        here = parent


def run_one(label):
    pkg = find_pkg()
    name, fn, multi, proxy = next(p for p in PLAN if p[0] == label)
    core.std.LoadPlugin(path=os.path.join(pkg, "bin", fn + ".dll"))
    src = core.ffms2.Source(os.path.join(pkg, "..", "media", "sintel_trailer_1080p.mp4"))
    src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
    src = core.std.Trim(src, 90, 93)
    clip = getattr(core, fn).FrameGen(src, multi=multi, proxy_dir=os.path.join(pkg, proxy), log=0)
    frames = [np.asarray(clip.get_frame(i)) for i in range(len(clip))]
    repeats = sum(1 for a, b in zip(frames, frames[1:]) if float(np.abs(a - b).mean()) < 1e-4)
    # The tail of the sequence holds the last source frame in the remaining slots; anything else repeating
    # would mean the engine returned the previous frame instead of an in-between.
    ok = len(frames) == len(src) * multi and repeats <= multi - 1
    print("%-5s %2d frames, %d repeats, mean %.4f %s"
          % (label, len(frames), repeats, float(frames[1].mean()), "ok" if ok else "FAIL"))
    return 0 if ok else 1


if len(sys.argv) > 1:
    sys.exit(run_one(sys.argv[1]))

failed = 0
for label, _, _, _ in PLAN:
    done = subprocess.run([sys.executable, os.path.abspath(__file__), label],
                          capture_output=True, text=True)
    line = (done.stdout.strip().splitlines() or ["(no output)"])[-1]
    print(line if done.returncode == 0 else "%-5s FAIL exit %d %s"
          % (label, done.returncode, (done.stderr.strip().splitlines() or [""])[-1]))
    failed += done.returncode != 0
print("%d of %d engines ok" % (len(PLAN) - failed, len(PLAN)))
sys.exit(1 if failed else 0)
