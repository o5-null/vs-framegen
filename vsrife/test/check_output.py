"""Sanity check for rife_native: even output frames must be the source frames bit for bit,
odd frames must be plausible in-betweens (compare against the neighbours' average)."""
import sys
import numpy as np
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")
MODEL = r"E:\code\dlssg-vfi\vsrife\models\rife_v4.26.onnx"
SRC = r"E:\code\dlssg-vfi\测试数据\阿哈角色PV 1080p.mkv"

src = core.ffms2.Source(SRC).std.Trim(0, 7)
out = core.rife_native.Rife(clip=src, model=open(MODEL, "rb").read(), factor=2, sc=0.0)


def planes(f):
    return [np.asarray(f[p]) for p in range(3)]


def psnr(a, b):
    mse = np.mean([np.mean((x.astype(np.float64) - y) ** 2) for x, y in zip(a, b)])
    return float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


srcs = [planes(src.get_frame(i)) for i in range(8)]
ok = True
for n in range(out.num_frames):
    o = planes(out.get_frame(n))
    if n % 2 == 0:
        d = psnr(o, srcs[n // 2])
        if d != float("inf"):
            ok = False
        print(f"frame {n:2d} passthrough psnr={d}")
    else:
        a, b = srcs[n // 2], srcs[n // 2 + 1]
        mid = [((x.astype(np.float64) + y) / 2) for x, y in zip(a, b)]
        print(f"frame {n:2d} in-between psnr vs mean={psnr(o, mid):.2f}  vs A={psnr(o, a):.2f}")

print("PASSTHROUGH_EXACT" if ok else "PASSTHROUGH_MISMATCH")
sys.exit(0 if ok else 1)
