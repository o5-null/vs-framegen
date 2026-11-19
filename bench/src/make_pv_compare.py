"""Build the 2x2 frame-generation comparison panels from 测试数据\\阿哈角色PV 4k.mkv.

Three moments are taken from the 4K PV. For each, frames n / n+1 / n+2 are cut out:
n and n+2 go through every engine as the pair to interpolate, and n+1 is the ground
truth the panel is judged against. Output: docs\\benchmark\\pv-compare-<k>.png
(2x2 of DLSS-G / XeSS-FG / VFG / FSR4, each labelled with PSNR and SSIM against GT,
plus a full-width ground-truth strip).

Usage: python make_pv_compare.py [--force]
"""

import os
import subprocess
import sys

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
PY = r"E:\code\VFI\.venv\Scripts\python.exe"
FFMPEG = os.path.join(ROOT, "参考插件", "SmoothMyVideo-protable", "resources", "engine", "bin", "ffmpeg.exe")
SRC = os.path.join(ROOT, "测试数据", "阿哈角色PV 4k.mkv")
WORK = os.path.join(os.environ.get("TEMP", "."), "opencode", "pv_compare")
OUT = os.path.join(ROOT, "docs", "benchmark")
ENGINE_SCRIPT = os.path.join(HERE, "pv_engine_frame.py")

ENGINES = [
    ("dlssg", "DLSS-G (NVIDIA, sm86)"),
    ("xessfg", "XeSS-FG (Intel, sm86)"),
    ("vfg", "NVIDIA VFG"),
    ("fsr4", "FSR4 (AMD)"),
]

# (time in seconds, output name) - picked from scan_pv_motion.py: adjacent frames 22-31 dB
# apart, i.e. real motion without a fast pan that every interpolator loses.
POSITIONS = [(10.0, "pv-compare-1"), (55.0, "pv-compare-2"), (120.0, "pv-compare-3")]

CELL_W = 1160
FONT = cv2.FONT_HERSHEY_DUPLEX
GAP = 10
PAD = 20


def extract(t: float, out_dir: str) -> tuple:
    os.makedirs(out_dir, exist_ok=True)
    tmpl = os.path.join(out_dir, "f%02d.png")
    cmd = [FFMPEG, "-y", "-v", "error", "-ss", "%.3f" % t, "-i", SRC,
           "-frames:v", "3", tmpl]
    subprocess.run(cmd, check=True)
    paths = [os.path.join(out_dir, "f%02d.png" % i) for i in (1, 2, 3)]
    for p in paths:
        if not os.path.exists(p):
            raise SystemExit("ffmpeg produced no " + p)
    return paths[0], paths[1], paths[2]


def read_rgb(path: str) -> np.ndarray:
    img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit("cannot read " + path)
    return img[:, :, ::-1].copy()  # BGR -> RGB


def write_png(path: str, rgb: np.ndarray) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    ok, buf = cv2.imencode(".png", rgb[:, :, ::-1])
    if not ok:
        raise SystemExit("cannot encode " + path)
    buf.tofile(path)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse <= 1e-12 else 10.0 * np.log10(255.0 ** 2 / mse)


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Standard 11x11/sigma1.5 SSIM on luma, no sklearn needed."""
    ga = cv2.cvtColor(a, cv2.COLOR_RGB2GRAY).astype(np.float64)
    gb = cv2.cvtColor(b, cv2.COLOR_RGB2GRAY).astype(np.float64)
    k = (11, 11)
    s = 1.5
    mu_a = cv2.GaussianBlur(ga, k, s)
    mu_b = cv2.GaussianBlur(gb, k, s)
    va = cv2.GaussianBlur(ga * ga, k, s) - mu_a * mu_a
    vb = cv2.GaussianBlur(gb * gb, k, s) - mu_b * mu_b
    cov = cv2.GaussianBlur(ga * gb, k, s) - mu_a * mu_b
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    m = ((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    return float(m.mean())


def scale_to(rgb: np.ndarray, width: int) -> np.ndarray:
    h = int(round(rgb.shape[0] * width / rgb.shape[1]))
    return cv2.resize(rgb, (width, h), interpolation=cv2.INTER_AREA)


def label(img: np.ndarray, text: str, x: int, y: int, scale: float = 0.8) -> None:
    # dark halo keeps the text readable over bright frames
    cv2.putText(img, text, (x, y), FONT, scale, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(img, text, (x, y), FONT, scale, (255, 255, 255), 1, cv2.LINE_AA)


def build_panel(gt: np.ndarray, frames: dict, title: str, subtitle: str) -> np.ndarray:
    cell = scale_to(gt, CELL_W)
    ch = cell.shape[0]
    gap_strip_h = 34  # space for the per-cell caption above each row
    panel_w = 2 * CELL_W + GAP + 2 * PAD
    panel_h = 2 * (ch + gap_strip_h) + GAP + (ch * 2 + gap_strip_h) + 2 * PAD + 86
    canvas = np.full((panel_h, panel_w, 3), 18, np.uint8)

    def place(x, y, rgb, cap):
        canvas[y:y + rgb.shape[0], x:x + rgb.shape[1]] = rgb
        if cap:
            label(canvas, cap, x + 8, y - 9, 0.7)

    y0 = PAD + 86
    label(canvas, title, PAD, PAD + 24, 0.85)
    label(canvas, subtitle, PAD, PAD + 50, 0.6)

    for i, (key, name) in enumerate(ENGINES):
        col, row = i % 2, i // 2
        x = PAD + col * (CELL_W + GAP)
        y = y0 + row * (ch + gap_strip_h + GAP)
        rgb = scale_to(frames[key], CELL_W)
        cap = "%s   PSNR %.1f dB   SSIM %.3f" % (name, psnr(frames[key], gt), ssim(frames[key], gt))
        place(x, y, rgb, cap)

    gt_y = y0 + 2 * (ch + gap_strip_h + GAP) + gap_strip_h
    gt_strip = scale_to(gt, 2 * CELL_W + GAP)
    place(PAD, gt_y, gt_strip, "Ground truth (frame n+1, not generated)")
    return canvas


def main() -> int:
    force = "--force" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(WORK, exist_ok=True)
    for index, (t, name) in enumerate(POSITIONS, start=1):
        dest = os.path.join(OUT, name + ".png")
        if os.path.exists(dest) and not force:
            print("cached", dest)
            continue
        run_dir = os.path.join(WORK, name)
        pa, pg, pb = extract(t, run_dir)
        gt = read_rgb(pg)
        frames = {}
        for key, _ in ENGINES:
            out_png = os.path.join(run_dir, key + ".png")
            proc = subprocess.run([PY, ENGINE_SCRIPT, key, pa, pb, out_png],
                                  capture_output=True, text=True, errors="replace", timeout=1800)
            if proc.returncode != 0:
                print(proc.stderr[-1500:])
                raise SystemExit("%s failed on %s" % (key, name))
            frames[key] = read_rgb(out_png)
            print("%-8s %s  psnr %.2f  ssim %.4f" % (key, name, psnr(frames[key], gt), ssim(frames[key], gt)),
                  flush=True)
        title = "Frame generation on the Aha character PV, 4K (3840x1632) - t=%.0fs, frame %d -> %d" % (
            t, int(round(t * 30)), int(round(t * 30)) + 2)
        panel = build_panel(gt, frames, title, "engines interpolate frames n and n+2; n+1 is the real frame")
        write_png(dest, panel)
        print("wrote", dest, panel.shape, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
