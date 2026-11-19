"""Probe rife_v4.22.onnx: work out the 7-channel packing, then time it on CPU.

The model takes a single NCHW tensor with 7 channels instead of three separate inputs,
so the layout has to be confirmed rather than assumed: feed timestep 0 and timestep 1 and
see which end frame comes back. Whatever channel layout returns A at t=0 and B at t=1 is
the one to use.
"""

import os
import time

import cv2
import numpy as np
import onnxruntime as ort

DATA = r"E:\code\dlssg-vfi\测试数据\1080p"
MODEL = r"E:\code\VFI\VFI-gui\models\rife_v2\rife_v4.22.onnx"
W, H = 1920, 1080


def imread_rgb(path):
    data = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(data, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit("cannot decode " + path)
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0


def to_nchw(rgb, pad_h, pad_w):
    if pad_h or pad_w:
        rgb = np.pad(rgb, ((0, pad_h), (0, pad_w), (0, 0)), mode="edge")
    return rgb.transpose(2, 0, 1)[None].astype(np.float32)


def main():
    a = imread_rgb(os.path.join(DATA, "0001.png"))
    b = imread_rgb(os.path.join(DATA, "0003.png"))
    pad_h, pad_w = (-H) % 32, (-W) % 32
    ta = to_nchw(a, pad_h, pad_w)
    tb = to_nchw(b, pad_h, pad_w)

    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 8
    sess = ort.InferenceSession(MODEL, opts, providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name

    def run(pack, label):
        feed = np.concatenate(pack, axis=1)
        t0 = time.perf_counter()
        out = sess.run(None, {name: feed})[0]
        dt = (time.perf_counter() - t0) * 1000.0
        frame = np.clip(out[0].transpose(1, 2, 0)[:H, :W], 0, 1)
        ma = float(np.abs(frame - a).mean())
        mb = float(np.abs(frame - b).mean())
        print("%s: %.0f ms  |out-A|=%.6f |out-B|=%.6f -> closer to %s"
              % (label, dt, ma, mb, "A" if ma < mb else "B"))
        return frame

    def tconst(v):
        return np.full((1, 1, ta.shape[2], ta.shape[3]), v, np.float32)

    # Layout candidates for (img0, img1, t).
    for label, pack in (
        ("[A,B,t=0]", [ta, tb, tconst(0.0)]),
        ("[A,B,t=1]", [ta, tb, tconst(1.0)]),
        ("[B,A,t=0]", [tb, ta, tconst(0.0)]),
    ):
        run(pack, label)

    # Timed mid-frame at t=0.5 with the winning layout, three runs.
    times = []
    for _ in range(3):
        feed = np.concatenate([ta, tb, tconst(0.5)], axis=1)
        t0 = time.perf_counter()
        sess.run(None, {name: feed})
        times.append((time.perf_counter() - t0) * 1000.0)
    print("cpu inference ms: %s" % ", ".join("%.0f" % t for t in times))


if __name__ == "__main__":
    main()
