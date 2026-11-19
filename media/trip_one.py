"""One triplet per process.

The plugin keeps a single NGX session per process, so running several clips in one interpreter
means the later ones inherit the earlier ones' temporal history. This script does exactly one
triplet, so the history starts cold - the same conditions the spike ran under.
"""

import os
import sys

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))


def rgb(path):
    return np.fromfile(path, dtype=np.uint8)


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def read_ppm(path):
    with open(path, "rb") as f:
        return np.frombuffer(f.read().split(b"\n", 3)[3], dtype=np.uint8)


def as_rgb(frame):
    parts = []
    for p in range(3):
        plane = np.asarray(frame[p], dtype=np.float32)
        parts.append(np.clip(plane * 255.0 + 0.5, 0, 255).astype(np.uint8).ravel())
    return np.concatenate(parts)


def main():
    tag = sys.argv[1]
    clip = os.path.join(ROOT, "work", "tri", tag + ".mkv")
    src = core.resize.Bicubic(core.ffms2.Source(clip), format=vs.RGBS)
    out = core.vdlssg.FrameGen(src, proxy_dir=os.path.join(ROOT, "work"), log=0)
    f = out.get_frame(1)
    gen = as_rgb(f)
    gt = rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
    a = rgb(os.path.join(FRAMES, tag + "_a.rgb"))
    spike = read_ppm(os.path.join(ROOT, "work", "spike_" + tag + ".ppm"))
    print("%s: fallback=%s gen-vs-gt=%.6f gen-vs-spike=%.6f gen-vs-a=%.6f a-vs-gt=%.6f" % (
        tag, f.props.get("_DLSSGFallback"), mae(gen, gt), mae(gen, spike), mae(gen, a),
        mae(a, gt)))


if __name__ == "__main__":
    main()
