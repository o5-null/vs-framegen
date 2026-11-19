r"""Same pass as vd_gen_dlssg_mv.py, but the motion clip is the 2x2 / 1-32px variant.

The mv clip comes from results\dlssg_mv_half\mv (see vd_make_mv_half.py): half the frame size, one
vector per 2x2 block, quantized to 1/32 pixel - the format Encore's own estimator emits. Output:
results\dlssg_mvh\frames\<gt>.png plus results\dlssg_mvh\timing.json.
"""

import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import vapoursynth as vs

METHOD = "dlssg_mvh"
MV_DIR = os.path.join(vd.RESULTS, "dlssg_mv_half", "mv")
# With a single input clip, Expr's `x` is the plane being evaluated, so the same expression decodes
# u (plane 0) and v (plane 1); plane 2 is unused by the plugin and only has to exist.
MV_DECODE = ["x 65535 * 32768 - 128 /", "x 65535 * 32768 - 128 /", "0"]


def frame_rgb(frame):
    """RGBS is planar, so each plane is a full HxW float32 image; stack them into HxWx3."""
    return np.stack([np.asarray(frame[i]) for i in range(3)], axis=-1)


def main():
    core = vs.core
    vd.load_plugin(core, vd.PLUGIN)

    triples = vd.triplets()
    a_only = [a for (a, _, _) in triples] + [triples[-1][2]]  # 20 inputs, last B closes the run
    mv_paths = [os.path.join(MV_DIR, "%04d.png" % (i + 1)) for i in range(len(a_only))]
    missing = [p for p in mv_paths if not os.path.exists(p)]
    if missing:
        raise SystemExit("missing motion frames, run vd_make_mv_half.py first: " + missing[0])
    print("inputs:", len(a_only), os.path.basename(a_only[0]), "->", os.path.basename(a_only[-1]))

    clip = core.imwri.Read(a_only, firstnum=1, float_output=1)
    mv = core.imwri.Read(mv_paths, firstnum=1, float_output=1)
    mv = core.std.Expr([mv], MV_DECODE)
    print("mv:", mv.num_frames, "frames", mv.format.name, mv.width, "x", mv.height)

    gen = core.vdlssg.FrameGen(clip, multi=2, mv=mv, proxy_dir=vd.PROXY, log=0,
                               scene_threshold=0.15)
    print("output frames:", gen.num_frames, "format:", gen.format.name)

    timing = []
    for k, (_, gt, _) in enumerate(triples):
        idx = 2 * k + 1
        t0 = time.perf_counter()
        frame = gen.get_frame(idx)
        elapsed = (time.perf_counter() - t0) * 1000.0
        rgb = np.clip(frame_rgb(frame), 0.0, 1.0)
        vd.write_rgb16(vd.frame_path(METHOD, gt), rgb)
        props = {key: frame.props.get(key) for key in
                 ("_DLSSGGenerated", "_DLSSGFallback", "_DLSSGSceneCut", "_DLSSGMultiFrameIndex")}
        timing.append({"out": idx, "gt": vd.gt_name(gt), "wall_ms": round(elapsed, 2), **props})
        print("out %3d -> %s  %.1f ms  fallback=%s" %
              (idx, vd.gt_name(gt), elapsed, props["_DLSSGFallback"]))

    out_dir = os.path.join(vd.RESULTS, METHOD)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "timing.json"), "w", encoding="utf-8") as fh:
        json.dump(timing, fh, indent=1)
    walls = [t["wall_ms"] for t in timing]
    print("wall ms: min %.1f median %.1f max %.1f" %
          (min(walls), sorted(walls)[len(walls) // 2], max(walls)))


if __name__ == "__main__":
    main()
