r"""Second opinion on 测试数据: VMAF/PSNR/SSIM via ffmpeg, plus a sharpness check.

DIS flow is a weak judge here - a slightly blurred frame is more self-consistent, so the flow
measure can rank a blurred frame above a real one. VMAF is trained on real distortions and does not
have that bias, and Laplacian variance says directly how much detail each method kept.

Writes 测试数据\results\metrics_extra.json and prints a table.
"""

import json
import os
import shutil
import subprocess
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import cv2

SEQ = r"E:\code\dlssg-vfi\work\vseq"  # ASCII only: ffmpeg's filter parser mangles non-ASCII and a raw drive colon
FPS = 25


def laplacian_var(rgb):
    gray = vd.to_u8(vd.to_gray(rgb)).astype(np.float32)
    return float(cv2.Laplacian(gray, cv2.CV_32F, ksize=3).var())


def build_sequences(methods, triples):
    """ffmpeg wants numbered frames, and the source names 0002, 0004, ... are not a sequence."""
    for name in methods + ["gt"]:
        path = os.path.join(SEQ, name)
        if os.path.isdir(path):
            shutil.rmtree(path)
        os.makedirs(path)
        for i, (a_path, gt_path, b_path) in enumerate(triples):
            dst = os.path.join(path, "%04d.png" % (i + 1))
            if name == "gt":
                shutil.copyfile(gt_path, dst)
            else:
                vd.write_rgb8(dst, vd.read_rgb16(vd.frame_path(name, gt_path)))


def run_vmaf(dist_dir, ref_dir, log_path):
    # Relative paths with cwd=SEQ: a drive-letter colon inside the filter graph cannot be escaped
    # reliably at the -lavfi level, and avoiding it is simpler than quoting it correctly.
    dist = os.path.basename(dist_dir)
    ref = os.path.basename(ref_dir)
    log_name = os.path.basename(log_path)
    filt = ("[0:v]format=yuv420p[d];[1:v]format=yuv420p[r];"
            "[d][r]libvmaf=log_fmt=json:log_path=%s:n_threads=8"
            ":feature=name=psnr|name=float_ssim" % log_name)
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
           "-framerate", str(FPS), "-i", os.path.join(dist, "%04d.png"),
           "-framerate", str(FPS), "-i", os.path.join(ref, "%04d.png"),
           "-lavfi", filt, "-f", "null", "-"]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=SEQ)
    if proc.returncode != 0:
        raise SystemExit("ffmpeg failed:\n" + proc.stderr[-2000:])
    with open(log_path, encoding="utf-8") as fh:
        return json.load(fh)


def main():
    triples = vd.triplets()
    argv = sys.argv[1:]
    out_name = "metrics_extra.json"
    if argv and argv[0].endswith(".json"):
        out_name = argv.pop(0)
    methods = argv if argv else list(vd.METHODS)
    build_sequences(methods, triples)

    extra = {"per_method": {}, "per_triplet": []}
    for m in methods:
        log_path = os.path.join(SEQ, "vmaf_%s.json" % m)
        data = run_vmaf(os.path.join(SEQ, m), os.path.join(SEQ, "gt"), log_path)
        pooled = data["pooled_metrics"]
        extra["per_method"][m] = {
            "vmaf": round(pooled["vmaf"]["mean"], 3),
            "psnr": round(pooled.get("psnr", pooled.get("psnr_y", {})).get("mean", 0.0), 3),
            "ssim": round(pooled.get("float_ssim", pooled.get("ssim", {})).get("mean", 0.0), 5),
        }
        print("%-8s vmaf %.3f  psnr %.3f  ssim %.5f" %
              (m, pooled["vmaf"]["mean"], extra["per_method"][m]["psnr"],
               extra["per_method"][m]["ssim"]))

    for a_path, gt_path, b_path in triples:
        GT = vd.read_rgb(gt_path)
        lap_gt = laplacian_var(GT)
        row = {"triplet": vd.gt_name(gt_path)}
        for m in methods:
            row[m] = round(laplacian_var(vd.read_rgb16(vd.frame_path(m, gt_path))) / lap_gt, 4)
        row["gt"] = 1.0
        extra["per_triplet"].append(row)

    for m in methods:
        vals = [r[m] for r in extra["per_triplet"]]
        extra["per_method"][m]["sharpness_vs_gt"] = round(float(np.mean(vals)), 4)
        print("%-8s sharpness (Laplacian var / GT) %.4f" % (m, np.mean(vals)))

    with open(os.path.join(vd.RESULTS, out_name), "w", encoding="utf-8") as fh:
        json.dump(extra, fh, indent=1)
    print("wrote", os.path.join(vd.RESULTS, out_name))


if __name__ == "__main__":
    main()
