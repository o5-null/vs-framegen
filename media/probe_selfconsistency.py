"""Self-consistency for a PyTorch-path model: feed frame a as both inputs.

An interpolator with correct padding returns a almost exactly. A model that zero-pads a 1080-row
frame up to 1088 rows (as IFRNet's ONNX does) smears that black band back in, and the residual
shows it. This decides whether a model's MAE/VMAF is trustworthy or an artefact.

Usage:
    python probe_selfconsistency.py --model film  --ckpt ...\film_net_fp32.pt
    python probe_selfconsistency.py --model rife  --ckpt ...\rife47.pth --version 4.7
"""

import argparse
import os
import sys

import numpy as np

MEDIA = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(MEDIA, "frames")
W, H = 1920, 1080

MODELS = {
    "rife": ("rife", "RIFEModel"),
    "film": ("film", "FILMModel"),
    "ifrnet": ("ifrnet", "IFRNetModel"),
    "amt": ("amt", "AMTModel"),
}


def load_rgb(path):
    return np.fromfile(path, dtype=np.uint8).reshape(H, W, 3).astype(np.float32) / 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="film", choices=sorted(MODELS))
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--version", default="4.7")
    ap.add_argument("--case", default="t07")
    args = ap.parse_args()

    sys.path.insert(0, r"E:\code\VFI\VFI-gui")
    import torch
    from core.torch_backend.vfi_torch.base import VFIConfig, ModelType

    sub, cls_name = MODELS[args.model]
    module = __import__("core.torch_backend.vfi_torch.%s" % sub, fromlist=[cls_name])
    cls = getattr(module, cls_name)
    model = cls(VFIConfig(model_type=getattr(ModelType, args.model.upper()),
                          model_version=args.version))
    model.load_model(args.ckpt)

    img = load_rgb(os.path.join(FRAMES, args.case + "_a.rgb"))
    # RIFE moves its inputs itself; every other model expects them on the weight device already.
    t = torch.from_numpy(img.transpose(2, 0, 1)).unsqueeze(0).to("cuda:0")
    with torch.no_grad():
        out = model.interpolate(t, t, 0.5).frame
    gen = out.float().clamp(0, 1).squeeze(0).permute(1, 2, 0).cpu().numpy()

    print("case=%s out shape=%s" % (args.case, gen.shape))
    if gen.shape[:2] != (H, W):
        print("model returned %s, not %dx%d -> its own padding leaked into the output"
              % (gen.shape[:2], H, W))
        return
    print("self-interpolation mae=%.6f (0 would be perfect)" % float(np.abs(gen - img).mean()))

    # Where is the residual concentrated? A padding artefact sits in a band at the edges.
    rows = np.abs(gen - img).mean(axis=(1, 2))
    print("row-residual: first 8 %.5f | last 8 %.5f | middle 64 %.5f"
          % (rows[:8].mean(), rows[-8:].mean(), rows[H // 2 - 32:H // 2 + 32].mean()))


if __name__ == "__main__":
    main()
