r"""Run an ONNX VFI model on the ten Sintel triplets (onnxruntime, whatever providers exist).

VFI-gui's PyTorch path only covers rife/film cleanly: ifrnet's .pth files do not match the
architecture in core\torch_backend\vfi_torch\ifrnet (pyramid*/convblock* vs encoder.conv*), and
amt-g.pth loads but then fails forward (388 vs 260 channels). The ONNX artifacts are the only
way to get IFRNet into the comparison, so this drives them directly.

Reported time is CPU inference here (this venv's onnxruntime has no CUDA EP), so it is NOT
comparable to the DLSS-G/RIFE GPU numbers - it is only there to show the cost of measurement.

Usage:
    python onnx_vfi.py --ckpt E:\code\VFI\VFI-gui\models\ifrnet\ifrnet-vimeo.onnx --tag ifrnet_vimeo
"""

import argparse
import json
import os
import subprocess
import time

import numpy as np
import onnxruntime as ort

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
WORK = os.path.join(ROOT, "work")
W, H = 1920, 1080


def load_rgb(path):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size != W * H * 3:
        raise SystemExit("%s has %d bytes, expected %d" % (path, data.size, W * H * 3))
    return data.reshape(H, W, 3).astype(np.float32) / 255.0


def save_rgb(path, rgb):
    np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8).tofile(path)


def vmaf(test_path, ref_path):
    log_name = "onnx_vmaf.json"
    log_path = os.path.join(WORK, log_name)
    if os.path.exists(log_path):
        os.unlink(log_path)
    cmd = [
        "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24", "-i", test_path,
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24", "-i", ref_path,
        "-lavfi", "[0]format=yuv420p[a];[1]format=yuv420p[b];[a][b]libvmaf=log_fmt=json:log_path=%s"
        % log_name,
        "-f", "null", "-",
    ]
    subprocess.run(cmd, check=True, cwd=WORK)
    with open(log_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    os.unlink(log_path)
    return float(data["pooled_metrics"]["vmaf"]["mean"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--tag", default="")
    ap.add_argument("--timestep", type=float, default=0.5)
    ap.add_argument("--iters", type=int, default=1)
    args = ap.parse_args()

    tag = args.tag or os.path.splitext(os.path.basename(args.ckpt))[0]
    sess = ort.InferenceSession(args.ckpt, providers=["CPUExecutionProvider"])
    ins = sess.get_inputs()
    outs = sess.get_outputs()
    a_name = ins[0].name
    b_name = ins[1].name
    t_name = ins[2].name if len(ins) > 2 else None
    print("onnx %s in=%s,%s,%s out=%s" % (
        os.path.basename(args.ckpt), a_name, b_name, t_name, outs[0].name))

    # These graphs pad the input up to a multiple of 32 themselves and pad with zeros. A 1080-row
    # frame therefore gets 8 black rows, which the encoder sees as a hard edge and smears back
    # into the picture: measured residual against a self-interpolation drops from 4.7e-3 to 1.1e-4
    # once those rows are border-replicated instead. So pad here, then crop the result back.
    pad_h = (-H) % 32
    pad_w = (-W) % 32

    names = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    for name in names:
        a = load_rgb(os.path.join(FRAMES, name + "_a.rgb"))
        b = load_rgb(os.path.join(FRAMES, name + "_b.rgb"))
        gt = load_rgb(os.path.join(FRAMES, name + "_gt.rgb"))

        def prep(img):
            if pad_h:
                img = np.concatenate([img, np.repeat(img[-1:], pad_h, axis=0)], axis=0)
            if pad_w:
                img = np.concatenate([img, np.repeat(img[:, -1:], pad_w, axis=1)], axis=1)
            return img.transpose(2, 0, 1)[None].astype(np.float32)

        ta = prep(a)
        tb = prep(b)

        feed = {a_name: ta, b_name: tb}
        if t_name:
            feed[t_name] = np.array([args.timestep], dtype=np.float32)

        best = None
        for it in range(args.iters):
            t0 = time.perf_counter()
            out = sess.run([outs[0].name], feed)[0]
            dt = (time.perf_counter() - t0) * 1000.0
            best = dt if best is None else min(best, dt)
            if it == 0:
                gen = np.clip(out[0].transpose(1, 2, 0), 0.0, 1.0)[:H, :W]

        out_path = os.path.join(WORK, "onnx_%s_%s.rgb" % (tag, name))
        save_rgb(out_path, gen)
        mae = float(np.abs(gen - gt).mean())
        score = vmaf(out_path, os.path.join(FRAMES, name + "_gt.rgb"))
        print("%s: mae=%.6f vmaf=%.2f  %.0f ms" % (name, mae, score, best))
        rows.append((name, mae, score, best))

    n = len(rows)
    print("---- %s mean: mae=%.6f vmaf=%.2f  %.0f ms ----" % (
        tag, sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n, sum(r[3] for r in rows) / n))


if __name__ == "__main__":
    main()
