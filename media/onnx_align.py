r"""Find where the ONNX model's padded output sits relative to a 1920x1080 frame.

Feeds the same frame twice (img0 == img1 == a): a good interpolator should return that frame.
Then scans vertical offsets so we know whether to crop from the top or the centre. 1080 % 32
is 24, so the model returns 1088 rows and one of the two crops is off by 4 px.

Usage:
    python onnx_align.py --ckpt ...\ifrnet-vimeo.onnx --case t07
"""

import argparse
import os

import numpy as np
import onnxruntime as ort

MEDIA = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(MEDIA, "frames")
W, H = 1920, 1080


def load_rgb(path):
    return np.fromfile(path, dtype=np.uint8).reshape(H, W, 3).astype(np.float32) / 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--case", default="t07")
    args = ap.parse_args()

    a = load_rgb(os.path.join(FRAMES, args.case + "_a.rgb"))
    ta = a.transpose(2, 0, 1)[None].astype(np.float32)

    sess = ort.InferenceSession(args.ckpt, providers=["CPUExecutionProvider"])
    ins = sess.get_inputs()
    feed = {ins[0].name: ta, ins[1].name: ta}
    if len(ins) > 2:
        feed[ins[2].name] = np.array([0.5], dtype=np.float32)
    out = sess.run([sess.get_outputs()[0].name], feed)[0]
    img = np.clip(out[0].transpose(1, 2, 0), 0.0, 1.0)
    print("output shape", img.shape)

    for off in range(0, img.shape[0] - H + 1):
        mae = float(np.abs(img[off:off + H] - a).mean())
        print("rows %4d..%4d  mae=%.6f" % (off, off + H, mae))


if __name__ == "__main__":
    main()
