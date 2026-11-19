"""Perceptual scoreboard for the DLSS-G spike: VMAF of every candidate against the true middle frame.

MAE rewards blur (grain averages out), so a blurry blend can look "good" on it. VMAF is the metric
people actually trust for this, and it is in the gyan ffmpeg build.
"""

import json
import os
import subprocess
import tempfile

MEDIA = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(MEDIA, "frames")
WORK = os.path.join(os.path.dirname(MEDIA), "work")
W, H = 1920, 1080


def read_ppm(path):
    with open(path, "rb") as f:
        return f.read().split(b"\n", 3)[3]


def write_rgb(path, data):
    with open(path, "wb") as f:
        f.write(data)


def vmaf(test_path, ref_path):
    log_name = "vmaf_log.json"
    log_path = os.path.join(WORK, log_name)
    if os.path.exists(log_path):
        os.unlink(log_path)
    cmd = [
        "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24", "-i", test_path,
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24", "-i", ref_path,
        "-lavfi", "[0]format=yuv420p[a];[1]format=yuv420p[b];[a][b]libvmaf=log_fmt=json:log_path=%s"
        % log_name,
        "-f", "null", "-",
    ]
    subprocess.run(cmd, check=True, cwd=WORK)
    with open(log_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    os.unlink(log_path)
    return float(data["pooled_metrics"]["vmaf"]["mean"])


def main():
    names = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    for name in names:
        gt = os.path.join(FRAMES, name + "_gt.rgb")
        if not os.path.exists(gt):
            continue
        with open(os.path.join(FRAMES, name + "_a.rgb"), "rb") as f:
            a = f.read()
        with open(os.path.join(FRAMES, name + "_b.rgb"), "rb") as f:
            b = f.read()

        gen_rgb = os.path.join(WORK, name + "_gen.rgb")
        write_rgb(gen_rgb, read_ppm(os.path.join(WORK, "spike_" + name + ".ppm")))
        blend_rgb = os.path.join(WORK, name + "_blend.rgb")
        write_rgb(blend_rgb, bytes((x + y) // 2 for x, y in zip(a, b)))
        mi_rgb = os.path.join(FRAMES, name + "_minterp.rgb")

        scores = {"dlssg": vmaf(gen_rgb, gt), "blend": vmaf(blend_rgb, gt)}
        if os.path.exists(mi_rgb):
            scores["minterp"] = vmaf(mi_rgb, gt)
        print("%s: dlssg=%.2f minterp=%.2f blend=%.2f" % (
            name, scores["dlssg"], scores.get("minterp", float("nan")), scores["blend"]))
        rows.append(scores)

    if rows:
        n = len(rows)
        for key in ("dlssg", "minterp", "blend"):
            vals = [r[key] for r in rows if key in r]
            print("mean %-8s: %.2f" % (key, sum(vals) / len(vals)))
        wins = sum(1 for r in rows if r["dlssg"] > r.get("minterp", -1))
        print("dlssg beats minterp on %d/%d cases (VMAF)" % (wins, n))


if __name__ == "__main__":
    main()
