"""外部切镜 / 重复帧算法的 numpy 复刻（不依赖 cv2 / scipy）。

对照对象（均为公开实现的算法口径）：
- PySceneDetect ContentDetector：HSV 分量加权平均差，0~255 量纲，默认阈值 27；
  本基准片源是灰阶（U/V 恒定），所以只取 luma 与 edges 两个分量。
- PySceneDetect AdaptiveDetector：content_val 与其邻域均值的比值，默认 ratio 3.0、
  min_content_val 15，用来抗快速摇镜。
- 运动补偿 MAE：先按 4x 降采样做全局平移搜索，再算补偿后的平均绝对差，抗摇镜。
- ffmpeg scdet：直接看 mafd 占最大变化的百分比（mae * 100），默认阈值 10。
- dHash / 精确校验和：重复帧判定，和逐像素 MAE 对比。

所有函数都接受 `code`：[N, H, W] uint16，10bit limited（64~940）亮度平面。
"""

from __future__ import annotations

import numpy as np

LOW, SPAN = 64.0, 876.0  # 10bit limited 的 Y 范围


def lum01(frame: np.ndarray) -> np.ndarray:
    """码值 → [0,1] 归一化亮度。"""

    return (frame.astype(np.float32) - LOW) / SPAN


# ------------------------------------------------------------ ContentDetector


def _edge_map(y: np.ndarray, thresh: float = 0.10) -> np.ndarray:
    """Sobel 梯度幅值过阈值，再做 3x3 膨胀（对应 PySceneDetect 的 dilate 边缘）。"""

    gx = np.zeros_like(y)
    gy = np.zeros_like(y)
    gx[:, :-1] = y[:, 1:] - y[:, :-1]
    gy[:-1, :] = y[1:, :] - y[:-1, :]
    edge = (np.hypot(gx, gy) > thresh).astype(np.float32)
    out = edge.copy()
    out[1:, :] = np.maximum(out[1:, :], edge[:-1, :])
    out[:-1, :] = np.maximum(out[:-1, :], edge[1:, :])
    out[:, 1:] = np.maximum(out[:, 1:], out[:, :-1])
    out[:, :-1] = np.maximum(out[:, :-1], out[:, 1:])
    return out * 255.0


def content_scores(
    code: np.ndarray, *, w_lum: float = 1.0, w_edge: float = 0.0
) -> dict[int, float]:
    """ContentDetector 的 content_val（0~255）：luma 与 edges 分量的加权平均差。

    delta_lum 用整帧平均绝对差，delta_edges 用膨胀边缘图的平均绝对差；
    权重和归一化（对应 PySceneDetect "normalized based on the weight of the components"）。
    """

    n = len(code)
    prepped: list[tuple[np.ndarray, np.ndarray | None]] = []
    for i in range(n):
        y = lum01(code[i])
        prepped.append((y, _edge_map(y) if w_edge > 0.0 else None))
    total_w = w_lum + w_edge
    scores: dict[int, float] = {}
    for i in range(n - 1):
        ya, ea = prepped[i]
        yb, eb = prepped[i + 1]
        score = w_lum * float(np.abs(ya - yb).mean()) * 255.0
        if w_edge > 0.0 and ea is not None and eb is not None:
            score += w_edge * float(np.abs(ea - eb).mean())
        scores[i] = score / total_w
    return scores


def adaptive_flags(
    scores: dict[int, float],
    *,
    window: int = 2,
    ratio: float = 3.0,
    min_content: float = 15.0,
) -> set[int]:
    """AdaptiveDetector 的判定：score / 邻域均值 >= ratio 且 score >= min_content。"""

    keys = sorted(scores)
    flags: set[int] = set()
    for idx, i in enumerate(keys):
        score = scores[i]
        if score < min_content:
            continue
        lo, hi = max(0, idx - window), min(len(keys), idx + window + 1)
        neigh = [scores[keys[k]] for k in range(lo, hi) if k != idx]
        if not neigh:
            continue
        avg = sum(neigh) / len(neigh)
        if avg < 1e-5:
            flags.add(i)
        elif min(score / avg, 255.0) >= ratio:
            flags.add(i)
    return flags


