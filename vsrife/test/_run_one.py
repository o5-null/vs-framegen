"""Run one RIFE model through rife_native: timing + interpolation quality.

Usage: python _run_one.py <model.onnx>

Prints one JSON line. Quality is measured against a real 60fps clip:
every other frame is dropped to make a 30fps clip, the plugin interpolates
it back to 60fps, and the interpolated frame is compared with the real one.
"""
import json
import sys
import time

import numpy as np
import vapoursynth as vs

CLIP = r"E:\code\dlssg-vfi\测试数据\test_60fps.mp4"
DLL = r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll"
PAIRS = 30          # interpolated frames compared
TRIM = PAIRS * 2 + 2


def psnr_y(a, b):
    # frames are YUV420P10: compare the luma plane on the 10-bit scale
    pa = np.asarray(a[0], dtype=np.float32)
    pb = np.asarray(b[0], dtype=np.float32)
    mse = float(np.mean((pa - pb) ** 2))
    if mse <= 0.0:
        return 99.0
    return 10.0 * np.log10(1023.0 ** 2 / mse)


def main(path):
    res = {"model": path.rsplit("\\", 1)[-1]}
    core = vs.core
    core.std.LoadPlugin(DLL)
    model = open(path, "rb").read()

    # -- timing on a synthetic 1080p 8-bit clip (no decode, no disk) --
    blank = core.std.BlankClip(None, 1920, 1080, vs.YUV420P8, 40)
    out = core.rife_native.Rife(clip=blank, model=model, factor=2, sc=0.0)
    for n in range(6):
        out.get_frame(n)
    t0 = time.perf_counter()
    for n in range(24):
        out.get_frame(n)
    dt = time.perf_counter() - t0
    res["ms_out"] = round(dt * 1000.0 / 24.0, 2)
    del out

    # -- quality against a real 60fps source (10-bit) --
    src = core.ffms2.Source(CLIP).std.Trim(0, TRIM)
    half = core.std.SelectEvery(src, 2, [0])
    interp = core.rife_native.Rife(clip=half, model=model, factor=2, sc=0.0)
    scores = []
    for k in range(PAIRS):
        fidx = 2 * k + 1
        if fidx >= interp.num_frames or fidx >= src.num_frames:
            break
        scores.append(psnr_y(interp.get_frame(fidx), src.get_frame(fidx)))
    res["psnr"] = round(float(np.mean(scores)), 2) if scores else None
    res["psnr_min"] = round(float(np.min(scores)), 2) if scores else None
    res["ok"] = True
    return res


if __name__ == "__main__":
    try:
        print(json.dumps(main(sys.argv[1])))
    except Exception as e:  # noqa: BLE001 - report instead of crashing the matrix
        print(json.dumps({"model": sys.argv[1].rsplit("\\", 1)[-1], "ok": False,
                          "err": str(e).strip().splitlines()[0][:200]}))
