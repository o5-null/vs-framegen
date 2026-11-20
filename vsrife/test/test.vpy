# rife_native smoke test: 2x interpolation of a single scene, no scene rounding.
#
#   "E:\code\VFI\.venv\Scripts\vspipe.exe" -p E:\code\dlssg-vfi\vsrife\test\test.vpy -
#
# The venv carries ffms2; the plugin comes from vsrife\bin (build it first).
import os

import vapoursynth as vs

core = vs.core

PLUGIN = r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll"
SRC = r"E:\code\dlssg-vfi\测试数据\阿哈角色PV 1080p.mkv"
# fp16 models from the animejanai-inference release "models-rife-fp16-2"
# (rife-fp16-2.7z); the dynamo-exported vs-mlrt models need
# tools/normalize_rife_onnx.py first.
MODEL = r"C:\Users\benben\AppData\Local\Temp\opencode\rifemodels\x\rife_v4.26.onnx"

core.std.LoadPlugin(PLUGIN)

src = core.ffms2.Source(SRC)
src = src.std.Trim(0, 47)  # 48 source frames -> 95 output frames

with open(MODEL, "rb") as f:
    model = f.read()

out = core.rife_native.Rife(clip=src, model=model, factor=2, sc=0.0)
out.set_output()
