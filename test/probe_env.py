import sys

print("python", sys.version.split()[0])
for name in ("cv2", "numpy", "torch", "onnxruntime", "tensorrt", "vapoursynth"):
    try:
        m = __import__(name)
        print(name, getattr(m, "__version__", "?"))
    except Exception as exc:  # noqa: BLE001
        print(name, "MISSING", type(exc).__name__, exc)

try:
    import torch

    print("cuda available:", torch.cuda.is_available())
    if torch.cuda.is_available():
        print("device:", torch.cuda.get_device_name(0))
except Exception:
    pass
