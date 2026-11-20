"""Time one model on a synthetic clip: python _time_one.py <model> <w> <h> [fmt] [factor]

fmt: p8 (YUV420P8, default) | p10 | p444
"""
import sys
import time

import vapoursynth as vs

DLL = r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll"
FMTS = {"p8": vs.YUV420P8, "p10": vs.YUV420P10, "p444": vs.YUV444P8}


def main(model_path, w, h, fmt_name, factor):
    core = vs.core
    core.std.LoadPlugin(DLL)
    model = open(model_path, "rb").read()
    clip = core.std.BlankClip(None, int(w), int(h), FMTS[fmt_name], 40)
    out = core.rife_native.Rife(clip=clip, model=model, factor=int(factor), sc=0.0)
    for n in range(6):
        out.get_frame(n)
    t0 = time.perf_counter()
    for n in range(24):
        out.get_frame(n)
    dt = (time.perf_counter() - t0) * 1000.0 / 24.0
    print("%s %dx%d %s f%d: %.2f ms/output (%.2f ms/interp)"
          % (model_path.rsplit("\\", 1)[-1], int(w), int(h), fmt_name, int(factor),
             dt, dt * int(factor) / (int(factor) - 1)))


if __name__ == "__main__":
    a = sys.argv[1:]
    main(a[0], a[1], a[2], a[3] if len(a) > 3 else "p8", a[4] if len(a) > 4 else 2)
