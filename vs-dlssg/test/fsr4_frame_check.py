"""Print per-frame plane means and adjacent deltas so a generated frame can be told from a hold."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
script = os.path.join(HERE, os.environ.get("FSR4_SCRIPT", "fsr4.vpy"))
ns = {"__file__": script, "__name__": "__main__"}
with open(script, "r", encoding="utf-8") as f:
    exec(compile(f.read(), script, "exec"), ns)

clip = ns["out"]
prev = None
for n in range(min(int(os.environ.get("FSR4_FRAMES", "6")), clip.num_frames)):
    frame = clip.get_frame(n)
    planes = []
    for p in range(3):
        data = np.asarray(frame[p], dtype=np.float32)
        planes.append(float(data.mean()))
    delta = ""
    if prev is not None:
        diff = np.abs(np.asarray(frame[0], np.float32) - prev)
        delta = " mad_vs_prev %.4f" % float(diff.mean())
    prev = np.asarray(frame[0], np.float32)
    print("frame %d  mean R/G/B %.3f %.3f %.3f%s" % (n, planes[0], planes[1], planes[2], delta))
