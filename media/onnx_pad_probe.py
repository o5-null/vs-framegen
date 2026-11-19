r"""Is IFRNet's ONNX residual from bad padding, or from the model itself?

Feeds img0 == img1 == a. An ideal interpolator returns a; the measured residual says how much
the model disagrees with itself. Runs three input preparations:

  raw       - 1080 rows handed to the model as-is (it pads internally, presumably with zeros)
  edgepad   - 1088 rows, extra rows replicated from the border, then crop the interior back
  1056rows  - crop the frame to a multiple of 32 (1056) and interpolate only that band

If edgepad or 1056rows collapses the residual, the earlier MAE/VMAF numbers were an artefact of
zero padding rather than a property of IFRNet.

Usage:
    python onnx_pad_probe.py --ckpt ...\ifrnet-vimeo.onnx --case t07
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


def run(sess, img):
    """Feed one frame as both inputs; return the raw model output as HWC float."""
    t = img.transpose(2, 0, 1)[None].astype(np.float32)
    ins = sess.get_inputs()
    feed = {ins[0].name: t, ins[1].name: t}
    if len(ins) > 2:
        feed[ins[2].name] = np.array([0.5], dtype=np.float32)
    out = sess.run([sess.get_outputs()[0].name], feed)[0]
    return np.clip(out[0].transpose(1, 2, 0), 0.0, 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--case", default="t07")
    args = ap.parse_args()

    a = load_rgb(os.path.join(FRAMES, args.case + "_a.rgb"))
    sess = ort.InferenceSession(args.ckpt, providers=["CPUExecutionProvider"])

    # raw: what the earlier run did
    out = run(sess, a)
    ph = max(0, out.shape[0] - H) // 2
    print("raw      out%s  centre-crop mae=%.6f" % (out.shape, float(np.abs(out[ph:ph + H] - a).mean())))
    # best vertical offset over the whole output, in case the pad is one-sided
    best = min(((float(np.abs(out[o:o + H] - a).mean()), o) for o in range(out.shape[0] - H + 1)))
    print("raw      best offset %d mae=%.6f" % (best[1], best[0]))

    # edgepad: replicate the last rows so no black band enters the model
    pad_h = (-H) % 32
    padded = np.concatenate([a, np.repeat(a[-1:], pad_h, axis=0)], axis=0)
    out = run(sess, padded)
    interior = out[:H]
    print("edgepad  pad_h=%d out%s  interior mae=%.6f" % (pad_h, out.shape, float(np.abs(interior - a).mean())))

    # 1056 rows only: input is already a multiple of 32
    band = a[:1056]
    out = run(sess, band)
    print("1056rows out%s  mae=%.6f" % (out.shape, float(np.abs(out[:1056] - band).mean())))


if __name__ == "__main__":
    main()
