r"""Run any of VFI-gui's PyTorch-path models on the ten Sintel triplets.

Same measurement as rife_vfigui.py (MAE + VMAF against the true middle frame, best-of-N GPU
time), but the model family is selectable, so DLSS-G can be compared against the whole set
VFI-gui ships rather than RIFE alone.

Only the four families with a PyTorch implementation exist: rife / film / ifrnet / amt.
Everything else in models\ (momo, m2m, xvfi, atm, stmfnet, flavr, gmfss, cain, sepconv) is
ONNX/TRT-only through core\backends\adapters, which needs engines built first.

Usage:
    python vfi_models.py --model film  --ckpt E:\code\VFI\VFI-gui\models\film\film_net_fp32.pt
    python vfi_models.py --model ifrnet --ckpt E:\code\VFI\VFI-gui\models\ifrnet\IFRNet_S_Vimeo90K.pth
    python vfi_models.py --model ifrnet --ckpt E:\code\VFI\VFI-gui\models\ifrnet\IFRNet_L_Vimeo90K.pth
    python vfi_models.py --model amt   --ckpt E:\code\VFI\VFI-gui\models\amt\amt-g.pth
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

MODELS = {
    "rife": ("rife", "RIFEModel"),
    "film": ("film", "FILMModel"),
    "ifrnet": ("ifrnet", "IFRNetModel"),
    "amt": ("amt", "AMTModel"),
}


def load_rgb(path):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size != W * H * 3:
        raise SystemExit("%s has %d bytes, expected %d" % (path, data.size, W * H * 3))
    return data.reshape(H, W, 3).astype(np.float32) / 255.0


def save_rgb(path, rgb):
    np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8).tofile(path)


def vmaf(test_path, ref_path):
    log_name = "vfi_vmaf.json"
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


def build(model_name, ckpt, args, VFIConfig, ModelType):
    """Construct and load one model from VFI-gui's registry."""
    sub, cls_name = MODELS[model_name]
    module = __import__("core.torch_backend.vfi_torch.%s" % sub, fromlist=[cls_name])
    cls = getattr(module, cls_name)
    cfg = VFIConfig(
        model_type=getattr(ModelType, model_name.upper()),
        model_version=args.version,
        fp16=args.fp16,
        ensemble=args.ensemble,
        fast_mode=args.fast,
    )
    model = cls(cfg)
    model.load_model(ckpt)
    return model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="rife", choices=sorted(MODELS))
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--version", default="4.7", help="only meaningful for rife")
    ap.add_argument("--tag", default="", help="output tag; defaults to model + checkpoint stem")
    ap.add_argument("--ensemble", action="store_true", help="rife only; no effect on the others")
    ap.add_argument("--fast", action="store_true", help="rife only")
    ap.add_argument("--fp16", action="store_true", help="rife only; others are fp32 in VFI-gui")
    ap.add_argument("--iters", type=int, default=3, help="timing repetitions per case")
    args = ap.parse_args()

    tag = args.tag or (args.model + "_" + os.path.splitext(os.path.basename(args.ckpt))[0])

    sys.path.insert(0, VFIGUI)
    import torch
    from core.torch_backend.vfi_torch.base import VFIConfig, ModelType

    model = build(args.model, args.ckpt, args, VFIConfig, ModelType)
    device = model.device
    print("loaded model=%s ckpt=%s device=%s fp16=%s"
          % (args.model, args.ckpt, device, args.fp16 or args.model != "rife"))

    names = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    for name in names:
        a = load_rgb(os.path.join(FRAMES, name + "_a.rgb"))
        b = load_rgb(os.path.join(FRAMES, name + "_b.rgb"))
        gt = load_rgb(os.path.join(FRAMES, name + "_gt.rgb"))

        # RIFE moves frames to the device itself; the others expect them there already.
        ta = torch.from_numpy(a.transpose(2, 0, 1)).unsqueeze(0).to(device)
        tb = torch.from_numpy(b.transpose(2, 0, 1)).unsqueeze(0).to(device)

        best = None
        for it in range(args.iters):
            if torch.cuda.is_available():
                torch.cuda.synchronize()
            t0 = time.perf_counter()
            out = model.interpolate(ta, tb, timestep=0.5).frame
            if torch.cuda.is_available():
                torch.cuda.synchronize()
            dt = (time.perf_counter() - t0) * 1000.0
            best = dt if best is None else min(best, dt)
            if it == 0:
                gen = out.float().clamp(0, 1).squeeze(0).permute(1, 2, 0).cpu().numpy()

        out_path = os.path.join(WORK, "vfi_%s_%s.rgb" % (tag, name))
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
