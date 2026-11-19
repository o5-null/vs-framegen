"""Per-triplet MAE / flow table from results/metrics.json.

metrics.json stores one flat row per (triplet, method), so the table is built by grouping on the
triplet name. Writes both a console table and results\triplets.md.
"""

import json
import os
import sys

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

with open(os.path.join(vd.RESULTS, "metrics.json"), encoding="utf-8") as fh:
    data = json.load(fh)

by_triplet = {}
for row in data["per_triplet"]:
    by_triplet.setdefault(row["triplet"], {})[row["method"]] = row

order = ["rife422", "dlssg", "gt"]
lines = ["| triplet | half\\|F\\| (px) | rife422 MAE | dlssg MAE | ratio rife | ratio dlssg | winner |",
         "|---|---|---|---|---|---|---|"]
wins = {"rife422": 0, "dlssg": 0}
for name in sorted(by_triplet):
    m = by_triplet[name]
    ref = m["rife422"]
    r, d = m["rife422"]["mae_gt"], m["dlssg"]["mae_gt"]
    win = "rife422" if r < d else "dlssg"
    wins[win] += 1
    # half|F| is not stored; flowblend_mae_gt is the same for both methods, so it marks the pair.
    lines.append("| %s | — | %.6f | %.6f | %.3f | %.3f | %s |" % (
        name, r, d, ref["flow_ratio_a2m"], m["dlssg"]["flow_ratio_a2m"], win))

print("\n".join(lines))
print("wins:", wins)

with open(os.path.join(vd.RESULTS, "triplets.md"), "w", encoding="utf-8") as fh:
    fh.write("# 逐组结果（19 组三元组）\n\n")
    fh.write("ratio = |F(A->M)| / (0.5|F(A->B)|)，1.0 表示位移量与真中间帧一致；"
             "越小说明模型把位移做小了（画面更保守/更糊）。\n\n")
    fh.write("\n".join(lines) + "\n")
