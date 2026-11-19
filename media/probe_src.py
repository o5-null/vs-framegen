"""Is the source frame itself wrong on the way in, or only in my harness?

Dumps frame 120 of the mp4 through VS's own writer (resize -> RGB24) with no numpy, no ffv1
intermediate, no FrameGen. Compared against media\\frames\\t00_a.rgb, which the spike already
proved is correct.
"""

import os
import subprocess

import numpy as np

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")
VSPIPE = r"E:\code\VFI\.venv\Scripts\vspipe.exe"
MP4 = os.path.join(MEDIA, "sintel_trailer_1080p.mp4")

BODY = """
import vapoursynth as vs
core = vs.core
src = core.ffms2.Source(r"%s")
src = core.std.Trim(src, 120, 120)
src = core.resize.Bicubic(src, format=vs.RGB24%s)
src.set_output()
"""


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def run(name, extra):
    vpy = os.path.join(TRI, name + ".vpy")
    with open(vpy, "w", encoding="utf-8") as f:
        f.write(BODY % (MP4, extra))
    dst = os.path.join(TRI, name + ".rgb")
    err = os.path.join(TRI, name + ".err")
    with open(dst, "wb") as out, open(err, "wb") as errf:
        subprocess.run([VSPIPE, vpy, "-"], stdout=out, stderr=errf, check=True)
    a = np.fromfile(os.path.join(FRAMES, "t00_a.rgb"), dtype=np.uint8)
    got = np.fromfile(dst, dtype=np.uint8)
    print("%s: %d bytes vs %d, MAE=%.6f" % (name, got.size, a.size, mae(got, a)))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                    "-s", "1920x1080", "-i", dst, "-frames:v", "1",
                    os.path.join(TRI, name + ".png")], check=True)


if __name__ == "__main__":
    run("probe_709", ', matrix_in_s="709"')
    run("probe_auto", "")
