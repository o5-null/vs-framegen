"""RIFE reference for the same 10 triplets, so DLSS-G has an AI-interpolator yardstick.

Uses rife-ncnn-vulkan (same model family as vs-rife, runs on Vulkan, no VapourSynth needed).
Costs per case are printed so the throughput question can be answered from the same run.
"""

import json
import os
import subprocess
import time

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
WORK = os.path.join(ROOT, "work")
RIFE_EXE = os.path.join(ROOT, "rife", "rife-ncnn-vulkan-20221029-windows", "rife-ncnn-vulkan.exe")
MODEL = "rife-v4.6"
W, H = 1920, 1080


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def raw_to_png(raw, png):
    run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", "%dx%d" % (W, H), "-i", raw, "-frames:v", "1", png])


def png_to_rgb(png, raw):
    run(["ffmpeg", "-v", "error", "-y", "-i", png, "-pix_fmt", "rgb24", "-f", "rawvideo", raw])


def read_ppm(path):
    with open(path, "rb") as f:
        return f.read().split(b"\n", 3)[3]


def mae(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a) / 255.0


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
        a_png = os.path.join(WORK, name + "_a.png")
        b_png = os.path.join(WORK, name + "_b.png")
        out_png = os.path.join(WORK, name + "_rife.png")
        out_rgb = os.path.join(WORK, name + "_rife.rgb")
        raw_to_png(os.path.join(FRAMES, name + "_a.rgb"), a_png)
        raw_to_png(os.path.join(FRAMES, name + "_b.rgb"), b_png)

        t0 = time.perf_counter()
        run([RIFE_EXE, "-0", a_png, "-1", b_png, "-o", out_png, "-m", MODEL, "-g", "0", "-j", "1:1:1"])
        ms = (time.perf_counter() - t0) * 1000.0
        png_to_rgb(out_png, out_rgb)

        with open(os.path.join(FRAMES, name + "_a.rgb"), "rb") as f:
            a = f.read()
        with open(os.path.join(FRAMES, name + "_b.rgb"), "rb") as f:
            b = f.read()
        blend_rgb = os.path.join(WORK, name + "_blend.rgb")
        with open(blend_rgb, "wb") as f:
            f.write(bytes((x + y) // 2 for x, y in zip(a, b)))

        with open(out_rgb, "rb") as f:
            rife = f.read()
        with open(gt, "rb") as f:
            g = f.read()

        gen_rgb = os.path.join(WORK, name + "_gen.rgb")
        with open(gen_rgb, "wb") as f:
            f.write(read_ppm(os.path.join(WORK, "spike_" + name + ".ppm")))

        row = {
            "rife": vmaf(out_rgb, gt),
            "dlssg": vmaf(gen_rgb, gt),
            "blend": vmaf(blend_rgb, gt),
            "rife_mae": mae(rife, g),
            "dlssg_mae": mae(read_ppm(os.path.join(WORK, "spike_" + name + ".ppm")), g),
            "ms": ms,
        }
        print("%s: vmaf rife=%.2f dlssg=%.2f blend=%.2f | mae rife=%.5f dlssg=%.5f | rife %.0f ms"
              % (name, row["rife"], row["dlssg"], row["blend"], row["rife_mae"], row["dlssg_mae"], ms))
        rows.append(row)

    n = len(rows)
    for key in ("rife", "dlssg", "blend"):
        print("mean vmaf %-6s: %.2f" % (key, sum(r[key] for r in rows) / n))
    for key in ("rife_mae", "dlssg_mae"):
        print("mean mae  %-6s: %.5f" % (key, sum(r[key] for r in rows) / n))
    print("mean rife wall time: %.0f ms/frame (includes process start + PNG I/O)" % (sum(r["ms"] for r in rows) / n))
    print("rife beats dlssg on %d/%d cases (VMAF)" % (sum(1 for r in rows if r["rife"] > r["dlssg"]), n))


if __name__ == "__main__":
    main()
