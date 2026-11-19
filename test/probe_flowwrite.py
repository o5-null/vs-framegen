"""Reproduce the flow-picture write failure with the exact call vd_flow_compare.py makes."""

import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd
import vd_flow_compare as fc

print("flow/ref entries:", sorted(os.listdir(os.path.join(vd.RESULTS, "flow", "ref")))[:5])

triples = vd.triplets()
a, gt, b = triples[0]
cur = vd.to_u8(vd.to_gray(vd.read_rgb(a)))
nxt = vd.to_u8(vd.to_gray(vd.read_rgb(b)))
dis = fc.make_dis()
flow = fc.flow_of(dis, cur, nxt)
pic = fc.flow_color(flow)
print("pic", pic.shape, pic.dtype)
path = os.path.join(vd.RESULTS, "flow", "ref", "0002_a2b.png")
print("target exists:", os.path.exists(path), "isdir:", os.path.isdir(path))
try:
    vd.write_rgb8(path, pic / 255.0)
    print("ok", os.path.getsize(path))
except OSError as exc:
    print("FAIL", exc)
    import cv2
    ok, buf = cv2.imencode(".png", np.clip(pic / 255.0 * 255.0 + 0.5, 0, 255).astype(np.uint8)[:, :, ::-1])
    print("imencode ok", ok, buf.dtype, buf.shape, buf.flags["C_CONTIGUOUS"])
