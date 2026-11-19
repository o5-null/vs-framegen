"""Control test: the passthrough frame must match the source byte for byte.

Frame 0 of FrameGen is the untouched source frame. If that one already differs from
frames\\tNN_a.rgb, the mismatch is in the measurement, not in the generation.
"""

import os
import sys

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def as_rgb(frame):
    parts = []
    for p in range(3):
        plane = np.asarray(frame[p], dtype=np.float32)
        parts.append(np.clip(plane * 255.0 + 0.5, 0, 255).astype(np.uint8).ravel())
    return np.concatenate(parts)


def main():
    tag = sys.argv[1]
    clip = os.path.join(TRI, tag + ".mkv")
    src = core.resize.Bicubic(core.ffms2.Source(clip), format=vs.RGBS)
    out = core.vdlssg.FrameGen(src, proxy_dir=os.path.join(ROOT, "work"), log=0)

    a = np.fromfile(os.path.join(FRAMES, tag + "_a.rgb"), dtype=np.uint8)
    b = np.fromfile(os.path.join(FRAMES, tag + "_b.rgb"), dtype=np.uint8)
    for idx, truth, name in ((0, a, "a"), (1, None, "gen")):
        frame = out.get_frame(idx)
        data = as_rgb(frame)
        print("%s frame %d: %d bytes, vs %s MAE=%s" % (
            tag, idx, data.size, name,
            "%.6f" % mae(data, truth) if truth is not None else "n/a"))
        with open(os.path.join(TRI, "%s_f%d.rgb" % (tag, idx)), "wb") as f:
            f.write(data.tobytes())
    print("b loaded, %d bytes" % b.size)


if __name__ == "__main__":
    main()
