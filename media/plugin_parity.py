"""Plugin-vs-spike parity check on the 10 Sintel triplets.

The spike (work\\spike_tNN.ppm) is the already-measured reference: dlssg mean VMAF 77.11.
If the plugin reproduces those pixels, its quality claim carries over without re-deriving it.

Feed the triplets as lossless RGB (FFV1/gbrp) so the plugin sees the exact same rgb24 input the
spike got - no YUV round trip, no resampler in between.
"""

import os
import subprocess
import sys

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")
PY = r"E:\code\VFI\.venv\Scripts\python.exe"
VSPIPE = r"E:\code\VFI\.venv\Scripts\vspipe.exe"
PLUGIN = os.path.join(ROOT, "plugin", "vdlssg.dll")
PROXY = os.path.join(ROOT, "work")
W, H = 1920, 1080


def make_clip(tag):
    """Two-frame lossless RGB clip: a, b (skipping the true middle)."""
    dst = os.path.join(TRI, tag + ".mkv")
    if os.path.exists(dst):
        return dst
    cmd = [
        "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24",
        "-i", os.path.join(FRAMES, tag + "_a.rgb"),
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24",
        "-i", os.path.join(FRAMES, tag + "_b.rgb"),
        "-filter_complex", "[0][1]concat=n=2:v=1:a=0",
        "-c:v", "ffv1", "-pix_fmt", "gbrp", dst,
    ]
    subprocess.run(cmd, check=True)
    return dst


def script(tag, clip):
    """Generated middle frame only: FrameGen emits source, middle, source, middle, ..."""
    return (
        "import vapoursynth as vs\n"
        "core = vs.core\n"
        'core.std.LoadPlugin(path=r"%s")\n'
        'src = core.ffms2.Source(r"%s")\n'
        "src = core.resize.Bicubic(src, format=vs.RGBS)\n"
        'out = core.vdlssg.FrameGen(src, proxy_dir=r"%s", log=0)\n'
        "out = core.std.Trim(out, 1, 1)\n"
        "out = core.resize.Bicubic(out, format=vs.RGB24)\n"
        "out.set_output()\n"
    ) % (PLUGIN, clip, PROXY)


def run(tag):
    vpy = os.path.join(TRI, tag + ".vpy")
    with open(vpy, "w", encoding="utf-8") as f:
        f.write(script(tag, make_clip(tag)))
    dst = os.path.join(TRI, tag + "_plugin.rgb")
    err = os.path.join(TRI, tag + ".err")
    with open(dst, "wb") as out, open(err, "wb") as errf:
        subprocess.run([VSPIPE, vpy, "-"], stdout=out, stderr=errf, check=True)
    return dst


def read_rgb(path):
    with open(path, "rb") as f:
        return f.read()


def read_ppm(path):
    with open(path, "rb") as f:
        return f.read().split(b"\n", 3)[3]


def mae(x, y):
    n = min(len(x), len(y))
    try:
        import numpy as np
        a = np.frombuffer(x[:n], dtype=np.uint8).astype(np.int16)
        b = np.frombuffer(y[:n], dtype=np.uint8).astype(np.int16)
        return float(np.abs(a - b).mean()) / 255.0
    except ImportError:
        return sum(abs(a - b) for a, b in zip(x[:n], y[:n])) / float(n) / 255.0


def main():
    os.makedirs(TRI, exist_ok=True)
    tags = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    for tag in tags:
        got = read_rgb(run(tag))
        spike_path = os.path.join(ROOT, "work", "spike_" + tag + ".ppm")
        gt = read_rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
        if len(got) != W * H * 3:
            print("%s: bad output size %d" % (tag, len(got)))
            continue
        spike = read_ppm(spike_path) if os.path.exists(spike_path) else None
        vs_spike = mae(got, spike) if spike else float("nan")
        vs_gt = mae(got, gt)
        print("%s: plugin-vs-spike MAE=%.6f  plugin-vs-GT MAE=%.6f" % (tag, vs_spike, vs_gt))
        rows.append((tag, vs_spike, vs_gt))
    if rows:
        print("mean plugin-vs-spike MAE=%.6f  plugin-vs-GT MAE=%.6f" % (
            sum(r[1] for r in rows) / len(rows), sum(r[2] for r in rows) / len(rows)))


if __name__ == "__main__":
    sys.exit(main())
