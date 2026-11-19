"""What is actually installed for a TensorRT RIFE run: TRT, torch, VS plugins, models."""

import importlib.util
import os
import sys

VENV = r"E:\code\VFI\.venv"
PLUGINS = os.path.join(VENV, "Lib", "site-packages", "vapoursynth", "plugins")
MODELS = r"E:\code\VFI\models"

print("python:", sys.version.split()[0])

for name in ("tensorrt", "tensorrt_lean", "torch", "onnx", "onnxruntime", "vapoursynth",
             "vsmlrt", "cupy", "pycuda"):
    spec = importlib.util.find_spec(name) if importlib.util.find_spec else None
    if not spec:
        print("%-14s MISSING" % name)
        continue
    try:
        mod = __import__(name)
        print("%-14s %s  (%s)" % (name, getattr(mod, "__version__", "?"), spec.origin))
    except Exception as exc:  # noqa: BLE001 - report whatever breaks
        print("%-14s present but import failed: %s" % (name, exc))

print("\nvapoursynth plugins dir:", PLUGINS)
if os.path.isdir(PLUGINS):
    for entry in sorted(os.listdir(PLUGINS)):
        print("  ", entry)

print("\nmodels dir:", MODELS)
for root, dirs, files in os.walk(MODELS):
    depth = root[len(MODELS):].count(os.sep)
    if depth > 2:
        dirs[:] = []
        continue
    print("  %s/  (%d files)" % (root[len(MODELS):] or ".", len(files)))
    for f in files[:8]:
        print("      ", f)
