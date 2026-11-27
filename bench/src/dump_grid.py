"""把多组"边界帧对"拼成 2x2 联系表（contact sheet），一张图看四个边界。

每个单元格里是帧 i（上）与帧 i+1（下）。用于人工标注真切镜。
用法：
  python dump_grid.py --sample 4 --start 1000 --frames 300 --bounds 224 241 259 261 --out g1
输出 WORK/pairs/<out>.png，单元格顺序为左上、右上、左下、右下。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import real_data_bench as B  # noqa: E402


def to_gray8(y: np.ndarray, scale: int) -> np.ndarray:
    v = (y.astype(np.float32) - 64.0) / 876.0 * 255.0
    v = np.clip(v, 0, 255).astype(np.uint8)
    return v[::scale, ::scale]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", type=int, required=True)
    ap.add_argument("--start", type=int, default=None)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--bounds", nargs="+", type=int, required=True)
    ap.add_argument("--scale", type=int, default=6)
    ap.add_argument("--out", default="grid")
    args = ap.parse_args()

    label, path, start = B.SAMPLES[args.sample]
    start = args.start if args.start is not None else start
    code, _ = B.decode_window(path, start, args.frames)

    cells = []
    order = []
    for i in args.bounds[:4]:
        pa = to_gray8(code[i], args.scale)
        pb = to_gray8(code[i + 1], args.scale)
        h, w = pa.shape
        sep = np.full((2, w), 255, np.uint8)
        cells.append(np.vstack([pa, sep, pb]))
        order.append(i)
    ch, cw = cells[0].shape
    sep2 = 255 * np.ones((ch, 2), np.uint8)
    while len(cells) < 4:
        cells.append(np.zeros((ch, cw), np.uint8))
    top = np.hstack([cells[0], sep2, cells[1]])
    bot = np.hstack([cells[2], sep2, cells[3]])
    sheet = np.vstack([top, 255 * np.ones((2, top.shape[1]), np.uint8), bot])

    out = B.WORK / "pairs" / f"{args.out}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    pgm = out.with_suffix(".pgm")
    with pgm.open("wb") as fh:
        fh.write(f"P5\n{sheet.shape[1]} {sheet.shape[0]}\n255\n".encode())
        fh.write(sheet.tobytes())
    subprocess.run(["ffmpeg", "-hide_banner", "-v", "error", "-y", "-i", str(pgm), str(out)], check=True)
    pgm.unlink()
    print(f"{out}  顺序: 左上={order[0]} 右上={order[1] if len(order)>1 else '-'} "
          f"左下={order[2] if len(order)>2 else '-'} 右下={order[3] if len(order)>3 else '-'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
