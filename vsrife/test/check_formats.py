"""Format and factor matrix: passthrough frames must be bit exact, other phases must be finite.

Covers 8/10-bit 4:2:0, 4:4:4 and factors 2/3/5.
"""
import sys
import numpy as np
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")
MODEL = open(r"E:\code\dlssg-vfi\vsrife\models\rife_v4.26.onnx", "rb").read()
SRC = r"E:\code\dlssg-vfi\测试数据\阿哈角色PV 1080p.mkv"

base = core.ffms2.Source(SRC).std.Trim(0, 7)


def planes(f):
    # Sizes differ per plane in 4:2:0, so compare plane by plane.
    return [np.asarray(f[p]).copy() for p in range(3)]


fail = 0
for fmt, factor in [(vs.YUV420P8, 2), (vs.YUV420P10, 2), (vs.YUV444P8, 2),
                    (vs.YUV420P8, 3), (vs.YUV420P8, 5)]:
    clip = base.resize.Point(format=fmt)
    out = core.rife_native.Rife(clip=clip, model=MODEL, factor=factor, sc=0.0)
    srcs = [planes(clip.get_frame(i)) for i in range(4)]
    exact = True
    for n in range(factor * 4 - 3):
        o = planes(out.get_frame(n))
        if n % factor == 0:
            a = srcs[n // factor]
            if any(not np.array_equal(x, y) for x, y in zip(o, a)):
                exact = False
                print(f"  MISMATCH {str(fmt)} factor {factor} frame {n}")
        elif not all(np.isfinite(x).all() for x in o):
            fail += 1
    print(f"{str(fmt):10s} factor {factor}: frames={out.num_frames} passthrough={'exact' if exact else 'BROKEN'}")
    if not exact:
        fail += 1

print("FORMAT_MATRIX_OK" if fail == 0 else f"FORMAT_MATRIX_FAILED ({fail})")
sys.exit(1 if fail else 0)
