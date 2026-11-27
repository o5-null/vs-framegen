"""对全部样本做一次短窗口硬件解码冒烟测试：验证各编码/位深/分辨率都能解出来。

不写缓存，只测 30 帧，打印耗时与亮度范围。
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import real_data_bench as B  # noqa: E402


def main() -> int:
    print(f"{'样本':<14} {'分辨率':<12} {'码流':<14} {'帧':>4} {'解码s':>7} {'ms/帧':>7} {'Ymin':>5} {'Ymax':>5}")
    for label, path, start in B.SAMPLES:
        if not Path(path).exists():
            print(f"{label:<14} 文件不存在：{path}")
            continue
        try:
            info = B.probe(path)
            t0 = time.perf_counter()
            code = B._decode_ffmpeg(path, start, 30, info, B.HWACCEL)
            dt = time.perf_counter() - t0
            print(f"{label:<14} {info['w']}x{info['h']:<7} {info['pix_fmt']:<14} {len(code):>4} "
                  f"{dt:>7.2f} {dt / max(len(code), 1) * 1000:>7.1f} "
                  f"{int(code.min()):>5} {int(code.max()):>5}")
        except Exception as exc:  # noqa: BLE001
            print(f"{label:<14} [失败] {exc}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
