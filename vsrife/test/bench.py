"""Timing and format matrix for rife_native (no decode: synthetic clips)."""
import time
import numpy as np
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")
MODEL = open(r"E:\code\dlssg-vfi\vsrife\models\rife_v4.26.onnx", "rb").read()


def timed(clip, factor, frames, warm=6):
    out = core.rife_native.Rife(clip=clip, model=MODEL, factor=factor, sc=0.0)
    for n in range(min(warm, out.num_frames)):
        out.get_frame(n)
    t0 = time.perf_counter()
    for n in range(frames):
        out.get_frame(n)
    dt = time.perf_counter() - t0
    return dt * 1000.0 / frames


for fmt, w, h in [(vs.YUV420P8, 1920, 1080), (vs.YUV420P10, 1920, 1080), (vs.YUV444P8, 1920, 1080),
                  (vs.YUV420P8, 3840, 2160)]:
    clip = core.std.BlankClip(None, w, h, fmt, 40)
    for factor in (2,):
        ms = timed(clip, factor, 24)
        permb = ms / (w * h / 1e6)
        per_interp = ms * factor / (factor - 1) if factor > 1 else float("nan")
        print(f"{str(fmt):16s} {w}x{h} factor {factor}: {ms:7.2f} ms/output frame "
              f"({per_interp:7.2f} ms per interpolated frame, {permb:6.2f} ms/output-Mpx)")
