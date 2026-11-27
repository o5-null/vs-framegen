"""真实素材上的切镜 / 重复帧检测对照。

合成片源能给出准确标注，但覆盖不到真实编码噪声、动画平涂、快切 PV、摄像机运动。
本脚本在真实片段上跑同一批检测器，用"多检测器共识"作伪标注，看：

1. 各检测器在各自**生产默认参数**下的触发数（密度，cuts/100 帧）——会不会过分敏感；
2. 相对共识伪标注的 precision / recall ——谁更接近"大家都同意"的切镜；
3. 分块最大绝对差在真实重复帧（静止画面/停顿）上的分布，判断重复帧门限。

用法：
    python real_data_bench.py              # 全部样本
    python real_data_bench.py --only 0 2   # 只跑指定下标
    python real_data_bench.py --frames 240 # 改窗口长度
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(r"E:\code\dlssg-vfi")
VSPIPE = ROOT / ".venv" / "Scripts" / "vspipe.exe"
FFPROBE = "ffprobe"
WORK = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\realbench")
# 解码用的硬件加速方式：cuda(NVDEC) / d3d11va / "off"（回退 BestSource 纯 CPU）
HWACCEL = "cuda"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_algos as algos  # noqa: E402

# (标签, 路径, 起始帧)  —— 样本覆盖：游戏 PV 快切 / 4K HDR / 高帧率 / 番剧平涂 / 实拍
SAMPLES: list[tuple[str, str, int]] = [
    ("PV-阿哈1080p", r"E:\code\dlssg-vfi\测试数据\阿哈角色PV 1080p.mkv", 0),
    ("EP-无中生有", r"E:\code\dlssg-vfi\测试数据\《崩坏：星穹铁道》EP：「无中生有」.mp4", 600),
    ("HFR-60fps", r"E:\code\dlssg-vfi\测试数据\test_60fps.mp4", 600),
    ("HDR10-4K", r"E:\code\dlssg-vfi\测试数据\hdr10.mkv", 300),
    ("番剧-TANKCHAIR", r"F:\番剧\[LoliHouse] TANK CHAIR - 04 [WebRip 1080p HEVC-10bit AAC ASSx2].mkv", 1000),
    ("番剧-ToaruItem", r"F:\番剧\[LoliHouse] Toaru Anbu no Item - 01 [WebRip 1080p HEVC-10bit AAC SRTx2].mkv", 1000),
    ("番剧-Horemajo", r"F:\番剧\[Nekomoe kissaten&LoliHouse] Horemajo - 02 [WebRip 1080p HEVC-10bit AAC ASSx2].mkv", 1000),
    ("实拍-Ramparts", r"F:\番剧\The.Ramparts.of.Ice.S02E06.Cultural.Festival.1080p.NF.WEB-DL.DUAL.AAC2.0.H.264-VARYG.mkv", 1000),
]

STEP = 8  # 插件采样步长（frame_gen.cpp）

# 各检测器在生产默认参数下的判定门限（归一化到 0~1 的插件量纲）
PROD_THRESHOLDS = {
    "plugin@0.12": 0.12,
    "plugin@0.15": 0.15,
    "plugin@0.25": 0.25,
    "content-edge@27": 27.0 / 255.0,
    "comp-mae@0.20": 0.20,
    "hog@0.10": 0.10,
    "hog@0.15": 0.15,
    "mv-res@0.05": 0.05,
}


# ------------------------------------------------------------------ 解码


def probe(path: str) -> dict:
    """ffprobe 取宽高、像素格式、帧率。"""

    out = subprocess.run(
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,pix_fmt,r_frame_rate", "-of", "json", path],
        capture_output=True, text=True, check=True)
    st = json.loads(out.stdout)["streams"][0]
    num, den = st["r_frame_rate"].split("/")
    return {"w": int(st["width"]), "h": int(st["height"]), "pix_fmt": st["pix_fmt"],
            "fps": float(num) / float(den)}


def decode_window(path: str, start: int, frames: int) -> tuple[np.ndarray, dict]:
    """解码一段，统一成 10bit 的 Y 平面 [N, H, W] uint16。

    默认走 ffmpeg 硬件解码（NVDEC/d3d11va）只取亮度平面：1080p HEVC 约 25ms/帧，
    比 BestSource 纯 CPU 解码（约 250ms/帧）快一个量级；硬件不可用时自动回退 BestSource。
    结果按 (路径, 起点, 长度) 缓存到 npy，重复跑检测器时直接复用。
    """

    import hashlib

    WORK.mkdir(parents=True, exist_ok=True)
    key = hashlib.md5(f"{path}|{start}|{frames}".encode("utf-8")).hexdigest()[:12]
    cache_npy = WORK / f"{key}.npy"
    cache_json = WORK / f"{key}.json"
    if cache_npy.exists() and cache_json.exists():
        return np.load(cache_npy), json.loads(cache_json.read_text(encoding="utf-8"))

    info = probe(path)
    if HWACCEL not in ("off", ""):
        try:
            stacked = _decode_ffmpeg(path, start, frames, info, HWACCEL)
        except Exception as exc:  # noqa: BLE001  （硬件解码失败就退回 BestSource）
            print(f"[warn] 硬件解码失败（{exc}），回退 BestSource", file=sys.stderr)
            stacked = _decode_bestsource(path, start, frames, info)
    else:
        stacked = _decode_bestsource(path, start, frames, info)

    np.save(cache_npy, stacked)
    cache_json.write_text(json.dumps(info, ensure_ascii=False), encoding="utf-8")
    return stacked, info


def _decode_ffmpeg(path: str, start: int, frames: int, info: dict, hw: str) -> np.ndarray:
    """硬件解码 + 只输出 gray10le（亮度 10bit），省掉 2/3 的管道流量。"""

    w, h = info["w"], info["h"]
    fps = info["fps"] or 24.0
    cmd = [
        "ffmpeg", "-hide_banner", "-v", "error",
        "-hwaccel", hw,
        "-ss", f"{start / fps:.6f}", "-i", path,
        "-frames:v", str(frames),
        "-pix_fmt", "gray10le", "-f", "rawvideo", "-",
    ]
    frame_bytes = w * h * 2
    out = np.zeros((frames, h, w), dtype=np.uint16)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=frame_bytes)
    got = 0
    try:
        while got < frames:
            buf = proc.stdout.read(frame_bytes)
            if not buf or len(buf) < frame_bytes:
                break
            out[got] = np.frombuffer(buf, dtype="<u2").reshape(h, w)
            got += 1
    finally:
        proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace")
        proc.wait()
    if got < 3:  # 硬件解码起步失败或文件异常
        raise RuntimeError(f"ffmpeg 只解出 {got} 帧: {err[-400:]}")
    if got < frames:
        print(f"[warn] 期望 {frames} 帧，实得 {got} 帧（{Path(path).name}）", file=sys.stderr)
    return out[:got].copy()


def _decode_bestsource(path: str, start: int, frames: int, info: dict) -> np.ndarray:
    """纯 CPU 回退路径：BestSource 解码 + Point 转 YUV420P10，取 Y 平面。"""

    script = WORK / "decode.vpy"
    script.write_text(
        f"""# -*- coding: utf-8 -*-
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin(r"{ROOT.as_posix()}/测试环境/mpv-lazy-nv/vs-plugins/BestSource.dll")
clip = core.bs.VideoSource(source=r"{Path(path).as_posix()}")
clip = clip[{start}:{start + frames}]
clip = core.resize.Point(clip, format=vs.YUV420P10)
clip.set_output()
""", encoding="utf-8")

    w, h = info["w"], info["h"]
    frame_bytes = w * h * 3  # 10bit 存成 16bit，YUV420 共 1.5 样本/像素
    proc = subprocess.Popen([str(VSPIPE), str(script), "-"], cwd=WORK,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    planes: list[np.ndarray] = []
    while True:
        buf = proc.stdout.read(frame_bytes)
        if not buf or len(buf) < frame_bytes:
            break
        arr = np.frombuffer(buf, dtype="<u2")
        planes.append(arr[: w * h].reshape(h, w).copy())
    err = proc.stderr.read().decode("utf-8", "replace")
    if proc.wait() != 0:
        raise RuntimeError(f"vspipe failed for {path}: {err[-800:]}")
    return np.stack(planes)


# ---------------------------------------------------------------- 检测器


def plugin_mae(code: np.ndarray, a: int, b: int, *, remove_mean: bool = False) -> float:
    """frame_gen.cpp:isSceneCut 等价实现（8 像素采样、[0,1] 归一化、平均绝对差）。"""

    ya = (code[a][::STEP, ::STEP].astype(np.float64) - 64.0) / 876.0
    yb = (code[b][::STEP, ::STEP].astype(np.float64) - 64.0) / 876.0
    d = ya - yb
    if remove_mean:
        d = d - d.mean()
    return float(np.abs(d).mean())


def score_all(code: np.ndarray) -> dict[str, dict[int, float]]:
    """所有检测器在每条边界上的分数（除 content 外都已归一化到插件量纲 0~1）。

    重算法（内容/边缘、运动补偿）在 1/4 尺寸上算：平均差与分辨率无关，速度提一个量级。
    """

    n = len(code)
    scores: dict[str, dict[int, float]] = {}
    scores["plugin"] = {i: plugin_mae(code, i, i + 1) for i in range(n - 1)}
    scores["plugin-new"] = {i: plugin_mae(code, i, i + 1, remove_mean=True) for i in range(n - 1)}
    lr = code[:, ::4, ::4]
    scores["content-edge"] = {i: v / 255.0 for i, v in
                              algos.content_scores(lr, w_lum=1.0, w_edge=0.25).items()}
    scores["comp-mae"] = {i: algos.compensated_mae(lr, i, i + 1) for i in range(n - 1)}
    scores["blockmax"] = {i: algos.block_max_abs_diff(lr, i, i + 1) for i in range(n - 1)}
    # 矢量类：HOG 方向距离 + 块匹配矢量场（残差、全局一致性）
    scores["hog"] = {i: algos.hog_distance(code, i, i + 1) for i in range(n - 1)}
    mv = {i: algos.mv_field(code, i, i + 1) for i in range(n - 1)}
    scores["mv-res"] = {i: v[0] for i, v in mv.items()}
    scores["mv-coherence"] = {i: v[1] for i, v in mv.items()}
    return scores


def adaptive_set(content: dict[int, float], ratio: float = 4.0, min_content: float = 0.078) -> set[int]:
    """AdaptiveDetector 口径的判定（输入 0~1，min_content 已归一化）。"""

    return algos.adaptive_flags({i: v * 255.0 for i, v in content.items()},
                                window=2, ratio=ratio, min_content=min_content * 255.0)


def three_frame_set(plugin: dict[int, float], thr: float = 0.09,
                    d_ac: dict[int, float] | None = None) -> set[int]:
    """三帧规则：孤立尖峰才算切镜，闪光（前后两对都大且首尾相近）排除。"""

    out: set[int] = set()
    for i, v in plugin.items():
        if v <= thr:
            continue
        nxt = plugin.get(i + 1, 0.0)
        if nxt > thr and d_ac is not None and d_ac.get(i, 1.0) < thr * 0.5:
            continue
        out.add(i)
    return out


# ------------------------------------------------------------------ 主流程


def pseudo_labels(scores: dict[str, dict[int, float]]) -> set[int]:
    """伪标注：四个独立度量（含矢量 HOG）里至少两个给出高置信切镜，才算"共识切镜"。

    门槛取 0.12：真实素材上真切镜普遍在 0.14~0.20（番剧 TANKCHAIR 边界 109 为
    plugin 0.17 / hog 0.17 而 plugin-new 仅 0.14），把门槛定在 0.20 会把真切镜排除出标注。
    """

    votes: dict[int, int] = {}
    for name, hi in (("plugin-new", 0.12), ("content-edge", 0.12),
                     ("comp-mae", 0.12), ("hog", 0.10)):
        for i, v in scores[name].items():
            if v >= hi:
                votes[i] = votes.get(i, 0) + 1
    return {i for i, c in votes.items() if c >= 2}


def evaluate(name: str, flagged: set[int], truth: set[int]) -> dict:
    tp = len(flagged & truth)
    fp = len(flagged - truth)
    fn = len(truth - flagged)
    prec = tp / (tp + fp) if tp + fp else 0.0
    rec = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
    return {"detector": name, "tp": tp, "fn": fn, "fp": fp,
            "fn_idx": sorted(truth - flagged)[:6], "fp_idx": sorted(flagged - truth)[:6],
            "precision": round(prec, 3), "recall": round(rec, 3), "f1": round(f1, 3)}


def run_sample(label: str, path: str, start: int, frames: int) -> dict | None:
    if not Path(path).exists():
        print(f"[跳过] {label}: 文件不存在", file=sys.stderr)
        return None
    t0 = time.perf_counter()
    code, info = decode_window(path, start, frames)
    dec = time.perf_counter() - t0
    if len(code) < 10:
        print(f"[跳过] {label}: 解码帧数不足", file=sys.stderr)
        return None

    scores = score_all(code)
    truth = pseudo_labels(scores)
    content = algos.content_scores(code[:, ::4, ::4], w_lum=1.0, w_edge=0.0)
    d_ac = {i: plugin_mae(code, i, i + 2) for i in range(len(code) - 2)}

    cands: dict[str, set[int]] = {}
    for name, thr in PROD_THRESHOLDS.items():
        base = name.split("@")[0]
        cands[name] = {i for i, v in scores[base].items() if v >= thr}
    cands["adaptive r4/c0.078"] = adaptive_set(content)
    cands["3-frame @0.09"] = three_frame_set(scores["plugin"], d_ac=d_ac)
    # 矢量判据：方向分布剧变 + 矢量场不一致（闪光方向分布不变，快摇矢量场一致）
    cands["hog+coh"] = {i for i, v in scores["hog"].items()
                        if v >= 0.10 and scores["mv-coherence"][i] < 0.5}

    rows = [evaluate(n, f, truth) for n, f in cands.items()]
    dup = {i: round(v, 4) for i, v in scores["blockmax"].items()}
    return {
        "label": label, "path": path, "start": start, "frames": len(code),
        "w": info["w"], "h": info["h"], "fps": round(info["fps"], 2), "decode_s": round(dec, 1),
        "consensus_cuts": len(truth), "cut_density": round(len(truth) / max(len(code), 1) * 100, 1),
        "rows": rows,
        "blockmax_p05": round(float(np.percentile(list(dup.values()), 5)), 4),
        "blockmax_p50": round(float(np.percentile(list(dup.values()), 50)), 4),
        "blockmax_min": round(min(dup.values()), 4),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", type=int)
    ap.add_argument("--frames", type=int, default=480)
    ap.add_argument("--hw", default=None, help="cuda / d3d11va / off")
    args = ap.parse_args()

    global HWACCEL
    if args.hw is not None:
        HWACCEL = args.hw

    idx = args.only if args.only is not None else range(len(SAMPLES))
    results = []
    for k in idx:
        label, path, start = SAMPLES[k]
        print(f"\n=== {label}  ({Path(path).name})", flush=True)
        try:
            r = run_sample(label, path, start, args.frames)
        except Exception as exc:  # noqa: BLE001  （单样本失败不影响整体）
            print(f"[失败] {label}: {exc}", file=sys.stderr)
            continue
        if r is None:
            continue
        results.append(r)
        print(f"{r['w']}x{r['h']} {r['fps']}fps  {r['frames']} 帧  解码 {r['decode_s']}s  "
              f"共识切镜 {r['consensus_cuts']} ({r['cut_density']}/100 帧)")
        print(f"{'检测器':<20} {'TP':>3} {'FN':>3} {'FP':>4} {'精确率':>6} {'召回':>5} {'F1':>5}")
        for row in r["rows"]:
            print(f"{row['detector']:<20} {row['tp']:>3} {row['fn']:>3} {row['fp']:>4} "
                  f"{row['precision']:>6.2f} {row['recall']:>5.2f} {row['f1']:>5.2f}"
                  + (f"  FN索引{row['fn_idx']}" if row["fn_idx"] else "")
                  + (f"  FP索引{row['fp_idx']}" if row["fp_idx"] else ""))
        print(f"块最大差 p05={r['blockmax_p05']} p50={r['blockmax_p50']} min={r['blockmax_min']}")

    # 汇总：各检测器跨样本的 F1 均值（跳过无共识切镜的样本）
    valid = [r for r in results if r["consensus_cuts"] >= 3]
    if valid:
        print("\n== 跨样本汇总（仅含 >=3 个共识切镜的样本，F1 平均） ==")
        names = [row["detector"] for row in valid[0]["rows"]]
        for n in names:
            vals = []
            for r in valid:
                row = next((x for x in r["rows"] if x["detector"] == n), None)
                if row is not None:
                    vals.append(row["f1"])
            if vals:
                print(f"{n:<20} {sum(vals) / len(vals):>5.2f}   "
                      f"({', '.join(f'{v:.2f}' for v in vals)})")

    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "real_result.json").write_text(
        json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n结果 -> {WORK / 'real_result.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
