"""Run _time_one.py for a few models and sizes, printing a compact table."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = r"E:\code\VFI\.venv\Scripts\python.exe"
MODELS = r"C:\Users\benben\AppData\Local\Temp\opencode\rifemodels\x"

CASES = [
    ("rife_v4.16_lite", 3840, 2160, "p8", 2),
    ("rife_v4.26", 3840, 2160, "p8", 2),
    ("rife_v4.17_ensemble", 3840, 2160, "p8", 2),
    ("rife_v4.26_heavy", 3840, 2160, "p8", 2),
    ("rife_v4.16_lite", 1920, 1080, "p10", 2),
    ("rife_v4.16_lite", 1920, 1080, "p444", 2),
    ("rife_v4.16_lite", 1920, 1080, "p8", 3),
    ("rife_v4.16_lite", 1920, 1080, "p8", 4),
]

for name, w, h, fmt, factor in CASES:
    p = subprocess.run([PY, os.path.join(HERE, "_time_one.py"),
                        os.path.join(MODELS, name + ".onnx"), str(w), str(h), fmt, str(factor)],
                       capture_output=True, text=True, timeout=300)
    out = [l for l in p.stdout.splitlines() if l.strip()]
    print(out[-1] if out else "FAIL %s: %s" % (name, p.stderr.strip()[-160:]), flush=True)
