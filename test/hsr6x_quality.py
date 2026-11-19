r"""Quality check for vdlssg multi=N on the HSR PV, with the generated frames saved to disk.

The harness feeds every STEP-th real frame to the plugin and compares the synthesised slots against
real frames. With STEP == MULTI (the default) a pair spans MULTI frame intervals, so every slot sits
on a real frame -- the hardest case, far above what the plugin does in normal use, where a pair is
two adjacent frames and each slot is only 1/MULTI of a frame away from one.

With STEP=2 and MULTI=6 only slot 3 lands on a real frame: it is the midpoint of a two-frame span,
i.e. one whole frame away from each bracket. That is the worst-case slot of a real 6x run (the other
four sit closer to a bracket frame), so it makes a fair upper bound, and it is directly comparable
with multi=2 STEP=2 (slot 1) on the very same frame pairs.

Every saved frame is a lossless PNG so the numbers can be re-checked (or fed to VMAF) later.

Usage: python test\hsr6x_quality.py [mkv] [start] [groups] [multi]
Env:   VDLSSG_STEP  keyframe stride, default = multi
       VDLSSG_SCENE scene-cut threshold, default 1.0 (off; the clip has no cuts and the thinned
                    brackets are farther apart than the detector's 0.15 assumes)
Defaults: HSR PV, start 3510 (117.0 s, cards/character action), 16 groups, multi 6.
Writes 测试数据\results\hsr6x_m{multi}_s{step}\slot{K}\NNNN.png, gt{K}\NNNN.png + metrics.json
"""

import json
import os
import subprocess
import sys

import numpy as np
import vapoursynth as vs
from PIL import Image

ROOT = r"E:\code\dlssg-vfi"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "-" else (
    r"E:\code\dlssg-vfi\测试数据\《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")
START = int(sys.argv[2]) if len(sys.argv) > 2 else 3510
GROUPS = int(sys.argv[3]) if len(sys.argv) > 3 else 16
MULTI = int(sys.argv[4]) if len(sys.argv) > 4 else 6
# Default: brackets MULTI frames apart, so every slot has a real reference frame.
STEP = int(os.environ.get("VDLSSG_STEP", str(MULTI)))
SCENE = float(os.environ.get("VDLSSG_SCENE", "1.0"))
# VDLSSG_MV=1 feeds dense DIS motion vectors; without them the runtime gets a constant zero field
# and can only cross-fade, which is all a plain FrameGen call ever does.
USE_MV = os.environ.get("VDLSSG_MV", "0") not in ("0", "", "no")
# Slots whose position k/MULTI of the bracket span lands on a real frame, with that frame's offset.
CMP = [(k, k * STEP // MULTI) for k in range(1, MULTI) if (k * STEP) % MULTI == 0]
OUT = os.path.join(ROOT, "测试数据", "results",
                   "hsr6x_m%d_s%d%s" % (MULTI, STEP, "_mv" if USE_MV else ""))

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))


def planes(frame):
    return [np.asarray(frame[p], dtype=np.float32) for p in range(3)]


def mae(x, y):
    return float(np.mean([np.abs(a - b).mean() for a, b in zip(x, y)]))


def psnr(x, y):
    mse = float(np.mean([((a - b) ** 2).mean() for a, b in zip(x, y)]))
    return 99.0 if mse <= 0 else 10.0 * np.log10(1.0 / mse)


def to_rgb8(frame):
    rgb = [np.asarray(frame[p], dtype=np.float32) for p in range(3)]
    return (np.clip(np.stack(rgb, axis=2), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def save(path, frame):
    Image.fromarray(to_rgb8(frame)).save(path, compress_level=4)


def run_vmaf(out_dir, groups, fps):
    """libvmaf per slot, computed on yuv420p like any delivery encode would be."""
    # ASCII-only sequence root: ffmpeg's filter parser mangles the CJK in 测试数据.
    seq = os.path.join(ROOT, "work", "hsr6x_vmaf")
    for name in ["gt%d" % k for k, _ in CMP] + ["slot%d" % k for k, _ in CMP]:
        src = os.path.join(out_dir, name)
        dst = os.path.join(seq, name)
        os.makedirs(dst, exist_ok=True)
        for i in range(groups):
            Image.open(os.path.join(src, "%04d.png" % i)).save(os.path.join(dst, "%04d.png" % i))
    result = {}
    for k, _ in CMP:
        # Relative log name: a drive-letter colon inside the filter graph cannot be escaped.
        log = "vmaf_%s_slot%d.json" % (os.path.basename(out_dir), k)
        filt = ("[0:v]format=yuv420p[d];[1:v]format=yuv420p[r];"
                "[d][r]libvmaf=log_fmt=json:log_path=%s:n_threads=8"
                ":feature=name=psnr|name=float_ssim" % log)
        cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
               "-framerate", str(fps), "-i", os.path.join("slot%d" % k, "%04d.png"),
               "-framerate", str(fps), "-i", os.path.join("gt%d" % k, "%04d.png"),
               "-lavfi", filt, "-f", "null", "-"]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=seq)
        if proc.returncode != 0:
            raise SystemExit("ffmpeg failed:\n" + proc.stderr[-2000:])
        with open(os.path.join(seq, log), encoding="utf-8") as fh:
            data = json.load(fh)
        pooled = data["pooled_metrics"]
        result[k] = {
            "vmaf": round(float(pooled["vmaf"]["mean"]), 3),
            "psnr": round(float(pooled.get("psnr", pooled.get("psnr_y", {})).get("mean", 0.0)), 3),
            "ssim": round(float(pooled.get("float_ssim", pooled.get("ssim", {})).get("mean", 0.0)), 5),
        }
    return result


