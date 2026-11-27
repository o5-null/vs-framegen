"""逐帧度量：亮度、帧间差、重复帧、黑帧、闪烁强度、YUV->RGB 参考换算。

全部用 numpy，输入是 frames.load_sequence() 的 (N,H,W,C) 数组。
"""

from __future__ import annotations

import numpy as np

#: 色彩矩阵的 Kr/Kb（RGB<->YUV 的标准系数）
MATRIX_COEF = {
    "601": (0.299, 0.114),
    "bt.601": (0.299, 0.114),
    "709": (0.2126, 0.0722),
    "bt.709": (0.2126, 0.0722),
    "2020": (0.2627, 0.0593),
    "bt.2020": (0.2627, 0.0593),
}


def to_unit(frames: np.ndarray) -> np.ndarray:
    """归一到 0..1 的 float32。"""
    frames = np.asarray(frames)
    if frames.dtype == np.uint16:
        return frames.astype(np.float32) / 65535.0
    return frames.astype(np.float32) / 255.0


def rgb(frames: np.ndarray) -> np.ndarray:
    """只保留前三个通道（BGRA/RGBA 之类丢掉 alpha）。"""
    return frames[..., :3] if frames.shape[-1] >= 3 else frames


def luma(frames: np.ndarray, coef: str = "709") -> np.ndarray:
    """逐帧 RGB 亮度均值，返回 (N,) 。

    灰度输入直接按灰度处理；alpha 会被丢掉。
    """
    f = to_unit(rgb(frames))
    if f.ndim == 3:  # (N,H,W) 灰度
        return f.reshape(f.shape[0], -1).mean(axis=1)
    kr, kb = MATRIX_COEF.get(coef, MATRIX_COEF["709"])
    kg = 1.0 - kr - kb
    w = np.array([kr, kg, kb], dtype=np.float32)
    return (f @ w).reshape(f.shape[0], -1).mean(axis=1)


def channel_means(frames: np.ndarray) -> np.ndarray:
    """逐帧每通道均值 (N,C)，用来抓色偏/亮度抽动。"""
    f = to_unit(frames)
    c = f.shape[-1] if f.ndim == 4 else 1
    flat = f.reshape(f.shape[0], -1, c)
    return flat.mean(axis=1)


def deltas(frames: np.ndarray) -> np.ndarray:
    """相邻帧平均绝对差，返回 (N-1,) 的 0..1 数值。

    大分辨率时按 stride 抽样，只影响绝对精度，不影响相对趋势。
    """
    f = to_unit(frames)
    n = f.shape[0]
    if n < 2:
        return np.zeros(0, dtype=np.float32)
    # 抽样：每帧最多看 ~200k 个样本
    step = max(1, int(np.prod(f.shape[1:]) // 200_000))
    flat = f.reshape(n, -1)[:, ::step]
    return np.abs(np.diff(flat, axis=0)).mean(axis=1)


def duplicate_frames(frames: np.ndarray, *, tol: float = 0.0) -> list[int]:
    """返回与前一帧几乎完全相同的帧序号（从 1 开始）。

    ``tol=0`` 表示逐字节相同（PNG 无损时成立）；补帧链路里出现静止段时用它排除误报。
    """
    f = to_unit(frames)
    if f.shape[0] < 2:
        return []
    step = max(1, int(np.prod(f.shape[1:]) // 200_000))
    flat = f.reshape(f.shape[0], -1)[:, ::step]
    diff = np.abs(np.diff(flat, axis=0))
    same = diff.max(axis=1) <= tol
    return [i + 1 for i, hit in enumerate(same) if hit]


def black_frames(frames: np.ndarray, *, threshold: float = 0.02) -> list[int]:
    """亮度低于 threshold（0..1）判为黑帧。"""
    return [i for i, y in enumerate(luma(frames)) if y < threshold]


def flicker_report(frames: np.ndarray) -> dict[str, float]:
    """亮度时间轴的一阶/二阶差分统计。

    ``ratio`` = 最大二阶差分 / 中位二阶差分：稳定序列接近个位数，
    抽帧错位、重复帧、亮度泵动会把它顶到几十以上。
    """
    y = luma(frames)
    if y.size < 3:
        return {"median_delta": 0.0, "max_delta": 0.0, "ratio": 0.0, "range": float(y.max() - y.min()) if y.size else 0.0}
    d1 = np.abs(np.diff(y))
    d2 = np.abs(np.diff(y, n=2))
    med = float(np.median(d2)) or 1e-6
    return {
        "median_delta": float(np.median(d1)),
        "max_delta": float(d1.max()),
        "ratio": float(d2.max() / med),
        "range": float(y.max() - y.min()),
    }


def yuv_to_rgb(y: float, cb: float, cr: float, *, matrix: str = "709", rng: str = "limited") -> tuple[float, float, float]:
    """8bit YUV 参考换算到 0..255 RGB，用于判断 mpv 用了哪个矩阵/范围。

    与 zimg 的实现同样按「先归一化、再线性反算、最后裁剪」处理。
    """
    kr, kb = MATRIX_COEF[matrix]
    kg = 1.0 - kr - kb
    if rng == "limited":
        y = (y - 16.0) / 219.0
        cb = (cb - 128.0) / 224.0
        cr = (cr - 128.0) / 224.0
    else:
        y = y / 255.0
        cb = (cb - 128.0) / 255.0
        cr = (cr - 128.0) / 255.0
    r = y + 2.0 * (1.0 - kr) * cr
    b = y + 2.0 * (1.0 - kb) * cb
    g = (y - kr * r - kb * b) / kg
    return tuple(float(np.clip(v, 0.0, 1.0) * 255.0) for v in (r, g, b))


def summarize(frames: np.ndarray) -> dict:
    """给产物留一份可读的度量摘要。"""
    rep = flicker_report(frames)
    return {
        "frames": int(frames.shape[0]),
        "size": [int(frames.shape[2]), int(frames.shape[1])],
        "dtype": str(frames.dtype),
        "luma_first": round(float(luma(frames)[0]), 4),
        "luma_last": round(float(luma(frames)[-1]), 4),
        "duplicates": duplicate_frames(frames),
        "black": black_frames(frames),
        **{k: round(v, 5) for k, v in rep.items()},
    }
