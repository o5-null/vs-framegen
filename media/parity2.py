"""True parity check: read the plugin's float planes and interleave them ourselves.

core.resize(..., format=vs.RGB24) hands back a frame whose row stride is 1920 instead of 5760 in
this VS build, so every packed-RGB dump came out tiled. Staying in RGBS (three float planes)
sidesteps that path completely.
"""

import os

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def read_rgb(path):
    return np.fromfile(path, dtype=np.uint8).reshape(1080, 1920, 3)


def read_ppm(path):
    with open(path, "rb") as f:
        return np.frombuffer(f.read().split(b"\n", 3)[3], dtype=np.uint8).reshape(1080, 1920, 3)


def to_rgb24(frame):
    """Three float planes -> one interleaved uint8 image."""
    planes = [np.asarray(frame[p], dtype=np.float32) for p in range(3)]
    return np.stack([np.clip(p * 255.0 + 0.5, 0, 255).astype(np.uint8) for p in planes], axis=-1)


def main():
    import sys
    wanted = sys.argv[1:]
    tags = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    if wanted:
        tags = [t for t in tags if t in wanted]
    rows = []
    for tag in tags:
        clip = os.path.join(TRI, tag + ".mkv")
        src = core.resize.Bicubic(core.ffms2.Source(clip), format=vs.RGBS)
        out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)

        passthrough = to_rgb24(out.get_frame(0))
        gen = to_rgb24(out.get_frame(1))
        a = read_rgb(os.path.join(FRAMES, tag + "_a.rgb"))
        gt = read_rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
        spike = read_ppm(os.path.join(ROOT, "work", "spike_" + tag + ".ppm"))

        rows.append(mae(gen, gt))
        print("%s: in=%.6f gen-vs-gt=%.6f gen-vs-spike=%.6f gen-vs-a=%.6f" % (
            tag, mae(passthrough, a), mae(gen, gt), mae(gen, spike), mae(gen, a)))

    if rows:
        print("mean gen-vs-gt MAE=%.6f" % (sum(rows) / len(rows)))


if __name__ == "__main__":
    main()
