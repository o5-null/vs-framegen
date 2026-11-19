r"""What a real 6x run emits: the full-rate clip in, six frames per source frame out.

This is the configuration a user actually runs (adjacent source frames, multi=6), so there is no
ground truth to score against -- every output frame is new. What it does show is the behaviour that
matters in practice: how many pairs get held as scene cuts, and how the 1/6 steps look between two
real frames.

Usage: python test\hsr6x_sample.py [start] [source_frames] [scene_threshold]
Writes 测试数据\results\hsr6x_sample:
  pair0\slot0..slot5.png   the first source pair's six frames (source frame + five generated)
  frames\NNNN.png          every output frame
  hsr6x_180fps.mp4         real time (180 fps)
  hsr6x_slowmo.mp4         the same frames at 30 fps, i.e. played six times slower
"""

import json
import os
import subprocess
import sys

import numpy as np
import vapoursynth as vs
from PIL import Image

ROOT = r"E:\code\dlssg-vfi"
SRC = os.path.join(ROOT, "测试数据",
                   "《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")
START = int(sys.argv[1]) if len(sys.argv) > 1 else 3510
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 12
SCENE = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
MULTI = 6
FPS = 30.0
OUT = os.path.join(ROOT, "测试数据", "results", "hsr6x_sample")


def to_rgb8(frame):
    rgb = [np.asarray(frame[p], dtype=np.float32) for p in range(3)]
    return (np.clip(np.stack(rgb, axis=2), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def main():
    core = vs.core
    core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))
    clip = core.resize.Bicubic(core.ffms2.Source(SRC), format=vs.RGBS, matrix_in_s="709")
    clip = core.std.Trim(clip, START, START + COUNT - 1)
    gen = core.vdlssg.FrameGen(clip, multi=MULTI, scene_threshold=SCENE,
                               proxy_dir=os.path.join(ROOT, ""vs-dlssg", "bin", "dlss45"), log=0)

    frames_dir = os.path.join(OUT, "frames")
    pair_dir = os.path.join(OUT, "pair0")
    os.makedirs(frames_dir, exist_ok=True)
    os.makedirs(pair_dir, exist_ok=True)

    holds = 0
    for i in range(gen.num_frames):
        frame = gen.get_frame(i)
        img = Image.fromarray(to_rgb8(frame))
        img.save(os.path.join(frames_dir, "%04d.png" % i), compress_level=4)
        if i < MULTI:
            img.save(os.path.join(pair_dir, "slot%d.png" % i), compress_level=4)
        if i % MULTI == 1 and frame.props.get("_DLSSGFallback") == 1:
            holds += 1
        print("  frame %d/%d" % (i + 1, gen.num_frames), end="\r", flush=True)
    print()

    for name, rate in (("hsr6x_180fps.mp4", FPS * MULTI), ("hsr6x_slowmo.mp4", FPS)):
        cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
               "-framerate", str(rate), "-i", "frames/%04d.png",
               "-c:v", "libx264", "-crf", "14", "-preset", "medium",
               "-pix_fmt", "yuv420p", name]
        subprocess.run(cmd, cwd=OUT, check=True)

    summary = {"start": START, "source_frames": COUNT, "multi": MULTI,
               "scene_threshold": SCENE, "output_frames": gen.num_frames,
               "held_pairs": holds}
    with open(os.path.join(OUT, "summary.json"), "w", encoding="utf-8") as fh:
        json.dump(summary, fh, ensure_ascii=False, indent=2)
    print("start=%d %d source frames -> %d output frames, %d pairs held (threshold %.2f)"
          % (START, COUNT, gen.num_frames, holds, SCENE))
    print("written to %s" % OUT)


if __name__ == "__main__":
    main()
