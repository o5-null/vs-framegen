"""Dynamic vs static TensorRT engine for the RIFE ONNX models.

run_throughput.py builds one fixed-shape engine per resolution because a dynamic engine is
suspected to drift on non-opt shapes, which would distort the "cost per pixel" curve. This
probe measures that drift instead of assuming it: one dynamic engine (min 640x360,
opt 1920x1088, max 3840x2176) against the cached static engines, same data path, same
iteration count. Result goes into the report as a footnote.
"""

import os
import statistics
import sys
import time

import numpy as np
import tensorrt as trt
import torch

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import common as C
from trt_runner import _builder, build_engine
import rife_trt

ONNX = C.RIFE_ONNX + r"\rife_v4.6.onnx"
DYN_ENGINE = C.TRT_CACHE + r"\rife_rife4.6_dynamic_fp16.engine"
MIN_SHAPE = (1, 11, 384, 640)   # must stay tile-aligned (32) or TRT rejects the profile
OPT_SHAPE = (1, 11, 1088, 1920)
MAX_SHAPE = (1, 11, 2176, 3840)
ITERS = 20
SHAPES = [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)]


def build_dynamic():
    logger, builder, network, parser = _builder()
    with open(ONNX, "rb") as fh:
        if not parser.parse(fh.read()):
            raise SystemExit("onnx parse failed")
    cfg = builder.create_builder_config()
    cfg.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 4 << 30)
    cfg.set_flag(trt.BuilderFlag.FP16)
    profile = builder.create_optimization_profile()
    name = network.get_input(0).name
    profile.set_shape(name, MIN_SHAPE, OPT_SHAPE, MAX_SHAPE)
    cfg.add_optimization_profile(profile)
    t0 = time.time()
    blob = builder.build_serialized_network(network, cfg)
    if blob is None:
        raise SystemExit("dynamic build failed")
    with open(DYN_ENGINE, "wb") as fh:
        fh.write(blob)
    print("dynamic engine built in %.1f s (%d MB)"
          % (time.time() - t0, os.path.getsize(DYN_ENGINE) // 10**6), flush=True)


class DynRunner:
    """Max-sized buffers, shape set per call - the normal way a dynamic engine is used."""

    def __init__(self, engine_path, max_shape):
        with open(engine_path, "rb") as fh:
            self.engine = trt.Runtime(trt.Logger(trt.Logger.WARNING)).deserialize_cuda_engine(fh.read())
        self.context = self.engine.create_execution_context()
        self.in_name = self.engine.get_tensor_name(0)
        self.out_name = self.engine.get_tensor_name(1)
        self.dtype = {trt.DataType.FLOAT: torch.float32, trt.DataType.HALF: torch.float16}[
            self.engine.get_tensor_dtype(self.in_name)]
        self.shape = tuple(max_shape)
        self.src = torch.zeros(self.shape, dtype=self.dtype, device="cuda")
        self.dst = torch.zeros(self.shape, dtype=self.dtype, device="cuda")
        self.context.set_tensor_address(self.in_name, self.src.data_ptr())
        self.context.set_tensor_address(self.out_name, self.dst.data_ptr())
        self.stream = torch.cuda.current_stream().cuda_stream

    def run(self, feed, h, w):
        # Same data path as RifeRunner without the device->host readback, which neither
        # side pays in a real pipeline (the result stays on the GPU).
        self.src[0, 0:7, :h, :w].copy_(torch.from_numpy(feed))
        self.context.set_input_shape(self.in_name, (1, 11, h, w))
        if not self.context.execute_async_v3(self.stream):
            raise SystemExit("execute_async_v3 failed")
        torch.cuda.synchronize()


def timeit(fn, iters=ITERS):
    for _ in range(3):
        fn()
    t0 = time.perf_counter()
    for _ in range(iters):
        fn()
    return (time.perf_counter() - t0) * 1000 / iters


def main():
    if not os.path.exists(DYN_ENGINE):
        build_dynamic()
    dyn = DynRunner(DYN_ENGINE, MAX_SHAPE)

    print("%-10s %10s %10s %8s" % ("res", "static", "dynamic", "drift"), flush=True)
    rows = []
    for w, h in SHAPES:
        runner = rife_trt.RifeRunner("rife4.6", "rife_v4.6", 32, w, h)
        wp, hp = runner.wp, runner.hp
        feed = np.random.rand(7, hp, wp).astype(np.float32)

        def static_call():
            runner.src[0, 0:7].copy_(torch.from_numpy(feed))
            if not runner.runner.context.execute_async_v3(runner.runner.stream):
                raise SystemExit("execute_async_v3 failed")
            torch.cuda.synchronize()

        ms_static = timeit(static_call)
        ms_dyn = timeit(lambda: dyn.run(feed, hp, wp))
        runner.close()
        rows.append((w, h, ms_static, ms_dyn))
        print("%-10s %9.2fms %9.2fms %+7.1f%%"
              % ("%dx%d" % (w, h), ms_static, ms_dyn, (ms_dyn / ms_static - 1) * 100), flush=True)
    drift = statistics.mean([d / s for _, _, s, d in rows])
    print("mean dynamic/static ratio: %.3f" % drift, flush=True)


if __name__ == "__main__":
    main()
