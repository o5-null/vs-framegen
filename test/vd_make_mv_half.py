r"""Rebuild the DIS motion field in Encore's format: one vector per 2x2 block, 1/32 pixel steps.

Input:  results\dlssg_mv\mv\<n>.png   (16-bit, code = v*128 + 32768, u in R, v in G)
Output: results\dlssg_mv_half\mv\<n>.png (half width/height, same encoding)

Two changes are applied, both of them what the Encore estimator does at its tail (see
docs\rtx-encore-motion-estimator.md §4 sh86):
  1. 2x2 box average, so one vector covers a 2x2 pixel block;
  2. quantization to 1/32 pixel (S10.5 fixed point), which is the precision the field is stored at.

The point of feeding this variant is to separate "the field is too coarse" from "the field is
ignored": a real per-2x2 field at Encore's precision must move the output if the runtime treats
application motion vectors as its primary source.
"""

import os
import sys

import cv2
import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

IN_DIR = os.path.join(vd.RESULTS, "dlssg_mv", "mv")
OUT_DIR = os.path.join(vd.RESULTS, "dlssg_mv_half", "mv")
SCALE = 128.0   # code units per pixel
BIAS = 32768.0  # zero displacement
Q = 32.0        # Encore stores 1/32 pixel steps


def decode(path):
    """16-bit PNG -> (u, v) in pixels, float64."""
    data = np.fromfile(path, dtype=np.uint8)
    bgr = cv2.imdecode(data, cv2.IMREAD_UNCHANGED)
    if bgr is None or bgr.dtype != np.uint16:
        raise SystemExit("not a 16-bit frame: " + path)
    u = (bgr[:, :, 2].astype(np.float64) - BIAS) / SCALE
    v = (bgr[:, :, 1].astype(np.float64) - BIAS) / SCALE
    return u, v


def encode(u, v):
    """(u, v) in pixels -> 3-channel uint16 PNG content."""
    out = np.zeros(u.shape + (3,), np.uint16)
    out[:, :, 2] = np.clip(np.round(u * SCALE + BIAS), 0, 65535).astype(np.uint16)
    out[:, :, 1] = np.clip(np.round(v * SCALE + BIAS), 0, 65535).astype(np.uint16)
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    names = sorted(n for n in os.listdir(IN_DIR) if n.endswith(".png"))
    if not names:
        raise SystemExit("no source frames, run vd_make_mv.py first")
    print("frames:", len(names))

    for name in names:
        u, v = decode(os.path.join(IN_DIR, name))
        h, w = u.shape
        h2, w2 = h // 2, w // 2
        # INTER_AREA on a larger target shrinks by the exact block mean, i.e. one vector per 2x2.
        u2 = cv2.resize(u, (w2, h2), interpolation=cv2.INTER_AREA)
        v2 = cv2.resize(v, (w2, h2), interpolation=cv2.INTER_AREA)
        uq = np.round(u2 * Q) / Q
        vq = np.round(v2 * Q) / Q
        # cv2.imwrite cannot open a path with these (non-ASCII) directories: it returns False and
        # writes nothing. imencode plus tofile goes through the bytes API instead.
        ok, buf = cv2.imencode(".png", encode(uq, vq))
        if not ok:
            raise SystemExit("png encode failed: " + name)
        buf.tofile(os.path.join(OUT_DIR, name))
        if name == names[0] or name == names[-1]:
            err = np.abs(np.concatenate([(uq - u2).ravel(), (vq - v2).ravel()]))
            mag = np.hypot(u2, v2)
            print("%s: %dx%d -> %dx%d  mean|mv| %.3f px  max|mv| %.3f px  "
                  "quant err mean %.4f max %.4f px"
                  % (name, w, h, w2, h2, mag.mean(), mag.max(), err.mean(), err.max()))

    print("wrote", len(names), "frames to", OUT_DIR)


if __name__ == "__main__":
    main()
