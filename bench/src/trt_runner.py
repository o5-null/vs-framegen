"""TensorRT wrapper for the RIFE ONNX bundles (rife_trt.py feeds the 11-plane layout).

The ONNX takes one NCHW tensor of 7 channels: [img0(3), img1(3), timestep(1)] - confirmed
empirically, t=0 returns img0 and t=1 returns img1. Building here keeps the engine on disk
so the generation script is a cheap re-run.

TensorRT builds the engine itself, so no nvcc or CUDA toolkit is needed - only the
tensorrt python package and a working CUDA context (torch provides both).
"""

import os

import numpy as np
import tensorrt as trt
import torch

import common as C

ENGINE_DIR = C.TRT_CACHE


def _builder():
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network(1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    parser = trt.OnnxParser(network, logger)
    return logger, builder, network, parser


def build_engine(onnx_path, engine_path, profile_shape, fp16=True, workspace_gb=4):
    """Build (and serialise) an engine with a fixed shape - the clip size never changes here."""
    logger, builder, network, parser = _builder()
    with open(onnx_path, "rb") as fh:
        if not parser.parse(fh.read()):
            for i in range(parser.num_errors):
                print("parse error:", parser.get_error(i))
            raise SystemExit("onnx parse failed: " + onnx_path)

    cfg = builder.create_builder_config()
    cfg.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, workspace_gb << 30)
    if fp16:
        cfg.set_flag(trt.BuilderFlag.FP16)
    profile = builder.create_optimization_profile()
    name = network.get_input(0).name
    profile.set_shape(name, profile_shape, profile_shape, profile_shape)
    cfg.add_optimization_profile(profile)

    print("building engine %s %s fp16=%s ..." % (os.path.basename(onnx_path), profile_shape, fp16))
    blob = builder.build_serialized_network(network, cfg)
    if blob is None:
        raise SystemExit("engine build failed")
    os.makedirs(os.path.dirname(engine_path), exist_ok=True)
    with open(engine_path, "wb") as fh:
        fh.write(blob)
    print("engine written: %s (%.1f MB)" % (engine_path, os.path.getsize(engine_path) / 1e6))
    return blob


class TrtRunner:
    """Fixed-shape TRT runner; buffers are torch tensors so torch owns the allocation."""

    def __init__(self, engine_path, shape, fp16=True):
        logger = trt.Logger(trt.Logger.WARNING)
        with open(engine_path, "rb") as fh:
            runtime = trt.Runtime(logger)
            self.engine = runtime.deserialize_cuda_engine(fh.read())
        if self.engine is None:
            raise SystemExit("cannot deserialize " + engine_path)
        self.context = self.engine.create_execution_context()
        self.shape = shape

        self.in_name = self.engine.get_tensor_name(0)
        self.out_name = self.engine.get_tensor_name(1)
        self.context.set_input_shape(self.in_name, shape)
        out_shape = tuple(self.context.get_tensor_shape(self.out_name))
        if any(d < 0 for d in out_shape):
            raise SystemExit("output shape still dynamic: %s" % (out_shape,))
        self.out_shape = out_shape
        # The IO dtype follows the ONNX declaration (float32), not the FP16 builder flag: FP16
        # only applies to the internal layers. Allocating fp16 buffers here is an instant
        # "illegal memory access" from execute_async_v3, so trust the engine.
        trt_dtype = self.engine.get_tensor_dtype(self.in_name)
        self.dtype = {trt.DataType.FLOAT: torch.float32, trt.DataType.HALF: torch.float16}[trt_dtype]
        self.src = torch.empty(shape, dtype=self.dtype, device="cuda")
        self.dst = torch.empty(out_shape, dtype=self.dtype, device="cuda")
        self.context.set_tensor_address(self.in_name, self.src.data_ptr())
        self.context.set_tensor_address(self.out_name, self.dst.data_ptr())
        self.stream = torch.cuda.current_stream().cuda_stream

    def run(self, feed):
        self.src.copy_(torch.from_numpy(feed).to(self.dtype))
        if not self.context.execute_async_v3(self.stream):
            raise SystemExit("execute_async_v3 failed")
        torch.cuda.synchronize()
        return self.dst