# --------------------------------------------------------- 运动补偿平均绝对差


def compensated_mae(code: np.ndarray, a: int, b: int, *, ds: int = 4, max_shift: int = 4) -> float:
    """4x 降采样后搜全局平移（±max_shift 个降采样像素），再算补偿后的归一化 MAE。"""

    ya = lum01(code[a])
    yb = lum01(code[b])
    sa = ya[: ya.shape[0] // ds * ds, : ya.shape[1] // ds * ds].reshape(
        ya.shape[0] // ds, ds, ya.shape[1] // ds, ds).mean(axis=(1, 3))
    sb = yb[: yb.shape[0] // ds * ds, : yb.shape[1] // ds * ds].reshape(
        yb.shape[0] // ds, ds, yb.shape[1] // ds, ds).mean(axis=(1, 3))
    best = None
    for dy in range(-max_shift, max_shift + 1):
        for dx in range(-max_shift, max_shift + 1):
            cand = np.roll(np.roll(sb, dy, axis=0), dx, axis=1)
            d = float(np.abs(sa - cand).mean())
            if best is None or d < best:
                best = d
    # 降采样均值不改变平均绝对差的期望，可直接和插件 MAE 比较
    return float(best if best is not None else 0.0)


def mafd_percent(mae: float) -> float:
    """ffmpeg scdet 的口径：mafd 占最大变化的百分比。"""

    return mae * 100.0


# --------------------------------------------------------- 矢量类特征（不用亮度差）


def hog_signature(frame: np.ndarray, bins: int = 9, signed: bool = False,
                  min_mag: float = 0.02) -> tuple[np.ndarray, float]:
    """梯度方向直方图（按幅值加权并归一化），返回 (直方图, 有效像素占比)。

    对整体增益/偏移不敏感：闪光是画面整体提亮，梯度方向分布基本不变，
    所以"闪光 vs 切镜"这一对用亮度差分不开，用方向分布可以分开。

    signed=False：方向折到 0~π（不区分亮暗极性），正片负片反相时距离不变；
    signed=True：方向取 0~2π，反相会让所有方向偏 π，距离被放大。
    min_mag：只统计幅值超过该值的像素（lum01 量纲），滤掉噪声；黑场/纯色帧的
    有效像素极少，直方图没有意义，用量占比让调用方跳过判定。
    """

    y = lum01(frame).astype(np.float32)
    gx = np.zeros_like(y)
    gy = np.zeros_like(y)
    gx[:, :-1] = y[:, 1:] - y[:, :-1]
    gy[:-1, :] = y[1:, :] - y[:-1, :]
    mag = np.hypot(gx, gy)
    mask = mag > min_mag
    coverage = float(mask.mean())
    span = 2.0 * np.pi if signed else np.pi
    ang = np.mod(np.arctan2(gy, gx), span)
    idx = np.clip((ang / span * bins).astype(np.int32), 0, bins - 1)
    hist = np.bincount(idx[mask], weights=mag[mask], minlength=bins)[:bins]
    total = float(hist.sum())
    hist = hist / total if total > 1e-8 else np.zeros(bins, dtype=np.float32)
    return hist, coverage


def hog_distance(code: np.ndarray, a: int, b: int, *, ds: int = 4, signed: bool = False,
                 min_coverage: float = 0.05) -> float:
    """两帧方向直方图的 L1 距离（0~2，越大越不像同一场景）。

    任一帧有效梯度像素占比低于 min_coverage（黑场、纯色、大面积渐隐）时返回 0，
    因为这种帧的方向直方图由噪声决定，不能当作切镜证据。5% 是实测门限：渐隐尾帧
    （有效梯度只有 3.4%）会被挡掉，正常画面覆盖在 60~95%。
    """

    sa, ca = hog_signature(code[a][::ds, ::ds], signed=signed)
    sb, cb = hog_signature(code[b][::ds, ::ds], signed=signed)
    if ca < min_coverage or cb < min_coverage:
        return 0.0
    return float(np.abs(sa - sb).sum() / 2.0)


def mv_field(code: np.ndarray, a: int, b: int, *, ds: int = 4, blk: int = 16, r: int = 3
             ) -> tuple[float, float]:
    """块匹配运动矢量场的两个统计量：(归一化残差中位数, 全局矢量一致性)。

    同一镜头内即使快速摇镜，各块的位移也高度一致（一致性高、补偿后残差低）；
    切镜时每块都找不到匹配，最优位移发散、残差高；闪光保留了结构，匹配仍然成立。
    这两个量都不直接比较亮度差，而是看"能不能用一组矢量把两帧对上"。
    """

    ya = lum01(code[a])
    yb = lum01(code[b])
    h, w = ya.shape[0] // ds, ya.shape[1] // ds
    sa = ya[: h * ds, : w * ds].reshape(h, ds, w, ds).mean(axis=(1, 3))
    sb = yb[: h * ds, : w * ds].reshape(h, ds, w, ds).mean(axis=(1, 3))

    bh, bw = h // blk, w // blk
    if bh < 2 or bw < 2:
        return 0.0, 1.0
    best_res = np.full((bh, bw), np.inf, dtype=np.float32)
    best_vec = np.zeros((bh, bw, 2), dtype=np.int32)
    for by in range(bh):
        for bx in range(bw):
            y0, x0 = by * blk, bx * blk
            ref = sa[y0:y0 + blk, x0:x0 + blk]
            for dy in range(-r, r + 1):
                yy = y0 + dy
                if yy < 0 or yy + blk > h:
                    continue
                for dx in range(-r, r + 1):
                    xx = x0 + dx
                    if xx < 0 or xx + blk > w:
                        continue
                    cand = sb[yy:yy + blk, xx:xx + blk]
                    res = float(np.abs(ref - cand).mean())
                    if res < best_res[by, bx]:
                        best_res[by, bx] = res
                        best_vec[by, bx] = (dy, dx)
    vecs = best_vec.reshape(-1, 2)
    # 一致性：出现次数最多的位移占全部块的比例
    uniq, counts = np.unique(vecs, axis=0, return_counts=True)
    coherence = float(counts.max() / len(vecs))
    return float(np.median(best_res)), coherence


# ------------------------------------------------------------- 重复帧判定


def dhash(frame: np.ndarray, grid: int = 8) -> int:
    """dHash：缩到 (grid+1) x grid 的块均值，比较水平相邻梯度，返回 64bit 整数。"""

    h, w = frame.shape
    by = h // grid
    bx = w // (grid + 1)
    small = frame[: by * grid, : bx * (grid + 1)].astype(np.float32)
    small = small.reshape(grid, by, grid + 1, bx).mean(axis=(1, 3))
    bits = (small[:, 1:] > small[:, :-1]).astype(np.uint64)
    out = 0
    for v in bits.ravel():
        out = (out << 1) | int(v)
    return out


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def exact_equal_ratio(a: np.ndarray, b: np.ndarray) -> float:
    """逐像素完全相同的比例（精确重复帧判定的上界）。"""

    return float((a == b).mean())


def block_max_abs_diff(code: np.ndarray, a: int, b: int, ds: int = 8) -> float:
    """分块最大绝对差：重复帧检测常用（块内最大绝对差都小才算重复），比全帧均值更敏感。"""

    ya = (code[a].astype(np.int32))
    yb = (code[b].astype(np.int32))
    d = np.abs(ya - yb)
    h, w = d.shape
    blocks = d[: h // ds * ds, : w // ds * ds].reshape(h // ds, ds, w // ds, ds).max(axis=(1, 3))
    return float(blocks.max()) / SPAN
