"""无符号 vs 有符号 HOG 在合成片源与真实素材边界上的对照（临时探针）。"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scene_algos as A  # noqa: E402
import real_data_bench as B  # noqa: E402
import scene_detect_bench as SB  # noqa: E402


def row(tag: str, code, i: int) -> None:
    uns = A.hog_distance(code, i, i + 1)
    sgn = A.hog_distance(code, i, i + 1, signed=True)
    print(f"{tag:>4} {i:>4}  uns={uns:.4f}  sgn={sgn:.4f}")


def main() -> int:
    code, events = SB.build_clip()
    print("--- 合成片源 ---")
    for e in events:
        if e["kind"] == "dup_run":
            continue
        row(e["kind"], code, e["at"])
    for i in (0, 20, 60, 90, 120, 200):
        row("motion", code, i)

    print("--- 真实 EP（start 600，爆闪/反相特效段） ---")
    c, _ = B.decode_window(B.SAMPLES[1][1], 600, 60)
    for i in (48, 49, 50, 51, 52, 53):
        row("ep", c, i)

    print("--- 真实番剧 TANKCHAIR（真切镜 109） ---")
    d, _ = B.decode_window(B.SAMPLES[4][1], 1000, 120)
    for i in (108, 109, 110):
        row("tank", d, i)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
