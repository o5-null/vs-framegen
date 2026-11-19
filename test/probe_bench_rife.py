r"""RIFE 4.22 TRT throughput on its own, with per-iteration times.

Bench started right after a DLSS-G run reported 550-950 ms per frame, against 60 ms in the build
probe - either the host copy in run() dominates or the GPU was busy. Printing every iteration tells
those apart.
"""

import os
import sys

import numpy as np
import torch

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
from trt_rife422 import TrtRunner

ENGINE = r"E:\code\dlssg-vfi\work\engines\rife422_1080p_fp16.engine"
SHAPE = (1, 7, 1088, 1920)


def main():
    runner = TrtRunner(ENGINE, SHAPE)
    rng = np.random.default_rng(0)
    feed = rng.random(SHAPE, dtype=np.float32)

    # Pre-uploaded tensor: measures pure execute, so the host copy can be separated out.
    pre = torch.from_numpy(feed).to(runner.dtype)
    ctx, src, stream = runner.context, runner.src, runner.stream

    print("A) run(feed) as the generation script calls it")
    for i in range(12):
        t0 = torch.cuda.Event(True)
        t1 = torch.cuda.Event(True)
        t0.record()
        runner.run(feed)
        t1.record()
        torch.cuda.synchronize()
        print("  %2d  %.1f ms" % (i, t0.elapsed_time(t1)))

    print("B) execute only, input already on the device")
    for i in range(12):
        t0 = torch.cuda.Event(True)
        t1 = torch.cuda.Event(True)
        t0.record()
        ctx.execute_async_v3(stream)
        t1.record()
        torch.cuda.synchronize()
        print("  %2d  %.1f ms" % (i, t0.elapsed_time(t1)))

    print("C) host->device copy only")
    for i in range(5):
        t0 = torch.cuda.Event(True)
        t1 = torch.cuda.Event(True)
        t0.record()
        src.copy_(pre)
        t1.record()
        torch.cuda.synchronize()
        print("  %2d  %.1f ms" % (i, t0.elapsed_time(t1)))


if __name__ == "__main__":
    main()
