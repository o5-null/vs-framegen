"""Per-clip fingerprints: is a later clip returning a stale or wrong picture?

For each tag it prints md5 of the passthrough slot and the generated slot, plus MAE against the
reference frames. Run with one tag to see a clean process, with several to see reuse effects.
"""

import hashlib
import os
import sys

import numpy as np
import vapoursynth as vs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAMES = os.path.join(ROOT, "media", "frames")
TRI = os.path.join(ROOT, "work", "tri")
W, H = 1920, 1080

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")


def to_rgb24(frame):
    planes = [np.asarray(frame[p], dtype=np.float32) for p in range(3)]
    return np.stack([np.clip(p * 255.0 + 0.5, 0, 255).astype(np.uint8) for p in planes], axis=-1)


def read_rgb(path):
    return np.fromfile(path, dtype=np.uint8).reshape(H, W, 3)


def mae(x, y):
    return float(np.abs(x.astype(np.int16) - y.astype(np.int16)).mean()) / 255.0


for tag in sys.argv[1:]:
    src = core.resize.Bicubic(core.ffms2.Source(os.path.join(TRI, tag + ".mkv")), format=vs.RGBS)
    out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)

    s0 = to_rgb24(out.get_frame(0))
    s1 = to_rgb24(out.get_frame(1))
    a = read_rgb(os.path.join(FRAMES, tag + "_a.rgb"))
    gt = read_rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
    b = read_rgb(os.path.join(FRAMES, tag + "_b.rgb"))

    print("FP %s slot0 md5=%s slot1 md5=%s" % (
        tag, hashlib.md5(s0.tobytes()).hexdigest()[:12],
        hashlib.md5(s1.tobytes()).hexdigest()[:12]))
    print("FP   slot0-vs-a=%.6f slot1-vs-gt=%.6f slot1-vs-b=%.6f slot1-vs-a=%.6f" % (
        mae(s0, a), mae(s1, gt), mae(s1, b), mae(s1, a)))
