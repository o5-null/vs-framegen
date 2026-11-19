r"""Probe the input normalisation an ONNX VFI model expects.

The ifrnet-*.onnx numbers looked far worse than the same family should be, so before trusting
them: run one triplet under three candidate ranges (0..1, -1..1, 0..255) and report MAE for each.
Best MAE wins; the rest are wrong.

Usage:
    python onnx_norm_probe.py --ckpt ...\ifrnet-vimeo.onnx --case t00
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


def centre_crop(img):
    ph = max(0, img.shape[0] - H) // 2
    pw = max(0, img.shape[1] - W) // 2
    return img[ph:ph + H, pw:pw + W]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--case", default="t00")
    args = ap.parse_args()

    a = load_rgb(os.path.join(FRAMES, args.case + "_a.rgb"))
    b = load_rgb(os.path.join(FRAMES, args.case + "_b.rgb"))
    gt = load_rgb(os.path.join(FRAMES, args.case + "_gt.rgb"))

    sess = ort.InferenceSession(args.ckpt, providers=["CPUExecutionProvider"])
    ins = sess.get_inputs()
    name_a, name_b = ins[0].name, ins[1].name
    name_t = ins[2].name if len(ins) > 2 else None
    out_name = sess.get_outputs()[0].name

    ta0 = a.transpose(2, 0, 1)[None].astype(np.float32)
    tb0 = b.transpose(2, 0, 1)[None].astype(np.float32)

    variants = {
        "0..1": (ta0, tb0),
        "-1..1": (ta0 * 2.0 - 1.0, tb0 * 2.0 - 1.0),
        "0..255": (ta0 * 255.0, tb0 * 255.0),
    }
    for label, (ta, tb) in variants.items():
        feed = {name_a: ta, name_b: tb}
        if name_t:
            feed[name_t] = np.array([0.5], dtype=np.float32)
        out = sess.run([out_name], feed)[0]
        img = out[0].transpose(1, 2, 0)
        # Undo the same range on the output before comparing.
        if label == "-1..1":
            img = (img + 1.0) * 0.5
        elif label == "0..255":
            img = img / 255.0
        img = centre_crop(np.clip(img, 0.0, 1.0))
        mae = float(np.abs(img - gt).mean())
        print("%-8s mae=%.6f  out range [%.3f, %.3f]" % (label, mae, float(img.min()), float(img.max())))


if __name__ == "__main__":
    main()
