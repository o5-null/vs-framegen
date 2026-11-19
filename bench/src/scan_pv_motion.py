"""Pick 3 moments in the 4K PV where motion is moderate (adjacent frames 24-34 dB apart).

Very low PSNR between neighbours means a fast pan where every interpolator frays; those
are poor comparison material. Very high means almost no motion, where all engines look
identical. This prints the per-timestamp numbers so the positions can be chosen honestly.
"""

import os
import subprocess

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FFMPEG = os.path.join(ROOT, "参考插件", "SmoothMyVideo-protable", "resources", "engine", "bin", "ffmpeg.exe")
SRC = os.path.join(ROOT, "测试数据", "阿哈角色PV 4k.mkv")
OUT = os.path.join(os.environ.get("TEMP", "."), "opencode", "pv_scan")


def read(p):
    img = cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)
    return img[:, :, ::-1].astype(np.float64)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse <= 1e-12 else 10 * np.log10(255.0 ** 2 / mse)


def motion(t):
    d = os.path.join(OUT, "t%03d" % int(t))
    os.makedirs(d, exist_ok=True)
    tmpl = os.path.join(d, "f%02d.png")
    subprocess.run([FFMPEG, "-y", "-v", "error", "-ss", "%.2f" % t, "-i", SRC,
                    "-frames:v", "3", "-vf", "scale=960:-1", tmpl], check=True)
    f = [read(os.path.join(d, "f%02d.png" % i)) for i in (1, 2, 3)]
    return psnr(f[0], f[1]), psnr(f[1], f[2]), psnr(f[0], f[2])


os.makedirs(OUT, exist_ok=True)
for t in range(5, 186, 5):
    a, b, c = motion(t)
    flag = "  <-- candidate" if 26 <= (a + b) / 2 <= 34 else ""
    print("t=%3ds  n/n+1 %5.1f  n+1/n+2 %5.1f  n/n+2 %5.1f%s" % (t, a, b, c, flag), flush=True)
