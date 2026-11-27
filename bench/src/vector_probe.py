"""合成片源上的矢量类特征探针：HOG 方向距离 + 块匹配矢量场统计。

对每个标注边界与若干普通运动边界打印三个量，检查：
- 切镜是否在 HOG / 矢量残差上明显高于普通帧；
- 闪光是否在 HOG 上保持低值（结构没变，只有增益变）。
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scene_algos as A  # noqa: E402
import scene_detect_bench as B  # noqa: E402


def main() -> int:
    code, events = B.build_clip()
    print(f"{'边界':>4} {'类型':<9} {'hog':>7} {'res':>7} {'coh':>6}  说明")
    for e in events:
        i = e["at"]
        hog = A.hog_distance(code, i, i + 1)
        res, coh = A.mv_field(code, i, i + 1)
        print(f"{i:>4} {e['kind']:<9} {hog:>7.4f} {res:>7.4f} {coh:>6.3f}  {e.get('note', '')}")
    print(f"\n{'边界':>4} {'类型':<9} {'hog':>7} {'res':>7} {'coh':>6}  （普通运动边界，作基线）")
    for i in (0, 20, 60, 90, 120, 200, 235):
        hog = A.hog_distance(code, i, i + 1)
        res, coh = A.mv_field(code, i, i + 1)
        print(f"{i:>4} {'motion':<9} {hog:>7.4f} {res:>7.4f} {coh:>6.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
