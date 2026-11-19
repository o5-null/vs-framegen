"""RIFE 4.22 (TensorRT FP16) interpolation pass over 测试数据\1080p.

Single-input 7-channel ONNX: [A(3), B(3), t(1)], t=0 returns A and t=1 returns B (verified), so
t=0.5 is the midpoint that 0002, 0004, ... stand for. Height is padded to a multiple of 32 for the
engine and cropped back afterwards.
Output: results\rife422\frames\<gt>.png (16-bit) plus results\rife422\timing.json.
"""

import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd
from trt_rife422 import ENGINE_DIR, TrtRunner

ONNX = r"E:\code\VFI\VFI-gui\models\rife_v2\rife_v4.22.onnx"
ENGINE = os.path.join(ENGINE_DIR, "rife422_1080p_fp16.engine")
WIDTH, HEIGHT = 1920, 1080
PAD_H = 1088  # 1080 rounded up to a multiple of 32


def pad_rows(img):
    """Replicate the last row: a constant t=0.5 extrapolation is the least surprising filler."""
    return np.concatenate([img, np.repeat(img[-1:], PAD_H - img.shape[0], axis=0)], axis=0)


def main():
    if not os.path.exists(ENGINE):
        raise SystemExit("engine missing, run probe_trt_rife422.py first: " + ENGINE)
    runner = TrtRunner(ENGINE, (1, 7, PAD_H, WIDTH))
    print("engine:", os.path.basename(ENGINE), "io:", runner.dtype, "out:", runner.out_shape)

    feed = np.empty((1, 7, PAD_H, WIDTH), np.float32)
    feed[0, 6] = 0.5  # timestep plane; it never changes

    timing = []
    for k, (a_path, gt, b_path) in enumerate(vd.triplets()):
        a = pad_rows(vd.read_rgb(a_path))  # HxWx3 RGB
        b = pad_rows(vd.read_rgb(b_path))
        feed[0, 0:3] = a.transpose(2, 0, 1)
        feed[0, 3:6] = b.transpose(2, 0, 1)

        t0 = time.perf_counter()
        out = runner.run(feed)
        elapsed = (time.perf_counter() - t0) * 1000.0

        rgb = out[0, 0:3, :HEIGHT].permute(1, 2, 0).float().cpu().numpy()
        vd.write_rgb16(vd.frame_path("rife422", gt), np.clip(rgb, 0.0, 1.0))
        timing.append({"gt": vd.gt_name(gt), "wall_ms": round(elapsed, 2)})
        print("%s  %.1f ms" % (vd.gt_name(gt), elapsed))

    os.makedirs(os.path.join(vd.RESULTS, "rife422"), exist_ok=True)
    with open(os.path.join(vd.RESULTS, "rife422", "timing.json"), "w", encoding="utf-8") as fh:
        json.dump(timing, fh, indent=1)
    walls = [t["wall_ms"] for t in timing]
    print("wall ms: min %.1f median %.1f max %.1f" %
          (min(walls), sorted(walls)[len(walls) // 2], max(walls)))


if __name__ == "__main__":
    main()
