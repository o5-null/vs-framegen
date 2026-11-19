"""Uniform benchmark: one engine, fixed source range; splits GPU time from host time.

Each engine is timed on the same 24 source frames (sintel 90..113, 1080p) so the numbers in docs/engines.md
are comparable; the engines have different multi ceilings, so pass the multiplier in. With LOG=1 the plugin's
own per-frame GPU time (timestamp queries for DLSS/XeSS, the model's stream timing for VFG, the bridge wait
for FSR 4) is collected too, so wall time minus GPU time is what the host still costs.
"""
import os
import re
import sys
import tempfile
import time

import vapoursynth as vs

core = vs.core

PLAN = {
    "dlss": ("vdlssg", 6, "dlss45"),
    "xess": ("vxessfg", 4, "xessfg"),
    "vfg": ("vvfg", 4, "vfg"),
    "fsr4": ("vfsr4", 2, "fsr4"),
}


def find_pkg():
    here = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.exists(os.path.join(here, "bin", "vfsr4.dll")):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("bin/vfsr4.dll not found above " + __file__)
        here = parent


pkg = find_pkg()
label = sys.argv[1]
fn, default_multi, proxy = PLAN[label]
multi = int(sys.argv[2]) if len(sys.argv) > 2 else default_multi
log = os.environ.get("LOG", "0") != "0"

stderr_path = None
if log:
    saved = os.dup(2)
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".log")
    stderr_path = tmp.name
    tmp.close()
    fd = os.open(stderr_path, os.O_WRONLY | os.O_TRUNC)
    os.dup2(fd, 2)
    os.close(fd)

core.std.LoadPlugin(path=os.path.join(pkg, "bin", fn + ".dll"))
src = core.ffms2.Source(os.path.join(pkg, "..", "media", "sintel_trailer_1080p.mp4"))
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 113)
clip = getattr(core, fn).FrameGen(src, multi=multi, proxy_dir=os.path.join(pkg, "bin", proxy), log=1 if log else 0)

for i in range(min(multi * 2, 8)):  # warm up: engine load / lane creation
    clip.get_frame(i)

t0 = time.perf_counter()
n = 0
for i in range(len(clip)):
    clip.get_frame(i)
    n += 1
dt = time.perf_counter() - t0

gpu = []
if log:
    os.dup2(saved, 2)
    os.close(saved)
    with open(stderr_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r":\s*([0-9.]+)\s*ms\s*$", line)
            if m:
                gpu.append(float(m.group(1)))
    os.unlink(stderr_path)

wall_ms = dt * 1000.0 / n
if gpu:
    g = sorted(gpu)
    mean = sum(g) / len(g)
    print("%-5s multi=%d %3d frames in %5.2f s | wall %.2f ms | gpu mean %.2f (p50 %.2f) | host %.2f ms"
          % (label, multi, n, dt, wall_ms, mean, g[len(g) // 2], wall_ms - mean))
else:
    print("%-5s multi=%d %3d frames in %5.2f s (%6.2f fps, %.2f ms/output frame)"
          % (label, multi, n, dt, n / dt, wall_ms))
