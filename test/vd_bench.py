r"""Throughput comparison, GPU compute only.

The numbers in timing.json are end-to-end wall time per generated frame and include decoding two
1080p PNGs (imwri) plus writing one 16-bit PNG, which dwarfs the actual synthesis. This script
separates the two: it measures the same get_frame calls with and without the filter in the chain,
and reads the plugin's own GPU timestamps out of its log lines.
"""

import json
import os
import re
import statistics
import sys
import time

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import vapoursynth as vs

STEP = re.compile(r"\[vdlssg\] frame (\d+) .*?: ([\d.]+) ms")


def dlssg_bench(core, inputs):
    clip = core.imwri.Read(inputs, firstnum=1, float_output=1)
    # log=1 makes the plugin print its own GPU time per generated frame to stderr.
    gen = core.vdlssg.FrameGen(clip, multi=2, proxy_dir=vd.PROXY, log=1, scene_threshold=0.15)

    # Baseline: pull the same source frames through imwri alone, so PNG decode can be subtracted.
    base = []
    for i in range(len(inputs)):
        t0 = time.perf_counter()
        clip.get_frame(i)
        base.append((time.perf_counter() - t0) * 1000.0)

    walls = []
    for idx in range(1, gen.num_frames, 2):
        t0 = time.perf_counter()
        frame = gen.get_frame(idx)
        walls.append((time.perf_counter() - t0) * 1000.0)
        assert frame.props["_DLSSGGenerated"] == 1
    return walls, base, gen.num_frames


def rife_bench():
    import torch
    from trt_rife422 import TrtRunner

    engine = r"E:\code\dlssg-vfi\work\engines\rife422_1080p_fp16.engine"
    if not os.path.exists(engine):
        return [], "engine missing"
    runner = TrtRunner(engine, (1, 7, 1088, 1920))
    a = np.random.rand(1088, 1920, 3).astype(np.float32)
    b = np.random.rand(1088, 1920, 3).astype(np.float32)
    feed = np.zeros((1, 7, 1088, 1920), np.float32)
    feed[0, 0:3] = a.transpose(2, 0, 1)
    feed[0, 3:6] = b.transpose(2, 0, 1)
    feed[0, 6] = 0.5
    times = []
    for _ in range(21):
        t0 = time.perf_counter()
        runner.run(feed)
        torch.cuda.synchronize()
        times.append((time.perf_counter() - t0) * 1000.0)
    return times[1:], ""  # first call includes warmup


def summarize(name, walls, gpu):
    if not walls:
        print("%-8s no data" % name)
        return {}
    rec = {"wall_ms": round(statistics.median(walls), 2),
           "wall_min_ms": round(min(walls), 2),
           "gpu_ms": round(statistics.median(gpu), 2) if gpu else None,
           "gpu_min_ms": round(min(gpu), 2) if gpu else None}
    fps = 1000.0 / rec["gpu_ms"] if rec["gpu_ms"] else None
    print("%-8s wall median %7.2f ms | gpu median %6.2f ms -> %.1f generated fps" %
          (name, rec["wall_ms"], rec["gpu_ms"] or 0.0, fps or 0.0))
    rec["fps_gpu"] = round(fps, 1) if fps else None
    return rec


def main():
    core = vs.core
    vd.load_plugin(core, vd.PLUGIN)
    triples = vd.triplets()
    inputs = [a for (a, _, _) in triples] + [triples[-1][2]]

    walls, base, _ = dlssg_bench(core, inputs)
    # The plugin prints one line per generated frame; those are its own GPU timestamps (two passes).
    gpu = []
    out_path = os.path.join(r"E:\code\dlssg-vfi\test", "vd_bench.out")
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8", errors="replace") as fh:
            gpu = [float(m.group(2)) for m in STEP.finditer(fh.read())]

    res = {"dlssg": summarize("dlssg", walls, gpu)}
    res["dlssg"]["source_decode_ms"] = round(statistics.median(base), 2)

    rife, note = rife_bench()
    res["rife422"] = summarize("rife422", rife, rife)
    if note:
        res["rife422"] = {"error": note}

    with open(os.path.join(vd.RESULTS, "timing_bench.json"), "w", encoding="utf-8") as fh:
        json.dump(res, fh, indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
