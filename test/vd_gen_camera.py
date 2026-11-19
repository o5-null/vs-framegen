r"""Camera-description experiment: same pass as vd_gen_dlssg_mv.py, but exposes the plugin's
`camera` parameter so modes 0-3 can be compared on identical input.

  python vd_gen_camera.py 0    identity matrices + orthoProjection (baseline)
  python vd_gen_camera.py 1    real perspective projection, no inter-frame camera motion
  python vd_gen_camera.py 2    perspective + clipToPrevClip fitted from this pair's field
  python vd_gen_camera.py 3    like 2, but cameraMotionIncluded = false

Outputs: results\dlssg_cam<N>\frames\<gt>.png and results\dlssg_cam<N>\timing.json.
"""

import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import vapoursynth as vs

MV_DIR = os.path.join(vd.RESULTS, "dlssg_mv", "mv")
MV_DECODE = ["x 65535 * 32768 - 128 /", "x 65535 * 32768 - 128 /", "0"]


def frame_rgb(frame):
    return np.stack([np.asarray(frame[i]) for i in range(3)], axis=-1)


def main():
    camera = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    method = "dlssg_cam%d" % camera

    core = vs.core
    vd.load_plugin(core, vd.PLUGIN)

    triples = vd.triplets()
    a_only = [a for (a, _, _) in triples] + [triples[-1][2]]
    mv_paths = [os.path.join(MV_DIR, "%04d.png" % (i + 1)) for i in range(len(a_only))]
    missing = [p for p in mv_paths if not os.path.exists(p)]
    if missing:
        raise SystemExit("missing motion frames, run vd_make_mv.py first: " + missing[0])
    print("camera mode:", camera, "inputs:", len(a_only))

    clip = core.imwri.Read(a_only, firstnum=1, float_output=1)
    mv = core.imwri.Read(mv_paths, firstnum=1, float_output=1)
    mv = core.std.Expr([mv], MV_DECODE)

    gen = core.vdlssg.FrameGen(clip, multi=2, mv=mv, proxy_dir=vd.PROXY, log=0,
                               scene_threshold=0.15, camera=camera)
    print("output frames:", gen.num_frames, "format:", gen.format.name)

    timing = []
    for k, (_, gt, _) in enumerate(triples):
        idx = 2 * k + 1
        t0 = time.perf_counter()
        frame = gen.get_frame(idx)
        elapsed = (time.perf_counter() - t0) * 1000.0
        rgb = np.clip(frame_rgb(frame), 0.0, 1.0)
        vd.write_rgb16(vd.frame_path(method, gt), rgb)
        props = {key: frame.props.get(key) for key in
                 ("_DLSSGGenerated", "_DLSSGFallback", "_DLSSGSceneCut", "_DLSSGMultiFrameIndex")}
        timing.append({"out": idx, "gt": vd.gt_name(gt), "wall_ms": round(elapsed, 2), **props})
        print("out %3d -> %s  %.1f ms  fallback=%s" %
              (idx, vd.gt_name(gt), elapsed, props["_DLSSGFallback"]))

    os.makedirs(os.path.join(vd.RESULTS, method), exist_ok=True)
    with open(os.path.join(vd.RESULTS, method, "timing.json"), "w", encoding="utf-8") as fh:
        json.dump(timing, fh, indent=1)
    walls = [t["wall_ms"] for t in timing]
    print("wall ms: min %.1f median %.1f max %.1f" %
          (min(walls), sorted(walls)[len(walls) // 2], max(walls)))


if __name__ == "__main__":
    main()
