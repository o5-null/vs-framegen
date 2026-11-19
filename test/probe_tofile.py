"""Why does buf.tofile() fail on the flow output path?"""

import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

targets = [
    os.path.join(vd.RESULTS, "flow", "ref", "_probe.png"),
    os.path.join(vd.RESULTS, "probe2.png"),
    os.path.join(vd.RESULTS, "flow", "probe3.png"),
]
for path in targets:
    d = os.path.dirname(path)
    print("dir:", d, "exists:", os.path.isdir(d))
    try:
        os.makedirs(d, exist_ok=True)
        np.array([1, 2, 3], np.uint8).tofile(path)
        print("  ok   ", path, os.path.getsize(path))
        os.remove(path)
    except OSError as exc:
        print("  FAIL ", path, exc)
print("cwd:", os.getcwd())
print("flow dir listing:", os.listdir(os.path.join(vd.RESULTS, "flow")) if os.path.isdir(os.path.join(vd.RESULTS, "flow")) else "none")
