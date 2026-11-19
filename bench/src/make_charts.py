"""Charts for the eight-engine benchmark.

Reads the result folder (VFI_DATA)\\throughput.json and quality.json, writes
测试数据\\engines\\charts\\*.png plus a throughput.csv pivot table.
"""

import csv
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as C

CHARTS = os.path.join(C.DATA, "charts")
RES_TAGS = [r[2] for r in C.RESOLUTIONS]  # full ladder; MAIN_RES only for the compact views
LADDER = RES_TAGS
MPX = {r[2]: r[0] * r[1] / 1e6 for r in C.RESOLUTIONS}


def setup_font():
    for name in ("Microsoft YaHei", "SimHei", "DengXian", "Noto Sans CJK SC"):
        if any(name in f.name for f in font_manager.fontManager.ttflist):
            plt.rcParams["font.sans-serif"] = [name, "DejaVu Sans"]
            break
    plt.rcParams["axes.unicode_minus"] = False
    plt.rcParams["figure.dpi"] = 130


def load():
    with open(os.path.join(C.DATA, "throughput.json"), encoding="utf-8") as fh:
        thr = json.load(fh)["runs"]
    q = {}
    p = os.path.join(C.DATA, "quality.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            q = json.load(fh)["per_method"]
    table = {}
    for r in thr:
        table.setdefault(r["engine"], {})[r["res"]] = r
    return table, q


def write_csv(table):
    with open(os.path.join(C.DATA, "throughput.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["engine"] + ["%s ms/pair" % t for t in RES_TAGS] +
                   ["%s ms/Mpx" % t for t in RES_TAGS] + ["%s fps" % t for t in RES_TAGS])
        for e in C.ENGINES:
            row = [table.get(e, {}).get(t, {}).get("ms_per_pair", "") for t in RES_TAGS]
            w.writerow([e] + row +
                       [round(v / MPX[t], 3) if v != "" else "" for t, v in zip(RES_TAGS, row)] +
                       [round(2000.0 / v, 2) if v != "" else "" for v in row])


def throughput_curves(table, key, ylabel, title, path, log=False):
    fig, ax = plt.subplots(figsize=(9.4, 5.2))
    x = list(range(len(RES_TAGS)))
    for e in C.ENGINES:
        ys = []
        for t in RES_TAGS:
            r = table.get(e, {}).get(t) or {}
            if "ms_per_pair" not in r:
                ys.append(None)
                continue
            ys.append(2000.0 / r["ms_per_pair"] if key == "fps" else r["ms_per_pair"])
        ax.plot(x, ys, marker="o", markersize=5, linewidth=1.9, label=e, color=C.COLORS[e])
    ax.set_xticks(x)
    ax.set_xticklabels([("%s\n%.2f Mpx" % (t, MPX[t])) for t in RES_TAGS], fontsize=7.5)
    ax.set_xlabel("分辨率（输入帧大小）")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if log:
        ax.set_yscale("log")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(ncol=2, fontsize=8.5)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    print("wrote", path)


def density_chart(table):
    """ms per megapixel against frame size: a flat line means cost grows with pixel count."""
    pts = [t for t in LADDER
           if sum(1 for e in C.ENGINES if "ms_per_pair" in table.get(e, {}).get(t, {})) >= len(C.ENGINES) - 2]
    if len(pts) < 3:
        print("ladder incomplete (%d sizes), skipping density chart" % len(pts))
        return

    def series(e):
        return [(t, table[e][t]["ms_per_pair"]) for t in pts if "ms_per_pair" in table.get(e, {}).get(t, {})]

    with open(os.path.join(C.DATA, "throughput_ladder.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["engine"] + ["%s ms/pair" % t for t in pts] + ["%s ms/Mpx" % t for t in pts])
        for e in C.ENGINES:
            got = dict(series(e))
            w.writerow([e] + [got.get(t, "") for t in pts] +
                       [round(got[t] / MPX[t], 3) if t in got else "" for t in pts])

    fig, (ax, bx) = plt.subplots(1, 2, figsize=(13.4, 5.4))
    for e in C.ENGINES:
        pts_e = series(e)
        if len(pts_e) < 2:
            continue
        ax.plot([MPX[t] for t, _ in pts_e], [v / MPX[t] for t, v in pts_e],
                marker="o", ms=4.5, lw=1.8, label=e, color=C.COLORS[e])
        bx.plot([MPX[t] for t, _ in pts_e], [v for _, v in pts_e],
                marker="o", ms=4.5, lw=1.8, label=e, color=C.COLORS[e])

    xs = [MPX[t] for t in pts]

    # Left: per-pixel cost, log x so the eleven steps are evenly spaced. A falling curve
    # means a fixed cost being amortised, a flat one means the cost is all per pixel.
    ax.set_xscale("log")
    ax.set_xticks(xs)
    ax.set_xticklabels(pts, rotation=30, fontsize=7.5)
    ax.minorticks_off()
    ax.set_xlabel("输入帧大小（对数轴）")
    ax.set_ylabel("每百万像素耗时 ms/Mpx")
    ax.set_title("单位像素耗时 vs 分辨率（下降 = 固定开销被摊薄，走平 = 纯按像素计费）")
    ax.grid(True, which="major", alpha=0.25)
    if "1080p" in pts:
        ax.axvline(MPX["1080p"], color="#cccccc", lw=1.0, ls=":", zorder=0)

    # log-log with the slope-1 reference: any deviation is fixed overhead or amortisation
    at_1080 = [table[e]["1080p"]["ms_per_pair"] for e in C.ENGINES
               if "ms_per_pair" in table.get(e, {}).get("1080p", {})]
    k = (sum(at_1080) / len(at_1080)) / MPX["1080p"] if at_1080 else 0.01
    bx.plot([xs[0], xs[-1]], [k * xs[0], k * xs[-1]], "--", color="#888888", lw=1.2,
            label="完全线性（每像素常数）")
    bx.legend([bx.get_lines()[-1]], ["完全线性（每像素常数）"], loc="upper left", fontsize=8)
    bx.set_xscale("log")
    bx.set_yscale("log")
    bx.set_xlabel("输入像素数（百万像素，对数轴）")
    bx.set_ylabel("每对耗时 ms（对数轴）")
    bx.set_title("双对数：斜率 = 1 表示按像素线性缩放")
    bx.grid(True, which="both", alpha=0.25)
    for t, x in zip(pts, xs):
        bx.annotate(t, (x, bx.get_ylim()[0]), xytext=(0, 3), textcoords="offset points",
                    ha="center", va="bottom", fontsize=6.5, color="#666666", rotation=90)

    ax.legend(ncol=2, fontsize=8, loc="upper right")
    fig.text(0.01, 0.01, "口径：每对生成耗时（SMV 宿主自报；RIFE 为 TensorRT fp16 本地计时），"
                         "40 帧 / 39 对，SMV 5 遍取最小、RIFE 3 遍交错取最小。",
             fontsize=7.5, color="#555555")
    fig.tight_layout()
    path = os.path.join(CHARTS, "time_per_pixel.png")
    fig.savefig(path)
    plt.close(fig)
    print("wrote", path)


def quality_charts(q):
    if not q:
        print("no quality.json yet, skipping quality charts")
        return
    metrics = [("vmaf", "VMAF (越高越好)", (0, 100)),
               ("psnr_y", "PSNR-Y dB", None),
               ("ssim", "SSIM", None),
               ("sharpness_vs_gt", "锐度 / GT（1.0 = 真实帧）", None)]
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.2))
    for ax, (key, label, ylim) in zip(axes.flat, metrics):
        vals = [q[e][key] for e in C.ENGINES]
        bars = ax.bar(C.ENGINES, vals, color=[C.COLORS[e] for e in C.ENGINES])
        ax.set_title(label, fontsize=10)
        ax.tick_params(axis="x", rotation=30, labelsize=8)
        ax.grid(True, axis="y", alpha=0.25)
        if ylim:
            ax.set_ylim(*ylim)
        lo, hi = min(vals), max(vals)
        span = (hi - lo) or (abs(hi) or 1.0)
        ax.set_ylim(lo - span * 0.35, hi + span * 0.35)
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v + span * 0.04,
                    ("%.4f" % v) if abs(v) < 2 else ("%.2f" % v),
                    ha="center", fontsize=7)
    fig.suptitle("1080p 帧生成质量（19 个真实中间帧为参考）", fontsize=12)
    fig.tight_layout()
    fig.savefig(os.path.join(CHARTS, "quality.png"))
    plt.close(fig)
    print("wrote", os.path.join(CHARTS, "quality.png"))

    # speed against quality at 1080p: the trade-off the numbers are really about
    fig, ax = plt.subplots(figsize=(8.4, 5.2))
    pts = []
    for e in C.ENGINES:
        if "1080p" not in table_global or not q.get(e):
            continue
        ms = table_global["1080p"][e]["ms_per_pair"]
        ax.scatter(ms, q[e]["vmaf"], s=90, color=C.COLORS[e], zorder=3)
        pts.append((ms, q[e]["vmaf"], e))
    lo = min(p[1] for p in pts)
    hi = max(p[1] for p in pts)
    for i, (ms, vmaf, e) in enumerate(sorted(pts, key=lambda p: (-p[1], p[0]))):
        dy = 12 if i % 2 == 0 else -26
        if vmaf < lo + 1.2:      # lowest points have no room underneath
            dy = 12
        elif vmaf > hi - 0.8:    # ... and the highest two split, or their labels collide
            dy = 12 if i == 0 else -26
        ax.annotate("%s\n%.1f ms, VMAF %.2f" % (e, ms, vmaf), (ms, vmaf),
                    textcoords="offset points", xytext=(9, dy), fontsize=8,
                    va="bottom" if dy > 0 else "top")
    ax.set_xlabel("每对生成耗时 ms（1080p，越低越好）")
    ax.set_ylabel("VMAF（越高越好）")
    ax.set_title("1080p 速度-质量权衡")
    ax.grid(True, alpha=0.25)
    ax.set_xlim(left=0, right=max(p[0] for p in pts) * 1.22)
    ax.set_ylim(lo - 1.2, hi + 1.3)
    fig.tight_layout()
    fig.savefig(os.path.join(CHARTS, "speed_quality.png"))
    plt.close(fig)
    print("wrote", os.path.join(CHARTS, "speed_quality.png"))


if __name__ == "__main__":
    os.makedirs(CHARTS, exist_ok=True)
    setup_font()
    table, quality = load()
    table_global = {t: {e: table[e][t] for e in C.ENGINES if t in table.get(e, {})} for t in RES_TAGS}
    write_csv(table)
    throughput_curves(table, "ms", "每对生成耗时 ms（对数轴）", "帧生成吞吐：每对耗时 vs 分辨率",
                      os.path.join(CHARTS, "throughput_ms.png"), log=True)
    throughput_curves(table, "fps", "输出帧率 fps（2 倍速）", "帧生成吞吐：等效输出帧率 vs 分辨率",
                      os.path.join(CHARTS, "throughput_fps.png"))
    density_chart(table)
    quality_charts(quality)
