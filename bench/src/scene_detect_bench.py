"""切镜 / 重复帧检测对照：插件采样 MAE vs k7f SCDetect2（algo 1-4）。

做法：
1. 生成一段 10bit YUV420 合成片源，每个镜头的纹理、相位都不同，并埋入带标注的事件：
   硬切、闪光、淡出后硬切、曝光渐变、精确重复帧、近重复帧（±1 码值）、快速运动、
   同亮度但结构完全不同的硬切、重复帧连跑。
2. 插件那一套（`frame_gen.cpp:isSceneCut`）按源码逐行用 numpy 复刻两个版本：
   old = 原样平均绝对差，new = 同一批采样点减掉该对均值后的平均绝对差。
3. k7f 那一套直接跑真代码：vspipe 里 `k7f.SCDetect2(clip, algo=..)`，读 `_SceneChangeNext`。
4. 按标注统计命中/误报/漏报，并给出重复帧判定可用性（各检测器在重复帧边界与正常边界上的
   原始差值分布，看有没有一个共同阈值能分开）。

用法：
    python scene_detect_bench.py            # 全部（含 vspipe，约 10s）
    python scene_detect_bench.py --no-vs    # 只跑 numpy 复刻
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_algos as algos  # noqa: E402  （外部算法复刻，见 scene_algos.py）

ROOT = Path(r"E:\code\dlssg-vfi")
VSPIPE = ROOT / ".venv" / "Scripts" / "vspipe.exe"
K7SRC = ROOT / "k7sfunc" / "src"
WORK = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\scbench")

W, H, FRAMES = 640, 360, 240
STEP = 8  # 插件采样步长（frame_gen.cpp 常量）


# ---------------------------------------------------------------- 片源生成


def _texture(shot: int, t: int, motion: float = 1.0) -> np.ndarray:
    """一个镜头的亮度（0~1）：不同镜头用不同频率结构，并随帧相位移动。"""

    rng = np.random.default_rng(1000 + shot)
    x = np.arange(W, dtype=np.float32)[None, :]
    y = np.arange(H, dtype=np.float32)[:, None]
    phase = t * 1.0 * motion
    if shot == 0:      # 低频斜纹
        tex = 0.5 + 0.16 * np.sin((x * 0.9 + y * 0.5 + phase) * 0.010)
    elif shot == 1:    # 低频竖条
        tex = 0.5 + 0.16 * np.sin((x + phase) * 0.10)
    elif shot == 2:    # 低频斜向纹理
        tex = 0.5 + 0.16 * np.sin((x * 1.7 - y * 1.3 + phase) * 0.028)
    else:             # 低频横条（与 shot2 亮度相同、结构正交）
        tex = 0.5 + 0.16 * np.sin((y + phase * 0.4) * 0.11)
    cx = W * (0.5 + 0.3 * np.sin(0.11 * t + shot))
    cy = H * (0.5 + 0.3 * np.cos(0.09 * t + shot))
    blob = 0.22 * np.exp(-(((x - cx) ** 2 + (y - cy) ** 2) / (2.0 * 60.0 ** 2)))
    noise = rng.normal(0.0, 0.02, size=(H, W)).astype(np.float32)
    return np.clip(tex + blob + noise, 0.0, 1.0)


def build_clip() -> tuple[np.ndarray, list[dict]]:
    """返回 (frames, events)。frames 是 [N, H, W] uint16 的 Y 平面（10bit limited）。"""

    events: list[dict] = []
    # 镜头起点与整体亮度：硬切处既有结构变化也有亮度台阶，179 那条切刻意保持同亮度
    shot_start = {0: 0, 1: 30, 2: 78, 3: 180}
    shot_level = {0: 0.45, 1: 0.60, 2: 0.35, 3: 0.35}
    breaks = [(0, 0), (30, 1), (78, 2), (180, 3)]

    luma = np.zeros((FRAMES, H, W), dtype=np.float32)
    for i in range(FRAMES):
        shot = max(s for b, s in breaks if b <= i)
        motion = 8.0 if 160 <= i < 180 else 1.0
        base = _texture(shot, i - shot_start[shot], motion)
        # 镜头间只差亮度偏移，结构差异留在纹理里
        luma[i] = np.clip(base + (shot_level[shot] - 0.5), 0.0, 1.0)

    events += [
        {"at": 29, "kind": "cut", "note": "硬切 0→1（含亮度台阶）"},
        {"at": 49, "kind": "flash", "note": "闪光 +60%"},
        {"at": 69, "kind": "draw", "note": "无关边界（同镜头）"},
        {"at": 77, "kind": "cut", "note": "淡出到黑后硬切 1→2"},
        {"at": 99, "kind": "ramp", "note": "曝光渐变 +40%/6 帧"},
        {"at": 129, "kind": "dup", "note": "精确重复帧"},
        {"at": 139, "kind": "near_dup", "note": "近重复帧 ±1 码值"},
        {"at": 159, "kind": "cut", "note": "运动量突变（生成器相位跳变，等同内容突变）"},
        {"at": 179, "kind": "cut", "note": "同亮度不同结构硬切 2→3"},
        {"at": 219, "kind": "dup_run", "note": "重复帧连跑 5 帧"},
    ]
    for k in range(220, 225):
        events.append({"at": k - 1, "kind": "dup_run", "note": "重复帧连跑中段"})

    # 事件施加：按帧号改亮度，再落 10bit 码值
    def scale_from_mid(i: int, f: float) -> None:
        luma[i] = np.clip(0.5 + (luma[i] - 0.5) * f, 0.0, 1.0)

    scale_from_mid(50, 2.6)                      # 闪光只亮一帧（接近爆白）
    for k in range(8):                            # 淡出
        scale_from_mid(70 + k, 1.0 - 0.12 * (k + 1))
    for k in range(6):                            # 曝光渐变
        scale_from_mid(100 + k, 1.0 + 0.0667 * (k + 1))
    luma[130] = luma[129]                         # 精确重复
    for k in range(220, 225):                     # 重复帧连跑
        luma[k] = luma[219]

    code = np.clip(np.rint(64.0 + luma * 876.0), 64, 940).astype(np.uint16)
    # 近重复帧：在码值上加 ±1，和精确重复区分开
    rng = np.random.default_rng(7)
    code[140] = np.clip(code[139].astype(np.int32) + rng.integers(-1, 2, size=(H, W)), 64, 940).astype(np.uint16)
    return code, events


def write_source(code: np.ndarray, path: Path) -> None:
    """把 Y 平面写成 4:2:0 10bit 裸流，用 ffmpeg 封成 FFV1 mkv（无损，供 vspipe 读）。"""

    chroma = np.full((len(code), H // 2, W // 2), 512, dtype=np.uint16)
    raw = path.with_suffix(".yuv")
    with raw.open("wb") as fp:
        for y, u, v in zip(code, chroma, chroma):
            fp.write(y.astype("<u2").tobytes())
            fp.write(u.astype("<u2").tobytes())
            fp.write(v.astype("<u2").tobytes())
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p10le",
         "-s", f"{W}x{H}", "-r", "24", "-i", str(raw), "-c:v", "ffv1", "-level", "3",
         "-pix_fmt", "yuv420p10le", str(path)],
        check=True,
    )


# ------------------------------------------------------- 插件检测器（numpy 复刻）


def plugin_mae(code: np.ndarray, a: int, b: int, *, remove_mean: bool) -> float:
    """frame_gen.cpp:isSceneCut 的等价实现：8 像素步长采 Y，[0,1] 归一化，平均绝对差。"""

    ya = (code[a][::STEP, ::STEP].astype(np.float64) - 64.0) / 876.0
    yb = (code[b][::STEP, ::STEP].astype(np.float64) - 64.0) / 876.0
    d = ya - yb
    if remove_mean:
        d = d - d.mean()
    return float(np.abs(d).mean())


# ------------------------------------------------------------ k7f 检测器（真代码）


def run_k7(code: np.ndarray, algo: int, threshold: float) -> tuple[list[int], float]:
    """vspipe 跑 k7f.SCDetect2，返回 _SceneChangeNext=1 的帧号与耗时（秒）。"""

    WORK.mkdir(parents=True, exist_ok=True)
    src = WORK / "clip.mkv"
    if not src.exists():
        write_source(code, src)
    out = WORK / f"flags_{algo}_{threshold:g}.json"
    if out.exists():
        out.unlink()
    script = WORK / f"sc_{algo}.vpy"
    script.write_text(
        f"""# -*- coding: utf-8 -*-
