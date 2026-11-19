r"""Full-size variant of the Encore motion format.

NGX refuses a motion texture smaller than the frame (EvaluateDlssg -> 0xbad00005, see
docs\vdlssg-plugin.md), so the 2x2 grid has to be spelled out at full resolution: every 2x2 block
repeats one vector, and that vector is already quantized to 1/32 px by vd_make_mv_half.py. That is
exactly what Encore's S10.5_RG16_SINT output means, only un-replicated here.

Input:  results\dlssg_mv_half\mv\<n>.png  (960x540, code = v*128 + 32768)
Output: results\dlssg_mv_enc\mv\<n>.png   (1920x1080, same encoding, 2x2 blocks identical)
"""

import os
import sys

import cv2
import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

SCALE = 128.0
BIAS = 32768.0
SRC_DIR = os.path.join(vd.RESULTS, "dlssg_mv_half", "mv")
OUT_DIR = os.path.join(vd.RESULTS, "dlssg_mv_enc", "mv")


def read_png(path):
    """cv2.imread does not survive the non-ASCII directory; go through imdecode."""
    raw = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(raw, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit("cannot read " + path)
    return img


def encode(u, v):
    bgr = np.zeros(u.shape + (3,), dtype=np.uint16)
    bgr[:, :, 2] = np.clip(u * SCALE + BIAS, 0.0, 65535.0)
    bgr[:, :, 1] = np.clip(v * SCALE + BIAS, 0.0, 65535.0)
    return bgr


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    names = sorted(n for n in os.listdir(SRC_DIR) if n.lower().endswith(".png"))
    if not names:
        raise SystemExit("no half-size motion frames in " + SRC_DIR)

    for name in names:
        img = read_png(os.path.join(SRC_DIR, name)).astype(np.float64)
        u = (img[:, :, 2] - BIAS) / SCALE
        v = (img[:, :, 1] - BIAS) / SCALE
        # One vector per 2x2 block: nearest replication of the block average.
        uf = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)
        vf = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)
        ok, buf = cv2.imencode(".png", encode(uf, vf))
        if not ok:
            raise SystemExit("png encode failed: " + name)
        buf.tofile(os.path.join(OUT_DIR, name))

    print("frames:", len(names))
    first = read_png(os.path.join(OUT_DIR, names[0])).astype(np.float64)
    last = read_png(os.path.join(OUT_DIR, names[-1])).astype(np.float64)
    for name, img in ((names[0], first), (names[-1], last)):
        u = (img[:, :, 2] - BIAS) / SCALE
        v = (img[:, :, 1] - BIAS) / SCALE
        mag = np.hypot(u, v)
        # A 2x2 block must be constant: check that the four values of each block agree exactly.
        quad = np.stack([mag[0::2, 0::2], mag[0::2, 1::2], mag[1::2, 0::2], mag[1::2, 1::2]])
        spread = float(np.abs(quad - quad[0]).max())
        print("%s: %dx%d mean|mv| %.3f px max|mv| %.3f px block spread %.6f" %
              (name, img.shape[1], img.shape[0], float(mag.mean()), float(mag.max()), spread))


if __name__ == "__main__":
    main()