def main():
    if not CMP:
        raise SystemExit("VDLSSG_STEP=%d lands no slot on a real frame; use a step of 2 or more"
                         % STEP)
    ref_clip = core.resize.Bicubic(core.ffms2.Source(SRC), format=vs.RGBS, matrix_in_s="709")
    fps = ref_clip.fps_num / ref_clip.fps_den

    # Keyframes every STEP-th frame: GROUPS+1 of them, so no evaluated pair hits the last-frame hold.
    first_key = START // STEP
    thin = core.std.SelectEvery(ref_clip, STEP, 0)
    thin = core.std.Trim(thin, first_key, first_key + GROUPS)
    kwargs = {}
    if USE_MV:
        import hsr_flow

        tag = "s%d_%d_%d_%d_k%d" % (STEP, START, GROUPS, thin.width, hsr_flow.K_FLOW_SCALE)
        print("  computing DIS motion vectors (%d frames)" % thin.num_frames)
        paths_u, paths_v = hsr_flow.backward_flows(thin, tag)
        kwargs["mv"] = hsr_flow.mv_clip(core, thin, paths_u, paths_v)
    out = core.vdlssg.FrameGen(thin, multi=MULTI, scene_threshold=SCENE,
                               proxy_dir=os.path.join(ROOT, "dist", "engine45"), log=0, **kwargs)

    for k, _ in CMP:
        os.makedirs(os.path.join(OUT, "slot%d" % k), exist_ok=True)
        os.makedirs(os.path.join(OUT, "gt%d" % k), exist_ok=True)

    slot_err = {k: [] for k, _ in CMP}
    slot_psnr = {k: [] for k, _ in CMP}
    slot_blend = {k: [] for k, _ in CMP}
    distinct = []
    for j in range(GROUPS):
        base = START + j * STEP
        # Only the two bracket frames are held; everything else is fetched per slot, because a
        # whole 4K RGBS window would be several GB of float32.
        a = planes(ref_clip.get_frame(base))
        b = planes(ref_clip.get_frame(base + STEP))
        gots = {}
        for k, off in CMP:
            frame = out.get_frame(j * MULTI + k)
            got = gots[k] = planes(frame)
            gt = planes(ref_clip.get_frame(base + off))
            save(os.path.join(OUT, "slot%d" % k, "%04d.png" % j), frame)
            save(os.path.join(OUT, "gt%d" % k, "%04d.png" % j), ref_clip.get_frame(base + off))
            slot_err[k].append(mae(got, gt))
            slot_psnr[k].append(psnr(got, gt))
            blended = [(1.0 - k / MULTI) * x + (k / MULTI) * y for x, y in zip(a, b)]
            slot_blend[k].append(mae(blended, gt))
        keys = [k for k, _ in CMP]
        for i, s in enumerate(keys):
            for t in keys[i + 1:]:
                distinct.append(mae(gots[s], gots[t]))

    metrics = {"source": os.path.basename(SRC), "start": START, "groups": GROUPS,
               "multi": MULTI, "step": STEP, "fps": fps, "slots": {}}
    total_err, total_blend, total_psnr = [], [], []
    for k, off in CMP:
        e = float(np.mean(slot_err[k]))
        bl = float(np.mean(slot_blend[k]))
        p = float(np.mean(slot_psnr[k]))
        metrics["slots"][k] = {"alpha": round(k / MULTI, 3), "gt_offset": off, "mae": round(e, 6),
                               "psnr": round(p, 3),
                               "blend_mae": round(bl, 6),
                               "gain_vs_blend": round(bl / e, 3) if e > 0 else None}
        total_err.append(e)
        total_blend.append(bl)
        total_psnr.append(p)
    metrics["all"] = {"mae": round(float(np.mean(total_err)), 6),
                      "psnr": round(float(np.mean(total_psnr)), 3),
                      "blend_mae": round(float(np.mean(total_blend)), 6)}
    if distinct:
        metrics["distinctness"] = {"mean": round(float(np.mean(distinct)), 6),
                                   "min": round(float(np.min(distinct)), 6)}

    with open(os.path.join(OUT, "metrics.json"), "w", encoding="utf-8") as fh:
        json.dump(metrics, fh, ensure_ascii=False, indent=2)

    try:
        metrics["vmaf_slots"] = run_vmaf(OUT, GROUPS, fps)
    except SystemExit as exc:
        print("  VMAF skipped: %s" % str(exc)[:300])
    else:
        with open(os.path.join(OUT, "metrics.json"), "w", encoding="utf-8") as fh:
            json.dump(metrics, fh, ensure_ascii=False, indent=2)

    print("multi=%d step=%d start=%d groups=%d (%.2f s @ %.2f fps), scene_threshold=%.2f"
          % (MULTI, STEP, START, GROUPS, GROUPS * STEP / fps, fps, SCENE))
    for k, off in CMP:
        m = metrics["slots"][k]
        v = metrics.get("vmaf_slots", {}).get(k, {})
        print("  slot %d (alpha=%.2f vs frame +%d)  MAE %.6f  PSNR %.2f dB  blend %.6f (%.2fx)"
              "  VMAF %s"
              % (k, m["alpha"], off, m["mae"], m["psnr"], m["blend_mae"],
                 m["gain_vs_blend"] or 0.0, v.get("vmaf", "-")))
    print("  all slots  MAE %.6f  PSNR %.2f dB  distinctness %s"
          % (metrics["all"]["mae"], metrics["all"]["psnr"],
             ("mean %.6f min %.6f" % (metrics["distinctness"]["mean"],
                                      metrics["distinctness"]["min"])) if distinct else "-"))
    print("  saved to %s" % OUT)


if __name__ == "__main__":
    main()
