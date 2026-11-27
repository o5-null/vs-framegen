"""打印合成片源敏感边界上的 HOG 距离与有效梯度覆盖，用于校准门限。"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scene_algos as A  # noqa: E402
import real_data_bench as B  # noqa: E402

SRC = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\scbench\clip.mkv")


def main() -> int:
    code, _ = B.decode_window(str(SRC), 0, 240)
    print(f"{'边界':>4} {'hog':>7} {'covA':>7} {'covB':>7} {'meanA':>7} {'meanB':>7}")
    for i in (28, 29, 48, 49, 50, 70, 73, 74, 75, 76, 77, 128, 129, 138, 139, 158, 159, 178, 179):
        ya, ca = A.hog_signature(code[i][::4, ::4])
        yb, cb = A.hog_signature(code[i + 1][::4, ::4])
        hog = A.hog_distance(code, i, i + 1)
        print(f"{i:>4} {hog:>7.4f} {ca:>7.4f} {cb:>7.4f} "
              f"{code[i].mean() / 876:>7.3f} {code[i + 1].mean() / 876:>7.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