import json, sys
import vapoursynth as vs
sys.path.insert(0, {K7SRC.as_posix()!r})
import k7sfunc as k7f
core = vs.core
core.std.LoadPlugin(r"{ROOT.as_posix()}/测试环境/mpv-lazy-nv/vs-plugins/BestSource.dll")
clip = core.bs.VideoSource(source=r"{src.as_posix()}")
clip = k7f.SCDetect2(clip, algo={algo}, threshold={threshold})
_fp = open({str(out).replace(chr(92), "/")!r}, "w", encoding="utf-8")

def _dump(n, f):
    frame = f[0] if isinstance(f, (list, tuple)) else f
    _fp.write(json.dumps({{"n": n, "next": int(frame.props.get("_SceneChangeNext", 0) or 0),
                          "diff": float(frame.props.get("SCPlaneStatsDiff", 0.0) or 0.0)}}) + "\\n")
    _fp.flush()
    return frame

clip = clip.std.ModifyFrame(clips=[clip], selector=_dump)
clip.set_output()
""",
        encoding="utf-8",
    )
    t0 = time.perf_counter()
    run = subprocess.run([str(VSPIPE), str(script), "-"], cwd=WORK,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    dt = time.perf_counter() - t0
    if run.returncode != 0:
        print(run.stderr[-2000:], file=sys.stderr)
        raise RuntimeError(f"vspipe failed for algo {algo}")
    rows = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines() if line.strip()]
    return rows, dt


# ------------------------------------------------------------------ 统计


def evaluate(name: str, cut_at: set[int], flagged: set[int], labels: dict[int, str],
             benign: set[int]) -> dict:
    tp = sorted(cut_at & flagged)
    fp = sorted(flagged - cut_at)
    fn = sorted(cut_at - flagged)
    fp_hard = [i for i in fp if i in benign or labels.get(i, "").startswith("dup")
               or labels.get(i) == "near_dup"]
    return {
        "detector": name,
        "tp": len(tp), "fp_hard": len(fp_hard), "fp_neutral": len(fp) - len(fp_hard), "fn": len(fn),
        "fp_detail": [(i, labels.get(i, "-")) for i in fp],
        "fn_detail": [(i, labels.get(i, "-")) for i in fn],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-vs", action="store_true", help="只跑 numpy 复刻，不启动 vspipe")
    args = ap.parse_args()

    code, events = build_clip()
    WORK.mkdir(parents=True, exist_ok=True)
    labels = {e["at"]: e["kind"] for e in events}
    cut_at = {e["at"] for e in events if e["kind"] == "cut"}
    dup_at = {e["at"] for e in events if e["kind"] in ("dup", "near_dup", "dup_run")}
    near_dup_at = {e["at"] for e in events if e["kind"] == "near_dup"}
    benign = {e["at"] for e in events if e["kind"] in ("flash", "ramp")}

    rows: list[dict] = []
    metrics: dict[str, dict[int, float]] = {}
    times: dict[str, float] = {}
    for remove_mean, tag in ((False, "plugin-old"), (True, "plugin-new")):
        mae = {i: plugin_mae(code, i, i + 1, remove_mean=remove_mean) for i in range(FRAMES - 1)}
        metrics[tag] = mae
        for thr in (0.10, 0.15, 0.25, 0.35):
            rows.append(evaluate(f"{tag}@{thr}", cut_at, {i for i, v in mae.items() if v > thr},
                                 labels, benign))

    # 三帧规则：逐对差值大、且下一对差值小，才算切镜；两次都大而首尾相近 = 闪光
    for remove_mean, tag in ((False, "plugin-3f"), (True, "plugin-3f-mean")):
        for thr in (0.10, 0.15):
            d1 = {i: plugin_mae(code, i, i + 1, remove_mean=remove_mean) for i in range(FRAMES - 1)}
            flagged = set()
            for i in range(FRAMES - 2):
                if d1[i] <= thr:
                    continue
                d_prev_next = plugin_mae(code, i, i + 2, remove_mean=remove_mean)
                # 首尾相近：闪光（前后都不像，但隔一帧又一样），判非切镜
                if d1[i + 1] > thr and d_prev_next < thr * 0.5:
                    continue
                flagged.add(i)
            rows.append(evaluate(f"{tag}@{thr}", cut_at, flagged, labels, benign))

    k7_grid = (0.02, 0.04, 0.06, 0.08, 0.10, 0.12, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50)
    k7_best: dict[str, tuple] = {}
    if not args.no_vs:
        # SCDetect2 不把 SCPlaneStatsDiff 传到输出帧，只能按阈值重跑取最优 F1
        for algo in (1, 2, 3, 4):
            best = None
            for thr in k7_grid:
                frame_rows, dt = run_k7(code, algo, thr)
                times.setdefault(f"k7-algo{algo}", round(dt, 2))
                flagged = {r["n"] for r in frame_rows if r["next"]}
                tp = len(cut_at & flagged)
                fn = len(cut_at - flagged)
                fp = len(flagged - cut_at)
                f1 = 0.0 if tp == 0 else 2 * tp / (2 * tp + fp + fn)
                if best is None or f1 > best[0]:
                    best = (f1, thr, tp, fn, fp, sorted(flagged - cut_at))
            k7_best[f"k7-algo{algo}"] = best

    # ---- 外部算法复刻：PySceneDetect 的 content / adaptive，以及运动补偿 MAE
    for tag, kw in (("pysd-content-luma", dict(w_lum=1.0, w_edge=0.0)),
                    ("pysd-content-edge", dict(w_lum=1.0, w_edge=0.25))):
        # 归一到 0~1，和插件采样 MAE 同一量纲，便于并排看
        metrics[tag] = {i: v / 255.0 for i, v in algos.content_scores(code, **kw).items()}
    metrics["comp-mae"] = {i: algos.compensated_mae(code, i, i + 1) for i in range(FRAMES - 1)}
    # 矢量类：梯度方向直方图距离（对整体增益不敏感，闪光≠切镜）
    metrics["hog"] = {i: algos.hog_distance(code, i, i + 1) for i in range(FRAMES - 1)}

    # 阈值扫描：每个指标取能让 F1 最高的阈值，才算公平比较（各指标量纲不同）
    print("\n== 阈值扫描（每个指标的最优阈值） ==")
    print(f"{'指标':<16} {'最优阈值':>8} {'TP':>3} {'FN':>3} {'FP':>3} {'F1':>5}  备注")
    for name, m in metrics.items():
        best = None
        for thr in sorted(set(round(v, 3) for v in m.values())):
            if thr <= 0:
                continue
            flagged = {i for i, v in m.items() if v >= thr}
            tp = len(cut_at & flagged)
            fn = len(cut_at - flagged)
            fp = len(flagged - cut_at)
            f1 = 0.0 if tp == 0 else 2 * tp / (2 * tp + fp + fn)
            if best is None or f1 > best[0]:
                best = (f1, thr, tp, fn, fp, sorted(flagged - cut_at))
        f1, thr, tp, fn, fp, fps = best
        note = "" if not fps else f"FP 帧 {fps[:6]}"
        print(f"{name:<16} {thr:>8.3f} {tp:>3} {fn:>3} {fp:>3} {f1:>5.2f}  {note}")
    for name, (f1, thr, tp, fn, fp, fps) in k7_best.items():
        note = "" if not fps else f"FP 帧 {fps[:6]}"
        print(f"{name:<16} {thr:>8.3f} {tp:>3} {fn:>3} {fp:>3} {f1:>5.2f}  {note}")

    # 三帧规则：逐对差值大、且下一对差值也大而首尾相近 → 闪光；其余大差值判切镜
    def flags_3f(d1: dict[int, float], i: int, thr: float, d_ac) -> bool | None:
        if d1[i] <= thr:
            return False
        if i + 1 in d1 and d1[i + 1] > thr and d_ac(i) < thr * 0.5:
            return False
        return True

    for tag, rm in (("plugin-3f", False), ("plugin-3f-mean", True)):
        best = None
        for thr in (0.06, 0.08, 0.10, 0.12, 0.15, 0.20, 0.25):
            d1 = metrics["plugin-old" if not rm else "plugin-new"]
            flagged = {i for i in range(FRAMES - 2) if flags_3f(
                d1, i, thr, lambda i: plugin_mae(code, i, i + 2, remove_mean=rm))}
            tp = len(cut_at & flagged)
            fn = len(cut_at - flagged)
            fp = len(flagged - cut_at)
            f1 = 0.0 if tp == 0 else 2 * tp / (2 * tp + fp + fn)
            if best is None or f1 > best[0]:
                best = (f1, thr, tp, fn, fp, sorted(flagged - cut_at))
        f1, thr, tp, fn, fp, fps = best
        print(f"{tag:<16} {thr:>8.3f} {tp:>3} {fn:>3} {fp:>3} {f1:>5.2f}  "
              + (f"FP 帧 {fps[:6]}" if fps else ""))

    # ---- PySceneDetect AdaptiveDetector：content_val 与邻域均值的比值（抗摇镜）
    adv_best = None
    for ratio in (1.5, 2.0, 2.5, 3.0, 4.0):
        for mc in (0.0, 0.03, 0.05, 0.08, 0.12):
            flagged = algos.adaptive_flags(
                {i: v * 255.0 for i, v in metrics["pysd-content-luma"].items()},
                window=2, ratio=ratio, min_content=mc * 255.0)
            tp = len(cut_at & flagged)
            fn = len(cut_at - flagged)
            fp = len(flagged - cut_at)
            f1 = 0.0 if tp == 0 else 2 * tp / (2 * tp + fp + fn)
            if adv_best is None or f1 > adv_best[0]:
                adv_best = (f1, f"r{ratio:g}/c{mc:g}", tp, fn, fp, sorted(flagged - cut_at))
    f1, thr, tp, fn, fp, fps = adv_best
    print(f"{'pysd-adaptive':<16} {thr:>8} {tp:>3} {fn:>3} {fp:>3} {f1:>5.2f}  "
          + (f"FP 帧 {fps[:6]}" if fps else ""))

    # 重复帧可用性：各检测器在 dup 边界与正常边界上的原始差值
    mae_old = {i: plugin_mae(code, i, i + 1, remove_mean=False) for i in range(FRAMES - 1)}
    mae_new = {i: plugin_mae(code, i, i + 1, remove_mean=True) for i in range(FRAMES - 1)}
    # 重复帧检测：逐像素 MAE、分块最大绝对差、dHash 汉明距离、完全相同像素比例
    def dup_metrics(i: int) -> dict:
        return {
            "mae": round(mae_old[i], 5),
            "blk_max": round(algos.block_max_abs_diff(code, i, i + 1), 5),
            "dhash": algos.hamming(algos.dhash(code[i]), algos.dhash(code[i + 1])),
            "equal_ratio": round(algos.exact_equal_ratio(code[i], code[i + 1]), 4),
        }

    dup_report = {
        "dup_boundaries": {i: dup_metrics(i) for i in sorted(dup_at)},
        "near_dup_boundaries": {i: dup_metrics(i) for i in sorted(near_dup_at)},
        "quiet_boundaries": {i: dup_metrics(i) for i in sorted(benign)[:3]},
        "motion_min_mae": round(min(mae_old[i] for i in range(FRAMES - 1) if i not in dup_at), 5),
    }

    print("\n== 切镜检测 ==")
    print(f"{'检测器':<18} {'TP':>3} {'FN':>3} {'FP良性':>6} {'FP中性':>6}  耗时")
    for r in rows:
        print(f"{r['detector']:<18} {r['tp']:>3} {r['fn']:>3} {r['fp_hard']:>6} {r['fp_neutral']:>6}"
              + (f"  {r['_vspipe_s']}s" if "_vspipe_s" in r else ""))
        if r["fn_detail"]:
            print(f"{'':<18} 漏报: {r['fn_detail']}")
        if r["fp_detail"]:
            print(f"{'':<18} 误报: {r['fp_detail']}")

    print("\n== 标注边界的原始差值（old 不减均值 / new 减均值） ==")
    mae_old_all = {i: plugin_mae(code, i, i + 1, remove_mean=False) for i in range(FRAMES - 1)}
    mae_new_all = {i: plugin_mae(code, i, i + 1, remove_mean=True) for i in range(FRAMES - 1)}
    for e in events:
        i = e["at"]
        print(f"{i:>4} {e['kind']:<8} old={mae_old_all[i]:.4f} new={mae_new_all[i]:.4f}  {e['note']}")

    print("\n== 重复帧边界的原始差值（能不能用一个阈值分开） ==")
    print(json.dumps(dup_report, ensure_ascii=False, indent=2))

    (WORK / "result.json").write_text(
        json.dumps({"rows": [{k: v for k, v in r.items() if k != "_mae"} for r in rows],
                    "dup": dup_report}, ensure_ascii=False, indent=2),
        encoding="utf-8")
    print(f"\n结果 -> {WORK / 'result.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
