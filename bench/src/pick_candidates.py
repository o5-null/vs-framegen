"""列出候选边界（plugin MAE 高但也可以看 HOG 差异），用于人工标注真切镜。

用法：python pick_candidates.py --sample 4 --frames 300 --top 12
输出按 plugin-new 降序的边界及 HOG 距离，供挑图核对。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scene_algos as A  # noqa: E402
import real_data_bench as B  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", type=int, required=True)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    label, path, start = B.SAMPLES[args.sample]
    code, _ = B.decode_window(path, start, args.frames)
    rows = []
    for i in range(len(code) - 1):
        rows.append((B.plugin_mae(code, i, i + 1, remove_mean=True), i))
    rows.sort(reverse=True)
    print(f"{label}: {'边界':>4} {'plugin-new':>10} {'hog':>7}")
    for v, i in rows[: args.top]:
        print(f"{'':>10} {i:>4} {v:>10.4f} {A.hog_distance(code, i, i + 1):>7.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
