"""统计各检测器在真实样本上的触发密度（每 100 帧多少次），用于给阈值选型。

触发密度比 F1 更能说明问题：伪标注本身受动作/特效污染，密度能直接对照
"正常剪辑节奏"（一般 1~5 次/100 帧）。
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scene_algos as A  # noqa: E402
import real_data_bench as B  # noqa: E402


def density(flags: set[int], n: int) -> float:
    return len(flags) / max(n, 1) * 100.0


def main() -> int:
    print(f"{'样本':<14} {'帧':>4} {'plugin .15':>11} {'hog .10':>8} {'hog .12':>8} "
          f"{'hog .15':>8} {'mvr .05':>8} {'hog|mvr':>8}")
    for label, path, start in B.SAMPLES:
        if not Path(path).exists():
            continue
        try:
            code, _ = B.decode_window(path, start, 300)
        except Exception as exc:  # noqa: BLE001
            print(f"{label:<14} [失败] {exc}")
            continue
        n = len(code)
        pla = {i for i in range(n - 1) if B.plugin_mae(code, i, i + 1, remove_mean=True) >= 0.15}
        hog = {i: A.hog_distance(code, i, i + 1) for i in range(n - 1)}
        mv = {i: A.mv_field(code, i, i + 1) for i in range(n - 1)}
        mvr = {i for i, v in mv.items() if v[0] >= 0.05}
        h10 = {i for i, v in hog.items() if v >= 0.10}
        h12 = {i for i, v in hog.items() if v >= 0.12}
        h15 = {i for i, v in hog.items() if v >= 0.15}
        union = h12 | mvr
        print(f"{label:<14} {n:>4} {density(pla, n):>11.1f} {density(h10, n):>8.1f} "
              f"{density(h12, n):>8.1f} {density(h15, n):>8.1f} {density(mvr, n):>8.1f} "
              f"{density(union, n):>8.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
