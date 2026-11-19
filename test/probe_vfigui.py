"""Check which VFI-gui runtime dependencies exist in a given interpreter."""

import importlib.util
import sys

MODULES = [
    "av", "cv2", "numpy", "torch", "torchvision", "imageio", "imageio_ffmpeg",
    "PyQt6", "onnxruntime", "tensorrt", "einops", "scipy", "tqdm",
    "safetensors", "vapoursynth", "psutil", "pynvml",
]

print(sys.version)
print(sys.executable)
for name in MODULES:
    try:
        found = importlib.util.find_spec(name) is not None
    except Exception:
        found = False
    print(f"{name:18s} {'YES' if found else 'no'}")
