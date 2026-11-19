r"""Dense-flow comparison of generated frames: rife422 vs dlssg on 测试数据\1080p.

Two questions, both answered with OpenCV's DIS dense flow on luma:

1. Where in time does the generated frame sit? For a true midpoint the flow A->M and M->B must
   each be half of the flow A->B. GT is run through the same measure, so every number can be read
   against what a real frame scores rather than against zero.
2. Does the frame agree with what classic flow interpolation would produce? Warp A forward and B
   backward along half of F_ab, average, and compare against the model's frame.

Outputs land under 测试数据\results: metrics.csv/json, flow\*.npy + colour maps, compare\* panels.
"""

import csv
import json
import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import cv2

FLOW_SCALE = 0.5  # stored flows are half resolution: full 1080p flow is ~200 MB as float16
KEYS = ("mae_gt", "psnr_gt", "flow_a2m_vs_half", "flow_m2b_vs_half", "flow_ratio_a2m",
        "frame_vs_flowblend", "flowblend_mae_gt")


def make_dis():
    dis = cv2.DISOpticalFlow_create(cv2.DISOpticalFlow_PRESET_MEDIUM)
    dis.setUseSpatialPropagation(True)
    return dis


def flow_of(dis, src_u8, dst_u8):
    return dis.calc(src_u8, dst_u8, None)


def resize_flow(flow, scale):
    """Scale a flow field and its magnitudes consistently when resizing the grid."""
    h, w = flow.shape[:2]
    small = cv2.resize(flow, (int(round(w * scale)), int(round(h * scale))),
                       interpolation=cv2.INTER_AREA)
    return small * scale


def flow_color(flow):
    """Middlebury-style HSV picture of a flow field, for eyeballing only."""
    mag, ang = cv2.cartToPolar(flow[:, :, 0], flow[:, :, 1])  # ang in degrees
    hsv = np.zeros((flow.shape[0], flow.shape[1], 3), np.uint8)
    hsv[:, :, 0] = (ang * 0.5).astype(np.uint8)  # 360 degrees -> 180 hue steps
    hsv[:, :, 1] = 255
    hsv[:, :, 2] = np.clip(mag * 12.0, 0, 255).astype(np.uint8)
    return cv2.cvtColor(hsv, cv2.COLOR_HSV2RGB)


