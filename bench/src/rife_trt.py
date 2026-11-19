"""TensorRT runner for the vs-mlrt RIFE 4.x ONNX models.

The models take a single 11-plane tensor; the layout is the one vsmlrt builds in
RIFEMerge/get_rife_input (src/probes/probe_rife_layout.py confirmed it against
ground truth):

    0-2   frame A, RGB, [0,1]
    3-5   frame B
    6     timepoint t
    7-8   horizontal / vertical meshgrid, [-1,1]
    9-10  constants 2/(W-1), 2/(H-1)

Planes 7-10 never change for a given size, so they are uploaded once and only the
7 changing planes are copied per pair. The tile multiple differs per model
(32 / 128 / 64 / 64) and is the padding vs-mlrt applies as well.
"""

import math
import os
import sys
import time

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from trt_runner import TrtRunner, build_engine  # noqa: E402

import common as C  # noqa: E402


class RifeRunner:
    def __init__(self, name, onnx_stem, multiple, w, h, fp16=True):
        self.name = name
        self.w, self.h = w, h
        self.wp = int(math.ceil(w / multiple) * multiple)
        self.hp = int(math.ceil(h / multiple) * multiple)
        onnx = os.path.join(C.RIFE_ONNX, onnx_stem + ".onnx")
        engine = os.path.join(C.TRT_CACHE, "rife_%s_%dx%d_fp16.engine" % (name, self.wp, self.hp))
        if not os.path.exists(engine):
            build_engine(onnx, engine, (1, 11, self.hp, self.wp), fp16=fp16)
        self.runner = TrtRunner(engine, (1, 11, self.hp, self.wp), fp16=fp16)
        self.src = self.runner.src
        self._fill_static()
        self._feed = np.empty((7, self.hp, self.wp), np.float32)
        self._t = torch.empty((7, self.hp, self.wp), dtype=self.runner.dtype, device="cuda")

    def _fill_static(self):
        """Meshgrid and the two 2/(n-1) constants: identical for every pair."""
        y = torch.arange(self.hp, dtype=torch.float32, device="cuda")
        x = torch.arange(self.wp, dtype=torch.float32, device="cuda")
        gx = (2.0 * x / (self.wp - 1) - 1.0).view(1, 1, self.wp).expand(1, self.hp, self.wp)
        gy = (2.0 * y / (self.hp - 1) - 1.0).view(1, self.hp, 1).expand(1, self.hp, self.wp)
        self.src[0, 7].copy_(gx[0])
        self.src[0, 8].copy_(gy[0])
        self.src[0, 9].fill_(2.0 / (self.wp - 1))
        self.src[0, 10].fill_(2.0 / (self.hp - 1))

    def _pack(self, a, b, t):
        """uint8 HxWx3 frames -> the 7 changing planes, edge-padded to the tile size."""
        cols = self.wp - self.w
        rows = self.hp - self.h
        for idx, img in ((0, a), (3, b)):
            f = img.astype(np.float32) * (1.0 / 255.0)
            if rows or cols:
                f = np.pad(f, ((0, rows), (0, cols), (0, 0)), mode="edge")
            self._feed[idx:idx + 3] = f.transpose(2, 0, 1)
        self._feed[6] = t
        return self._feed

    def tween(self, a, b, t=0.5, timed=False):
        """Interpolate one intermediate frame. Returns (rgb float32 HxWx3, ms, gpu_ms)."""
        feed = self._pack(a, b, t)
        t0 = time.perf_counter()
        self._t.copy_(torch.from_numpy(feed))
        self.src[0, 0:7].copy_(self._t)
        t1 = time.perf_counter()
        if not self.runner.context.execute_async_v3(self.runner.stream):
            raise SystemExit("execute_async_v3 failed")
        torch.cuda.synchronize()
        t2 = time.perf_counter()
        out = self.runner.dst[0, 0:3, :self.h, :self.w].permute(1, 2, 0).float().cpu().numpy()
        ms = (t2 - t0) * 1000.0
        gpu_ms = (t2 - t1) * 1000.0
        return np.clip(out, 0.0, 1.0), ms, gpu_ms

    def close(self):
        self.runner = None
        self.src = self._t = self._feed = None
        torch.cuda.empty_cache()
