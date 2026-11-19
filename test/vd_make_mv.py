"""Dense motion field for the DLSS-G pass, computed with OpenCV DIS.

The runtime wants mvec_curr2prev: per pixel, where that pixel sat in the previous frame, in
pixels, on the current frame's grid. Clip frame k holds the field of source frame k; frame 0 has
no predecessor, so it stays zero.

Saved as 16-bit PNG with u in R and v in G: code = v * 128 + 32768, i.e. +/-256 px at 1/128 px.
core.imwri.Read keeps 16-bit precision with float_output=1 (verified by probe_imwri16.py).
"""

import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

SCALE = 128.0    # codes per pixel: the field spans +/-256 px
BIAS = 32768.0
OUT = os.path.join(vd.RESULTS, "dlssg_mv", "mv")


def encode(flow):
    """CV float field -> 3-channel uint16 image with u in R, v in G, 0 in B."""
    q = np.clip(np.rint(flow * SCALE + BIAS), 0.0, 65535.0).astype(np.uint16)
    bgr = np.zeros(flow.shape[:2] + (3,), np.uint16)
    bgr[:, :, 2] = q[:, :, 0]  # R = horizontal
    bgr[:, :, 1] = q[:, :, 1]  # G = vertical
    return bgr


def warp_error(prev, cur, flow, sign):
    """Mean |prev(x + sign*flow) - cur(x)|: tells which direction a flow field points."""
    import cv2

    h, w = flow.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    warped = cv2.remap(prev, gx + sign * flow[:, :, 0], gy + sign * flow[:, :, 1],
                       cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    return float(np.mean(np.abs(warped.astype(np.float32) - cur.astype(np.float32))))


def main():
    import cv2

    triples = vd.triplets()
    a_only = [a for (a, _, _) in triples] + [triples[-1][2]]  # same 20 frames as vd_gen_dlssg
    dis = cv2.DISOpticalFlow_create(cv2.DISOpticalFlow_PRESET_MEDIUM)
    dis.setUseSpatialPropagation(True)
    # This build exposes no FINE preset, so push MEDIUM to its ceiling: full-resolution patch
    # field plus 12 variational refinement iterations (default 5). ~0.8 s per 1080p pair.
    dis.setFinestScale(0)
    dis.setGradientDescentIterations(25)
    dis.setVariationalRefinementIterations(12)
    os.makedirs(OUT, exist_ok=True)

    # OpenCV's DIS direction has flipped between versions, so decide the sign once on the first
    # real pair: keep whichever warp of prev lands closer to cur. The field is wanted on cur's
    # grid, hence calc(cur, prev).
    sign = None
    prev_rgb = None
    mags = []
    for i, path in enumerate(a_only):
        cur_rgb = vd.read_rgb(path)
        if prev_rgb is None:
            flow = np.zeros(cur_rgb.shape[:2] + (2,), np.float32)
        else:
            # DIS wants single-channel 8-bit; luma is what every flow estimator here sees.
            cur = vd.to_u8(vd.to_gray(cur_rgb))
            prev = vd.to_u8(vd.to_gray(prev_rgb))
            raw = dis.calc(cur, prev, None)
            if sign is None:
                plus = warp_error(prev, cur, raw, +1.0)
                minus = warp_error(prev, cur, raw, -1.0)
                sign = 1.0 if plus < minus else -1.0
                print("DIS direction: prev(x+d) err %.2f, prev(x-d) err %.2f -> sign %+.0f"
                      % (plus, minus, sign))
            flow = sign * raw
            mags.append(float(np.mean(np.hypot(flow[:, :, 0], flow[:, :, 1]))))
        ok, buf = cv2.imencode(".png", encode(flow))
        if not ok:
            raise RuntimeError("cannot encode frame %d" % (i + 1))
        buf.tofile(os.path.join(OUT, "%04d.png" % (i + 1)))
        prev_rgb = cur_rgb

    m = np.array(mags)
    print("frames: %d  mean |mv| px: %.3f  max per-frame mean: %.3f" %
          (len(a_only), m.mean(), m.max()))
    print("out:", OUT)


if __name__ == "__main__":
    main()
