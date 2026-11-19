"""DLSS-G interpolation pass over 测试数据\1080p.

Feeds only the odd frames (0001, 0003, ... 0039) to core.vdlssg.FrameGen(multi=2), so the frame it
generates at output index 2k+1 sits between input k and k+1 - exactly where 0002, 0004, ... live.
Output: results\dlssg\frames\<gt>.png (16-bit) plus results\dlssg\timing.json.
"""

import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import vapoursynth as vs


def frame_rgb(frame):
    """RGBS is planar, so each plane is a full HxW float32 image; stack them into HxWx3."""
    return np.stack([np.asarray(frame[i]) for i in range(3)], axis=-1)


def main():
    core = vs.core
    vd.load_plugin(core, vd.PLUGIN)

    triples = vd.triplets()
    a_only = [a for (a, _, _) in triples] + [triples[-1][2]]  # 20 inputs, last B closes the run
    print("inputs:", len(a_only), os.path.basename(a_only[0]), "->", os.path.basename(a_only[-1]))

    clip = core.imwri.Read(a_only, firstnum=1, float_output=1)
    gen = core.vdlssg.FrameGen(clip, multi=2, proxy_dir=vd.PROXY, log=0, scene_threshold=0.15)
    print("output frames:", gen.num_frames, "format:", gen.format.name)

    timing = []
    for k, (_, gt, _) in enumerate(triples):
        idx = 2 * k + 1
        t0 = time.perf_counter()
        frame = gen.get_frame(idx)
        elapsed = (time.perf_counter() - t0) * 1000.0
        rgb = np.clip(frame_rgb(frame), 0.0, 1.0)
        vd.write_rgb16(vd.frame_path("dlssg", gt), rgb)
        props = {key: frame.props.get(key) for key in
                 ("_DLSSGGenerated", "_DLSSGFallback", "_DLSSGSceneCut", "_DLSSGMultiFrameIndex")}
        timing.append({"out": idx, "gt": vd.gt_name(gt), "wall_ms": round(elapsed, 2), **props})
        print("out %3d -> %s  %.1f ms  fallback=%s" %
              (idx, vd.gt_name(gt), elapsed, props["_DLSSGFallback"]))

    os.makedirs(os.path.join(vd.RESULTS, "dlssg"), exist_ok=True)
    with open(os.path.join(vd.RESULTS, "dlssg", "timing.json"), "w", encoding="utf-8") as fh:
        json.dump(timing, fh, indent=1)
    walls = [t["wall_ms"] for t in timing]
    print("wall ms: min %.1f median %.1f max %.1f" %
          (min(walls), sorted(walls)[len(walls) // 2], max(walls)))


if __name__ == "__main__":
    main()
