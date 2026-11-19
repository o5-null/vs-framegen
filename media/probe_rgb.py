"""Where does the 3x3 mosaic come from?

The decoder's luma plane is a normal picture, so the damage happens between the YUV frame and the
RGB24 frame (or in the dump). This reads the RGB24 frame in-process, using its real stride, and
compares geometry against the known-good ffmpeg extraction.
"""

import os

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")
MP4 = os.path.join(MEDIA, "sintel_trailer_1080p.mp4")

core = vs.core


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean())


def as_packed(frame, width, height):
    """RGB24 lives in one plane; honour its row stride instead of assuming width*3."""
    buf = frame[0]
    row = buf.strides[0]
    arr = np.frombuffer(buf, dtype=np.uint8).reshape(height, row)
    return arr[:, : width * 3].reshape(height, width, 3)


def main():
    clip = core.std.Trim(core.ffms2.Source(MP4), 120, 120)
    rgb = core.resize.Bicubic(clip, format=vs.RGB24, matrix_in_s="709")
    f = rgb.get_frame(0)
    row = f[0].strides[0]
    print("RGB24 frame: w=%d h=%d row bytes=%d (packed would be %d)" % (
        rgb.width, rgb.height, row, rgb.width * 3))

    view = as_packed(f, rgb.width, rgb.height)
    a = np.fromfile(os.path.join(FRAMES, "t00_a.rgb"), dtype=np.uint8).reshape(1080, 1920, 3)
    print("vs ffmpeg extraction: MAE=%.3f" % mae(view, a))
    print("vertical thirds identical? %.3f" % mae(view[:360], view[360:720]))
    print("horizontal thirds identical? %.3f" % mae(view[:, :640], view[:, 640:1280]))

    # Is a tile just the whole picture, shrunk? Then the frame holds 9 copies of it.
    small = core.resize.Bicubic(rgb, format=vs.RGB24, width=320, height=180)
    tile = small.get_frame(0)
    small_view = as_packed(tile, 320, 180).astype(np.float32)
    crop = view[90:270, 160:480].astype(np.float32)
    print("tile vs 3x-downscaled whole: MAE=%.3f" % mae(crop, small_view))

    # What the dump actually wrote, and what it looks like when read back with the real stride.
    raw = np.fromfile(os.path.join(TRI, "probe_709.rgb"), dtype=np.uint8)
    print("probe_709.rgb: %d bytes, vs in-process view MAE=%.3f" % (raw.size, mae(raw, view)))


if __name__ == "__main__":
    main()
