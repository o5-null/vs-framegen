r"""Run VFI-gui's own RIFE implementation on the ten Sintel triplets, for a like-for-like
comparison against the DLSS-G plugin.

Each case is (a, b) with the true middle frame gt, so the model has to invent the frame DLSS-G
invents. Writes the result as raw rgb24 into work/vfigui_<tag>_tNN.rgb and reports MAE and VMAF
against gt, plus the per-frame GPU time.

Usage:
    python rife_vfigui.py --version 4.7 --ckpt ..\VFI\VFI-gui\models\rife\rife47.pth --tag rife47
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
WORK = os.path.join(ROOT, "work")
W, H = 1920, 1080

VFIGUI = r"E:\code\VFI\VFI-gui"


def load_rgb(path):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size != W * H * 3:
        raise SystemExit("%s has %d bytes, expected %d" % (path, data.size, W * H * 3))
    return data.reshape(H, W, 3).astype(np.float32) / 255.0


def save_rgb(path, rgb):
    np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8).tofile(path)


def vmaf(test_path, ref_path):
    log_name = "vfigui_vmaf.json"
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
    ap.add_argument("--version", default="4.7", help="VFI-gui model version string, e.g. 4.7 or 4.26")
    ap.add_argument("--ckpt", default="", help="checkpoint path; defaults to the version map")
    ap.add_argument("--tag", default="", help="output tag, defaults to the version")
    ap.add_argument("--ensemble", action="store_true")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--iters", type=int, default=3, help="timing repetitions per case")
    args = ap.parse_args()

    tag = args.tag or ("rife" + args.version.replace(".", ""))
    sys.path.insert(0, VFIGUI)
    import torch
    from core.torch_backend.vfi_torch.base import VFIConfig
    from core.torch_backend.vfi_torch.rife import RIFEModel

    cfg = VFIConfig(
        model_version=args.version,
        fp16=True,
        ensemble=args.ensemble,
        fast_mode=args.fast,
    )
    model = RIFEModel(cfg)
    model.load_model(args.ckpt)
    print("loaded %s arch=%s ckpt=%s" % (args.version, model.arch_ver, args.ckpt or "(map)"))

    names = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    timings = []
    for name in names:
        a = load_rgb(os.path.join(FRAMES, name + "_a.rgb"))
        b = load_rgb(os.path.join(FRAMES, name + "_b.rgb"))
        gt = load_rgb(os.path.join(FRAMES, name + "_gt.rgb"))

        ta = torch.from_numpy(a.transpose(2, 0, 1)).unsqueeze(0)
        tb = torch.from_numpy(b.transpose(2, 0, 1)).unsqueeze(0)

        best = None
        for it in range(args.iters):
            torch.cuda.synchronize()
            t0 = time.perf_counter()
            out = model.interpolate(ta, tb, timestep=0.5).frame
            torch.cuda.synchronize()
            dt = (time.perf_counter() - t0) * 1000.0
            best = dt if best is None else min(best, dt)
            if it == 0:
                gen = out.float().clamp(0, 1).squeeze(0).permute(1, 2, 0).cpu().numpy()
        timings.append(best)

        out_path = os.path.join(WORK, "vfigui_%s_%s.rgb" % (tag, name))
        save_rgb(out_path, gen)
        mae = float(np.abs(gen - gt).mean())
        score = vmaf(out_path, os.path.join(FRAMES, name + "_gt.rgb"))
        print("%s: mae=%.6f vmaf=%.2f  %.1f ms" % (name, mae, score, best))
        rows.append((name, mae, score, best))

    n = len(rows)
    print("---- %s mean: mae=%.6f vmaf=%.2f  %.1f ms ----" % (
        tag, sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n, sum(r[3] for r in rows) / n))


if __name__ == "__main__":
    main()
