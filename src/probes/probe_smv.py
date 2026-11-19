"""Feasibility probe: run each SmoothMyVideo frame-generation backend through its
offline host (smv-live.exe) on a synthetic clip, and report exit code, output
frame count and wall time.

The point is to find out which backends this machine can actually run before the
real benchmark; every backend should emit (N-1) generated frames for --multi 2.
"""
import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import common as C  # noqa: E402

ENGINE = C.SMV_ENGINE
EXE = os.path.join(ENGINE, "live", "smv-live.exe")
CACHE = C.SMV_CACHE
PY = sys.executable

BACKENDS = [
    ("dlssg", ["--dlssg"]),
    ("xess", ["--xess-fg"]),
    ("vfg", ["--nvvfg"]),
    ("fsr4", ["--fsrfg"]),
    ("rife", []),  # default model is RIFE 4.25
]


def make_clip(w, h, n):
    """Moving box on a textured background: cheap, unambiguous motion."""
    frames = []
    yy, xx = np.mgrid[0:h, 0:w]
    bg = ((xx * 3 + yy * 2) % 64 + 96).astype(np.uint8)
    for i in range(n):
        f = np.stack([bg] * 3, axis=2)
        cx, cy = int(w * 0.1 + i * w * 0.05), int(h * 0.5)
        f[max(0, cy - h // 8):cy + h // 8, max(0, cx - w // 8):cx + w // 8] = (240, 40, 40)
        frames.append(f)
    return b"".join(f.tobytes() for f in frames)


def run(name, flags, w=480, h=270, n=12, timeout=600):
    data = make_clip(w, h, n)
    cmd = [EXE, "--offline", "--w", str(w), "--h", str(h), "--multi", "2",
           "--frames", str(n), "--pixfmt", "rgb24", "--out-pixfmt", "rgb24",
           "--cache", CACHE] + flags
    env = dict(os.environ)
    env.pop("SMV_DLSSG_LIMIT", None)
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, cwd=ENGINE, input=data, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        print("%-6s TIMEOUT" % name)
        return False
    dt = time.perf_counter() - t0
    fb = w * h * 3
    nout = len(p.stdout) // fb
    err = p.stderr.decode("utf-8", "replace").strip().splitlines()
    print("%-6s exit=%-3d out=%-4d (expect %d) %7.2f s  %.1f ms/pair"
          % (name, p.returncode, nout, n - 1, dt, dt * 1000.0 / max(1, n - 1)))
    for line in err[-6:]:
        print("        |", line)
    return p.returncode == 0 and nout >= (n - 1)


if __name__ == "__main__":
    which = sys.argv[1:] or [b[0] for b in BACKENDS]
    ok = {}
    for name, flags in BACKENDS:
        if name in which:
            ok[name] = run(name, flags)
    print("\nsummary:", {k: ("OK" if v else "FAIL") for k, v in ok.items()})
