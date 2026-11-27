"""检查某个真实样本里指定边界的各检测器分数，并导出相邻帧 PNG 供人工确认。

用法：python inspect_boundary.py <样本索引> <边界索引> [--frames N]
样本索引与 real_data_bench.SAMPLES 一致。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import scene_algos as algos  # noqa: E402
import real_data_bench as B  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sample", type=int)
    ap.add_argument("boundary", type=int)
    ap.add_argument("--frames", type=int, default=120)
    args = ap.parse_args()

    label, path, start = B.SAMPLES[args.sample]
    code, info = B.decode_window(path, start, args.frames)
    i = args.boundary
    print(f"{label}  边界 {i}（帧 {i} → {i + 1}）")
    print(f"plugin       {algos.__name__}")
    print(f"plugin-old   {B.plugin_mae(code, i, i + 1):.4f}")
    print(f"plugin-new   {B.plugin_mae(code, i, i + 1, remove_mean=True):.4f}")
    print(f"blockmax     {algos.block_max_abs_diff(code, i, i + 1):.4f}")
    print(f"hog          {algos.hog_distance(code, i, i + 1):.4f}")
    res, coh = algos.mv_field(code, i, i + 1)
    print(f"mv resid/coh {res:.4f} / {coh:.3f}")
    print("相邻边界对照：")
    for j in range(max(0, i - 2), min(len(code) - 1, i + 3)):
        r, c = algos.mv_field(code, j, j + 1)
        print(f"  {j:>4} plugin={B.plugin_mae(code, j, j + 1):.4f} hog={algos.hog_distance(code, j, j + 1):.4f} "
              f"res={r:.4f} coh={c:.3f}")

    # 导出两帧 PNG（Y 平面直接当灰度），便于用看图工具确认是否真切镜
    out = B.WORK / "frames"
    out.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image
        for k in (i, i + 1):
            y = code[k].astype(np.float32) - 64.0
            np.clip(y / 876.0 * 255.0, 0, 255, out=y)
            Image.fromarray(y.astype(np.uint8)).save(out / f"b{i}_f{k}.png")
        print(f"帧 PNG -> {out}")
    except ImportError:
        print("未安装 Pillow，跳过 PNG 导出")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
