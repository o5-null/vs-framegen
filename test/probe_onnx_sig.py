"""Print the rife_v4.22 ONNX signature (and a couple of neighbours) via onnxruntime."""

import glob
import os

import onnxruntime as ort

base = r"E:\code\VFI\VFI-gui\models\rife_v2"
for name in ("rife_v4.22.onnx", "rife_v4.22_lite.onnx", "rife_v4.26.onnx"):
    path = os.path.join(base, name)
    if not os.path.exists(path):
        print(name, "MISSING")
        continue
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    ins = [(i.name, i.shape, i.type) for i in sess.get_inputs()]
    outs = [(o.name, o.shape, o.type) for o in sess.get_outputs()]
    print("%s (%.1f MB)" % (name, os.path.getsize(path) / 1e6))
    for i in ins:
        print("   in ", i)
    for o in outs:
        print("   out", o)
