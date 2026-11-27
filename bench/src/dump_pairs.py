"""把若干"边界帧对"导出成上下拼接的 PNG，供人工确认是不是真切镜。

用法：
  python dump_pairs.py --sample 4 --start 1000 --frames 300 --bounds 224 225 230

每对写一张图（帧 i 在上、i+1 在下），降采样到 1/3 宽，输出到 WORK/pairs/。
没有 Pillow，就先写 8bit PGM 再用 ffmpeg 转 PNG。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import real_data_bench as B  # noqa: E402


def to_gray8(y: np.ndarray) -> np.ndarray:
    """10bit limited（64..940）→ 8bit。"""

    v = (y.astype(np.float32) - 64.0) / 876.0 * 255.0
    return np.clip(v, 0, 255).astype(np.uint8)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", type=int, required=True)
    ap.add_argument("--start", type=int, default=None)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--bounds", nargs="+", type=int, required=True)
    ap.add_argument("--stride", type=int, default=2)
    args = ap.parse_args()

    label, path, start = B.SAMPLES[args.sample]
    start = args.start if args.start is not None else start
    code, _ = B.decode_window(path, start, args.frames)
    out = B.WORK / "pairs"
    out.mkdir(parents=True, exist_ok=True)

    for i in args.bounds:
        pa = to_gray8(code[i])[:: args.stride, :: args.stride]
        pb = to_gray8(code[i + 1])[:: args.stride, :: args.stride]
        h, w = pa.shape
        stack = np.vstack([pa, np.full((4, w), 255, np.uint8), pb])
        stem = out / f"{label}_b{i}"
        pgm = stem.with_suffix(".pgm")
        with pgm.open("wb") as fh:
            fh.write(f"P5\n{w} {stack.shape[0]}\n255\n".encode())
            fh.write(stack.tobytes())
        subprocess.run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(pgm),
                        str(stem.with_suffix(".png"))], check=True)
        pgm.unlink()
        print(f"{stem.with_suffix('.png')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
