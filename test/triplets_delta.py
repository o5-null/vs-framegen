"""Does the amount of motion in a triplet predict how much the motion-vector input helps?

For every one of the 19 triplets this prints the mean |mv| of its first frame (the field the
plugin feeds for that pair) next to the VMAF / MAE change from the zero-vector baseline to the
Encore-format motion run. If the vectors mattered, the gain should follow the motion.
"""

import json
import os

import cv2
import numpy as np

RESULTS = r"E:\code\dlssg-vfi\测试数据\results"
MV_DIR = os.path.join(RESULTS, "dlssg_mv", "mv")
SCALE = 128.0
BIAS = 32768.0


def read_png(path):
    """cv2.imread cannot open non-ASCII paths, so go through a buffer."""
    buf = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(buf, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit("decode failed: " + path)
    return img


def mv_magnitude(name):
    img = read_png(os.path.join(MV_DIR, name))
    u = (img[:, :, 2].astype(np.float32) - BIAS) / SCALE
    v = (img[:, :, 1].astype(np.float32) - BIAS) / SCALE
    return float(np.hypot(u, v).mean())


def main():
    with open(os.path.join(RESULTS, "metrics_extra.json"), encoding="utf-8") as f:
        extra = json.load(f)
    vmaf = {row["triplet"]: row for row in extra["per_triplet"]}

    rows = []
    for row in extra["per_triplet"]:
        name = row["triplet"]                       # "0002.png" -> GT frame
        a_index = int(name[:4]) - 1                 # GT = A + 1
        mv_name = "%04d.png" % a_index
        if not os.path.exists(os.path.join(MV_DIR, mv_name)):
            continue
        rows.append((mv_name, mv_magnitude(mv_name), row))

    print("%-10s %8s %9s %9s %9s %9s" % ("A frame", "|mv| px", "vmaf_dl", "vmaf_enc", "d_vmaf",
                                          "d_vmaf_mv"))
    gains = []
    for mv_name, mag, row in sorted(rows, key=lambda r: r[1]):
        d_enc = (row["dlssg_mvenc"] - row["dlssg"]) * 100.0
        d_mv = (row["dlssg_mv"] - row["dlssg"]) * 100.0
        gains.append((mag, d_enc))
        print("%-10s %8.2f %9.3f %9.3f %+9.3f %+9.3f"
              % (mv_name, mag, row["dlssg"], row["dlssg_mvenc"], d_enc, d_mv))

    if len(gains) > 2:
        mags = np.array([g[0] for g in gains], dtype=np.float64)
        ds = np.array([g[1] for g in gains], dtype=np.float64)
        corr = float(np.corrcoef(mags, ds)[0, 1])
        print("\ncorrelation between |mv| and VMAF gain: %.3f" % corr)
        print("gain range: %+.3f .. %+.3f  (mean %+.3f)" % (ds.min(), ds.max(), ds.mean()))

    # MAE per triplet comes from metrics.csv (float plane domain)
    import csv
    mae = {}
    with open(os.path.join(RESULTS, "metrics.csv"), encoding="utf-8") as f:
        for r in csv.DictReader(f):
            mae[(r["triplet"], r["method"])] = float(r["mae_gt"])
    deltas = [mae[(k, "dlssg_mvenc")] - mae[(k, "dlssg")] for k in sorted({k[0] for k in mae})
              if (k, "dlssg_mvenc") in mae and (k, "dlssg") in mae]
    if deltas:
        d = np.array(deltas)
        print("MAE delta (enc - base): min %+.6f max %+.6f mean %+.6f  (negative = enc better)"
              % (d.min(), d.max(), d.mean()))


if __name__ == "__main__":
    main()
