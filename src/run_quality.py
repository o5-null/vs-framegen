"""Quality pass, 1080p: every engine generates the 19 missing frames of the source clip.

The 40 source frames are consecutive, so frame 2, 4, ... are the true frames between
their neighbours. Feeding the 20 odd frames to a 2x interpolator therefore returns
19 tweens that each have real ground truth (the same construction as
test/vd_common.triplets). Output goes to 测试数据\\engines\\quality\\frames\\<engine>\\<gt>.png
so the metrics pass is a cheap re-run.

Usage: python run_quality.py [--force] [engine ...]
"""

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as C
import smv_run
from rife_trt import RifeRunner

W, H = 1920, 1080


def save_png(path, rgb):
    import cv2

    os.makedirs(os.path.dirname(path), exist_ok=True)
    ok, buf = cv2.imencode(".png", rgb[:, :, ::-1])
    if not ok:
        raise RuntimeError("cannot encode " + path)
    buf.tofile(path)


def done(engine):
    d = C.quality_dir(engine)
    return all(os.path.exists(os.path.join(d, g)) for g in C.gt_names())


def run_smv(name, flags):
    frames, _ = C.odd_frames(W, H)
    raw = C.write_raw(frames, os.path.join(C.TMP, "odd_1080p.raw"))
    cap = os.path.join(C.TMP, "smv_%s_1080p.raw" % name)
    info = smv_run.run(flags, raw, W, H, len(frames), out_file=cap)
    if info["exit"] != 0:
        raise SystemExit("%s failed:\n%s" % (name, info["log"][-1500:]))
    out, n = smv_run.read_frames(cap, W, H, len(frames))
    os.remove(cap)
    if n != 2 * len(frames) - 1:
        raise SystemExit("%s produced %d frames, expected %d" % (name, n, 2 * len(frames) - 1))
    # f0, t01, f1, t12, ... -> every odd output frame is a tween
    tweens = out[1::2]
    for gt, tw in zip(C.gt_names(), tweens):
        save_png(os.path.join(C.quality_dir(name), gt), tw)
    print("%-14s %d tweens  (%.1f ms/pair)" % (name, len(tweens), info["ms_per_pair"]), flush=True)


def run_rife(name, stem, mult):
    frames, _ = C.odd_frames(W, H)
    run = RifeRunner(name, stem, mult, W, H)
    try:
        for gt, i in zip(C.gt_names(), range(len(frames) - 1)):
            rgb, _, _ = run.tween(frames[i], frames[i + 1])
            save_png(os.path.join(C.quality_dir(name), gt), np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8))
    finally:
        run.close()
    print("%-14s %d tweens (TensorRT)" % (name, 19), flush=True)


def main():
    argv = sys.argv[1:]
    force = "--force" in argv
    only = [a for a in argv if not a.startswith("--")]
    for name, flags in C.SMV_ENGINES:
        if only and name not in only:
            continue
        if done(name) and not force:
            print("%-14s cached" % name, flush=True)
            continue
        run_smv(name, flags)
    for name, stem, mult in C.RIFE_MODELS:
        if only and name not in only:
            continue
        if done(name) and not force:
            print("%-14s cached" % name, flush=True)
            continue
        run_rife(name, stem, mult)
    print("done ->", os.path.join(C.DATA, "quality", "frames"))


if __name__ == "__main__":
    main()
