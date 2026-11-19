"""Smoke test for RifeRunner: build one engine, check the tween lands on the real
middle frame, and print the per-pair cost."""
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import common as C
from rife_trt import RifeRunner

W, H = 1280, 720
name, stem, mult = C.RIFE_MODELS[0]


def main():
    frames, names = C.odd_frames(W, H)
    t0 = time.perf_counter()
    run = RifeRunner(name, stem, mult, W, H)
    print("engine ready in %.1f s" % (time.perf_counter() - t0))

    for i in (0, 8, 17):
        a, b = frames[i], frames[i + 1]
        gt = C.load_frame(names[2 * i + 1], W, H).astype(np.float32) / 255.0
        out, ms, gpu_ms = run.tween(a, b)
        print("%s  mae-vs-gt %.4f  mae-vs-blend %.4f  %.1f ms (gpu %.1f)"
              % (names[2 * i + 1], np.abs(out - gt).mean(),
                 np.abs(out - (a.astype(np.float32) / 255.0 + b.astype(np.float32) / 255.0) / 2).mean(),
                 ms, gpu_ms))
    run.close()


if __name__ == "__main__":
    main()
