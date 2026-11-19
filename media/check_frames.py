"""Sums of the reference frames: is the material sane, or did some groups come out black?"""

import os

import numpy as np

MEDIA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAMES = os.path.join(MEDIA, "media", "frames")
W, H = 1920, 1080

for name in sorted(os.listdir(FRAMES)):
    if not name.endswith(".rgb"):
        continue
    a = np.fromfile(os.path.join(FRAMES, name), dtype=np.uint8)
    if a.size != W * H * 3:
        print("%s: size %d (unexpected)" % (name, a.size))
        continue
    a = a.reshape(H, W, 3).astype(np.float32) / 255.0
    print("%s: mean=%.4f std=%.4f" % (name, a.mean(), a.std()))
