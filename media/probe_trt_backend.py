"""Is the TensorRT-RTX path usable here, and what does it report?"""

import importlib
import sys

sys.path.insert(0, r"E:\code\VFI\VFI-gui")

for name in ("tensorrt_rtx", "tensorrt", "onnx", "onnxruntime", "pycuda", "cuda"):
    try:
        mod = importlib.import_module(name)
        version = getattr(mod, "__version__", "?")
        print("MOD %-14s %s" % (name, version))
    except Exception as exc:
        print("MOD %-14s MISSING %s" % (name, type(exc).__name__))

try:
    from core.types import BackendType
    from core.backends import BackendFactory

    available = BackendFactory.get_available_backends()
    print("BACKENDS %s" % sorted(str(b) for b in available))
    print("HAS_TRT_RTX %s" % (BackendType.TENSORRT_RTX in available))
except Exception as exc:
    print("BACKENDS probe failed: %s: %s" % (type(exc).__name__, exc))
