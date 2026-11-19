"""Confirm the 11-channel RIFE layout documented by vs-mlrt (RIFEMerge + get_rife_input):

    ch 0-2  : frame A, RGB in [0,1]
    ch 3-5  : frame B
    ch 6    : timepoint t in [0,1]
    ch 7-10 : horizontal / vertical meshgrid in [-1,1], then the constants
              2/(W-1) and 2/(H-1) that the model needs to turn them back into pixels

t=0 must return A, t=1 must return B, t=0.5 should land on the true middle frame.
"""
import os
import sys

import numpy as np
import onnxruntime as ort

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import common as C  # noqa: E402

MODELS = C.RIFE_ONNX
S = 256
NAMES = ["rife_v4.6", "rife_v4.25_lite", "rife_v4.26", "rife_v4.26_heavy"]


def frame(path):
    import cv2
    return cv2.resize(C.read_rgb(path), (S, S), interpolation=cv2.INTER_AREA)


def main():
    a_path, gt_path, b_path = C.triplets()[8]
    A, GT, B = frame(a_path), frame(gt_path), frame(b_path)

    gy, gx = np.mgrid[0:S, 0:S].astype(np.float32)
    grid_x = 2.0 * gx / (S - 1) - 1.0
    grid_y = 2.0 * gy / (S - 1) - 1.0

    for name in NAMES:
        sess = ort.InferenceSession("%s\\%s.onnx" % (MODELS, name), providers=["CPUExecutionProvider"])
        key = sess.get_inputs()[0].name

        def run(t):
            feed = np.zeros((1, 11, S, S), np.float32)
            feed[0, 0:3] = A.transpose(2, 0, 1)
            feed[0, 3:6] = B.transpose(2, 0, 1)
            feed[0, 6] = t
            feed[0, 7] = grid_x
            feed[0, 8] = grid_y
            feed[0, 9] = 2.0 / (S - 1)
            feed[0, 10] = 2.0 / (S - 1)
            out = sess.run(None, {key: feed})[0][0].transpose(1, 2, 0)
            return np.clip(out, 0, 1)

        o0, o5, o1 = run(0.0), run(0.5), run(1.0)
        m = lambda a, b: np.abs(a - b).mean()
        print("%-16s t0->A %.4f t1->B %.4f | t.5 vs GT %.4f  vs blend %.4f"
              % (name, m(o0, A), m(o1, B), m(o5, GT), m(o5, (A + B) / 2)))


if __name__ == "__main__":
    main()
