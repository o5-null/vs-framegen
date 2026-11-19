"""Score the 19 generated frames of every engine against their ground truth.

VMAF / PSNR-Y / SSIM come from ffmpeg's libvmaf (trained on real distortions, so a
blurred frame cannot win by being self-consistent the way a flow-error metric would
allow). MAE and the Laplacian-variance sharpness ratio are computed here.

Writes quality.json and quality.csv.
"""

import csv
import json
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as C

import cv2

SEQ = os.path.join(C.TMP, "vseq")
OUT_JSON = os.path.join(C.DATA, "quality.json")
OUT_CSV = os.path.join(C.DATA, "quality.csv")


def laplacian_var(rgb):
    gray = cv2.cvtColor(rgb.astype(np.float32), cv2.COLOR_RGB2GRAY)
    return float(cv2.Laplacian(gray, cv2.CV_32F, ksize=3).var())


def build_sequences():
    """ffmpeg needs numbered frames; 0002/0004/... is not a sequence."""
    if os.path.isdir(SEQ):
        shutil.rmtree(SEQ)
    names = C.gt_names()
    os.makedirs(os.path.join(SEQ, "gt"))
    for i, gt in enumerate(names):
        shutil.copyfile(os.path.join(C.PNG_DIR if hasattr(C, "PNG_DIR") else C.SRC_PNG, gt),
                        os.path.join(SEQ, "gt", "%04d.png" % (i + 1)))
    for m in C.ENGINES:
        os.makedirs(os.path.join(SEQ, m))
        for i, gt in enumerate(names):
            shutil.copyfile(os.path.join(C.quality_dir(m), gt), os.path.join(SEQ, m, "%04d.png" % (i + 1)))


def vmaf(dist, ref, log_name):
    filt = ("[0:v]format=yuv420p[d];[1:v]format=yuv420p[r];"
            "[d][r]libvmaf=log_fmt=json:log_path=%s:n_threads=8"
            ":feature=name=psnr|name=float_ssim" % log_name)
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
           "-framerate", "25", "-i", os.path.join(dist, "%04d.png"),
           "-framerate", "25", "-i", os.path.join(ref, "%04d.png"),
           "-lavfi", filt, "-f", "null", "-"]
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=SEQ)
    if p.returncode != 0:
        raise SystemExit("ffmpeg failed:\n" + p.stderr[-2000:])
    with open(os.path.join(SEQ, log_name), encoding="utf-8") as fh:
        return json.load(fh)


def main():
    build_sequences()
    names = C.gt_names()
    gts = [C.read_rgb(os.path.join(C.SRC_PNG, g)) for g in names]
    lap_gt = [laplacian_var(g) for g in gts]

    per_method, per_frame = {}, []
    for m in C.ENGINES:
        data = vmaf(m, "gt", "vmaf_%s.json" % m)
        pooled = data["pooled_metrics"]
        per_method[m] = {
            "vmaf": round(pooled["vmaf"]["mean"], 3),
            "psnr_y": round(pooled["psnr_y"]["mean"], 3),
            "ssim": round(pooled["float_ssim"]["mean"], 5),
        }
        for i, fr in enumerate(data["frames"]):
            mt = fr["metrics"]
            per_frame.append({"engine": m, "gt": names[i],
                              "vmaf": round(mt["vmaf"], 3),
                              "psnr_y": round(mt["psnr_y"], 3),
                              "ssim": round(mt["float_ssim"], 5)})
        print("%-14s vmaf %.3f  psnr_y %.3f  ssim %.5f"
              % (m, per_method[m]["vmaf"], per_method[m]["psnr_y"], per_method[m]["ssim"]), flush=True)

    # numpy side: MAE + sharpness, both against the same 8-bit ground truth
    for m in C.ENGINES:
        maes, sharp = [], []
        for i, gt in enumerate(names):
            gen = C.read_rgb(os.path.join(C.quality_dir(m), gt))
            maes.append(float(np.abs(gen - gts[i]).mean()))
            sharp.append(laplacian_var(gen) / lap_gt[i])
        per_method[m]["mae"] = round(float(np.mean(maes)), 6)
        per_method[m]["sharpness_vs_gt"] = round(float(np.mean(sharp)), 4)

    # per-frame wins (best VMAF of the eight)
    for i, gt in enumerate(names):
        rows = [r for r in per_frame if r["gt"] == gt]
        best = max(r["vmaf"] for r in rows)
        for r in rows:
            r["best"] = int(abs(r["vmaf"] - best) < 1e-9)
    wins = {m: sum(r["best"] for r in per_frame if r["engine"] == m) for m in C.ENGINES}
    for m in C.ENGINES:
        per_method[m]["vmaf_wins"] = wins[m]

    payload = {"source": "测试数据\\1080p, 19 ground-truth tweens", "per_method": per_method,
               "per_frame": per_frame}
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1, ensure_ascii=False)

    cols = ["engine", "vmaf", "psnr_y", "ssim", "mae", "sharpness_vs_gt", "vmaf_wins"]
    with open(OUT_CSV, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for m in C.ENGINES:
            w.writerow([m] + [per_method[m][c] for c in cols[1:]])

    print("\n%-14s %7s %7s %7s %8s %7s %5s" % ("engine", "vmaf", "psnr_y", "ssim", "mae", "sharp", "wins"))
    for m in C.ENGINES:
        d = per_method[m]
        print("%-14s %7.3f %7.3f %7.5f %8.6f %7.4f %5d"
              % (m, d["vmaf"], d["psnr_y"], d["ssim"], d["mae"], d["sharpness_vs_gt"], d["vmaf_wins"]))
    print("\nwrote", OUT_JSON, "and", OUT_CSV)


if __name__ == "__main__":
    main()
