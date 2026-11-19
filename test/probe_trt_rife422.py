"""Build the rife 4.22 engine once and benchmark it, so the comparison run is not a build run."""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trt_rife422 import ENGINE_DIR, TrtRunner, build_engine  # noqa: E402

ONNX = r"E:\code\VFI\VFI-gui\models\rife_v2\rife_v4.22.onnx"
SHAPE = (1, 7, 1088, 1920)
ENGINE = os.path.join(ENGINE_DIR, "rife422_1080p_fp16.engine")


def main():
    if not os.path.exists(ENGINE):
        build_engine(ONNX, ENGINE, SHAPE, fp16=True)
    runner = TrtRunner(ENGINE, SHAPE, fp16=True)
    print("output shape", runner.out_shape)

    feed = np.random.rand(*SHAPE).astype(np.float32)
    times = []
    for _ in range(5):
        t0 = time.perf_counter()
        out = runner.run(feed)
        times.append((time.perf_counter() - t0) * 1000.0)
    print("trt ms: %s" % ", ".join("%.1f" % t for t in times))
    arr = out.float().cpu().numpy()
    print("output finite:", bool(np.isfinite(arr).all()), "range %.3f..%.3f" % (arr.min(), arr.max()))


if __name__ == "__main__":
    main()
