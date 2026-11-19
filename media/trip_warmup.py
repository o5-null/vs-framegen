"""Is the first generated frame of a fresh session unreliable?

Each triplet is a 2-frame clip: the DLSS-G history holds exactly one frame, which is not enough
for the temporal network to know what is moving. Re-run the same pair at the END of a longer
half-rate clip so the history is warm, and compare both answers against the true middle frame.
"""

import os

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
MP4 = os.path.join(MEDIA, "sintel_trailer_1080p.mp4")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
PROXY = os.path.join(ROOT, "work")

# Same triplets as media\extract.bat: a = N, gt = N+1, b = N+2.
TRIPLETS = {t: n for t, n in zip(
    ["t%02d" % i for i in range(10)],
    [120, 200, 300, 420, 540, 660, 780, 900, 1020, 1140])}


def rgb(path):
    return np.fromfile(path, dtype=np.uint8)


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def as_rgb(frame):
    parts = []
    for p in range(3):
        plane = np.asarray(frame[p], dtype=np.float32)
        parts.append(np.clip(plane * 255.0 + 0.5, 0, 255).astype(np.uint8).ravel())
    return np.concatenate(parts)


def source():
    src = core.ffms2.Source(MP4)
    return core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")


def cold(tag, n):
    """2-frame clip: the a/b pair alone."""
    src = core.std.Trim(source(), n, n + 2)
    src = core.std.SelectEvery(src, 2, 0)
    out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)
    return as_rgb(out.get_frame(1))


def warm(tag, n, back=2):
    """Same pair at the end of a half-rate clip that starts `back` pairs earlier."""
    src = core.std.Trim(source(), n - 2 * back, n + 2)
    src = core.std.SelectEvery(src, 2, 0)
    out = core.vdlssg.FrameGen(src, proxy_dir=PROXY, log=0)
    pairs = (back + 1) * 2
    return as_rgb(out.get_frame(pairs - 1))


def main():
    for tag, n in sorted(TRIPLETS.items()):
        gt = rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
        c = cold(tag, n)
        w = warm(tag, n)
        print("%s: cold=%.6f warm=%.6f cold-vs-warm=%.6f" % (
            tag, mae(c, gt), mae(w, gt), mae(c, w)))


if __name__ == "__main__":
    main()