def warp_mid(src, flow_ab, sign):
    """Sample src at the midpoint position: sign=-1 walks back from B, +1 walks forward from A."""
    h, w = flow_ab.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    mapx = gx + sign * 0.5 * flow_ab[:, :, 0]
    mapy = gy + sign * 0.5 * flow_ab[:, :, 1]
    return cv2.remap(src, mapx, mapy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse <= 0 else 10.0 * np.log10(1.0 / mse)


def heat(error, vmax):
    """Red-hot error map: black = 0, saturated = vmax pixels of flow disagreement."""
    scaled = np.clip(error / max(vmax, 1e-6), 0.0, 1.0)
    return cv2.applyColorMap((scaled * 255).astype(np.uint8), cv2.COLORMAP_INFERNO)[:, :, ::-1]


def label(img_rgb, text):
    out = np.ascontiguousarray(img_rgb)
    cv2.rectangle(out, (0, 0), (len(text) * 11 + 12, 30), (0, 0, 0), -1)
    cv2.putText(out, text, (6, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2,
                cv2.LINE_AA)
    return out


def measure(dis, A, ref_frame, B, a8, b8, half_ab, half_mag):
    """Flow-placement and flow-synthesis metrics for one candidate midpoint frame."""
    m8 = vd.to_u8(vd.to_gray(ref_frame))
    f_am = flow_of(dis, a8, m8)
    f_mb = flow_of(dis, m8, b8)
    return {
        "f_am": f_am,
        "flow_a2m_vs_half": float(np.mean(np.linalg.norm(f_am - half_ab, axis=2))),
        "flow_m2b_vs_half": float(np.mean(np.linalg.norm(f_mb - half_ab, axis=2))),
        "flow_ratio_a2m": float(np.mean(np.linalg.norm(f_am, axis=2)) /
                                max(half_mag, 1e-6)),
    }


def main():
    os.makedirs(vd.RESULTS, exist_ok=True)
    triples = vd.triplets()
    methods = list(vd.METHODS)

    ref_dir = os.path.join(vd.RESULTS, "flow", "ref")
    os.makedirs(ref_dir, exist_ok=True)
    for m in methods:
        os.makedirs(os.path.join(vd.RESULTS, "flow", m), exist_ok=True)
    os.makedirs(os.path.join(vd.RESULTS, "compare"), exist_ok=True)

    dis = make_dis()
    rows = []
    for k, (a_path, gt_path, b_path) in enumerate(triples):
        name = vd.gt_name(gt_path).replace(".png", "")
        A, GT, B = vd.read_rgb(a_path), vd.read_rgb(gt_path), vd.read_rgb(b_path)
        a8, g8, b8 = vd.to_u8(vd.to_gray(A)), vd.to_u8(vd.to_gray(GT)), vd.to_u8(vd.to_gray(B))

        f_ab = flow_of(dis, a8, b8)
        half_ab = 0.5 * f_ab
        half_mag = float(np.mean(np.linalg.norm(half_ab, axis=2)))

        np.save(os.path.join(ref_dir, name + "_a2b.npy"), resize_flow(f_ab, FLOW_SCALE)
                .astype(np.float16))
        vd.write_rgb8(os.path.join(ref_dir, name + "_a2b.png"), flow_color(f_ab) / 255.0)

        # Classic half-and-half flow interpolation from the real frames: the reference for
        # "does the model's frame look like what flow warping would give".
        flow_blend = 0.5 * (warp_mid(A, f_ab, +1.0) + warp_mid(B, f_ab, -1.0))
        blend_mae_gt = float(np.mean(np.abs(flow_blend - GT)))

        panels = [label(vd.to_u8(A), "A"), label(vd.to_u8(GT), "GT"),
                  label(vd.to_u8(flow_blend), "flow blend")]
        flow_panels = [label(flow_color(f_ab), "F A->B (ref)")]

        candidates = [(m, vd.read_rgb16(vd.frame_path(m, gt_path))) for m in methods]
        candidates.append(("gt", GT))  # the reference row: a real frame through the same measure

        for m, frame in candidates:
            stat = measure(dis, A, frame, B, a8, b8, half_ab, half_mag)
            mae_gt = float(np.mean(np.abs(frame - GT)))
            rows.append({
                "triplet": name, "method": m,
                "mae_gt": round(mae_gt, 6), "psnr_gt": round(psnr(frame, GT), 3),
                "flow_a2m_vs_half": round(stat["flow_a2m_vs_half"], 4),
                "flow_m2b_vs_half": round(stat["flow_m2b_vs_half"], 4),
                "flow_ratio_a2m": round(stat["flow_ratio_a2m"], 4),
                "frame_vs_flowblend": round(float(np.mean(np.abs(frame - flow_blend))), 6),
                "flowblend_mae_gt": round(blend_mae_gt, 6),
            })

            err = np.linalg.norm(stat["f_am"] - half_ab, axis=2)
            flow_panels.append(label(flow_color(stat["f_am"]), m + " F A->M"))
            flow_panels.append(label(heat(err, 4.0), m + " err vs half"))
            if m == "gt":
                np.save(os.path.join(ref_dir, name + "_a2gt.npy"),
                        resize_flow(stat["f_am"], FLOW_SCALE).astype(np.float16))
                continue

            np.save(os.path.join(vd.RESULTS, "flow", m, name + "_a2m.npy"),
                    resize_flow(stat["f_am"], FLOW_SCALE).astype(np.float16))
            vd.write_rgb8(os.path.join(vd.RESULTS, "flow", m, name + "_a2m.png"),
                          flow_color(stat["f_am"]) / 255.0)
            vd.write_rgb8(os.path.join(vd.RESULTS, "flow", m, name + "_err.png"),
                          heat(err, 4.0) / 255.0)
            panels.append(label(vd.to_u8(frame), m))

        strip = np.concatenate([cv2.resize(p, (480, 270), interpolation=cv2.INTER_AREA)
                                for p in panels[:5]], axis=1)
        vd.write_rgb8(os.path.join(vd.RESULTS, "compare", name + "_panel.png"),
                      strip.astype(np.float32) / 255.0)
        strip = np.concatenate([cv2.resize(p, (420, 236), interpolation=cv2.INTER_AREA)
                                for p in flow_panels], axis=1)
        vd.write_rgb8(os.path.join(vd.RESULTS, "compare", name + "_flow.png"),
                      strip.astype(np.float32) / 255.0)
        best = min((r["mae_gt"], r["method"]) for r in rows[-3:] if r["method"] in methods)
        print("%s  half|F|=%.2f  blend_mae_gt=%.5f  best=%s" %
              (name, half_mag, blend_mae_gt, best[1]))

    write_outputs(rows, methods)


def write_outputs(rows, methods):
    with open(os.path.join(vd.RESULTS, "metrics.csv"), "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    groups = methods + ["gt"]
    summary = {}
    for m in groups:
        subset = [r for r in rows if r["method"] == m]
        summary[m] = {key: round(float(np.mean([r[key] for r in subset])), 6) for key in KEYS}
    for m in methods:
        summary[m]["triplets_won"] = sum(
            1 for i in range(len(rows) // len(groups))
            if rows[i * len(groups) + methods.index(m)]["mae_gt"] ==
            min(rows[i * len(groups) + j]["mae_gt"] for j in range(len(methods))))
        summary[m]["triplets_last"] = sum(
            1 for i in range(len(rows) // len(groups))
            if rows[i * len(groups) + methods.index(m)]["mae_gt"] ==
            max(rows[i * len(groups) + j]["mae_gt"] for j in range(len(methods))))

    with open(os.path.join(vd.RESULTS, "metrics.json"), "w", encoding="utf-8") as fh:
        json.dump({"summary": summary, "per_triplet": rows}, fh, indent=1)

    lines = ["# rife4.22 vs dlssg on 测试数据 1080p", "",
             "19 triplets (A=0001 / GT=0002 / B=0003 ...) at 1920x1080. GT is a real frame, so the "
             "GT column shows what the flow measure scores on a genuine midpoint.",
             "Flow figures are DIS dense-flow differences on luma, in pixels per frame.", "",
             "| metric | " + " | ".join(groups) + " |",
             "|---|" + "---|" * len(groups)]
    labels = [("mae_gt", "MAE vs GT"), ("psnr_gt", "PSNR vs GT (dB)"),
              ("flow_a2m_vs_half", "||F(A->M) - 0.5F(A->B)||"),
              ("flow_m2b_vs_half", "||F(M->B) - 0.5F(A->B)||"),
              ("flow_ratio_a2m", "|F(A->M)| / 0.5|F(A->B)|"),
              ("frame_vs_flowblend", "|M - flow blend|"),
              ("flowblend_mae_gt", "|flow blend - GT|")]
    for key, text in labels:
        lines.append("| %s | %s |" % (text, " | ".join(str(summary[m][key]) for m in groups)))
    lines.append("| triplets won on MAE | %s |" %
                 " | ".join(str(summary[m]["triplets_won"]) for m in methods))
    lines.append("| triplets last on MAE | %s |" %
                 " | ".join(str(summary[m]["triplets_last"]) for m in methods))

    with open(os.path.join(vd.RESULTS, "report.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
