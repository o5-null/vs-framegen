"""Scene change path: a hard cut inside the clip must not be interpolated across.

The filter copies the anchor frame when the luma difference between the two
source frames exceeds `sc`, so frame 1 (t=0.5 across the cut) must equal frame 0.
"""
import numpy as np
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")
MODEL = open(r"E:\code\dlssg-vfi\vsrife\models\rife_v4.26.onnx", "rb").read()

a = core.std.BlankClip(None, 640, 360, vs.YUV420P8, 4, color=[16, 128, 128])
b = core.std.BlankClip(None, 640, 360, vs.YUV420P8, 4, color=[235, 128, 128])
clip = core.std.Splice([a, b])  # hard cut between frame 3 and 4

for sc in (0.0, 0.05):
    out = core.rife_native.Rife(clip=clip, model=MODEL, factor=2, sc=sc)
    # frame 7 is the in-between of source frames 3 and 4, i.e. across the cut
    f = np.asarray(out.get_frame(7)[0])
    before = np.asarray(out.get_frame(6)[0])
    same_as_anchor = np.array_equal(f, before)
    print(f"sc={sc}: frame7 mean luma={f.mean():.1f} equals previous frame={same_as_anchor}")
