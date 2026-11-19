"""Is ffms2's frame 120 the same picture as ffmpeg's frame 120, and is it tiled?

Reads the luma plane straight from the decoder (no resize, no writer) so nothing can distort it.
Also checks whether the frame repeats itself in thirds, which is what the dumps looked like.
"""

import os

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(MEDIA, "frames")
MP4 = os.path.join(MEDIA, "sintel_trailer_1080p.mp4")

core = vs.core


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean())


def ref_luma(tag):
    rgb = np.fromfile(os.path.join(FRAMES, tag + "_a.rgb"), dtype=np.uint8)
    rgb = rgb.reshape(1080, 1920, 3).astype(np.float32)
    return (0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2])


def main():
    clip = core.ffms2.Source(MP4)
    print("format=%s size=%dx%d frames=%d" % (
        clip.format.name, clip.width, clip.height, clip.num_frames))
    ref = ref_luma("t00")
    for k in range(117, 124):
        luma = np.asarray(clip.get_frame(k)[0], dtype=np.uint8)[:, :1920]
        tiled = np.abs(luma[:, :640].astype(np.int16) - luma[:, 640:1280].astype(np.int16)).mean()
        print("frame %d: MAE vs ffmpeg luma=%7.3f  left-vs-middle third=%7.3f" % (
            k, mae(luma, ref), tiled))


if __name__ == "__main__":
    main()
