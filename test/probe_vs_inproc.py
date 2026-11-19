"""Probe how to drive the plugin in-process: imwri source -> RGBS -> FrameGen, then read frames."""

import sys

import numpy as np
import vapoursynth as vs

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

core = vs.core
vd.load_plugin(core, vd.PLUGIN)
print("plugins:", [p.namespace for p in core.plugins()])

a_only = vd.png_names()[0:40:2]  # 0001, 0003, ... 0039
print("a_only", len(a_only), a_only[0], a_only[-1])

clip = core.imwri.Read(a_only, firstnum=1, float_output=1)
print("imwri out:", clip.format, clip.width, clip.height, clip.num_frames)

rgb = clip
frame = rgb.get_frame(0)
plane = np.asarray(frame[0])
print("plane0 shape", plane.shape, plane.dtype)
print("range %.4f..%.4f" % (plane.min(), plane.max()))

gen = core.vdlssg.FrameGen(rgb, multi=2, proxy_dir=vd.PROXY, log=1)
print("framegen:", gen.format, gen.width, gen.height, gen.num_frames)
g = gen.get_frame(1)
arr = np.asarray(g[0])
print("generated frame 1 plane shape", arr.shape,
      "range %.4f..%.4f" % (arr.min(), arr.max()),
      "props:", {k: g.props[k] for k in ("_DLSSGGenerated", "_DLSSGFallback", "_DLSSGSceneCut")})

# Sanity: does generated frame 1 resemble GT 0002 more than the neighbours?
gt = vd.imread(vd.triplets()[0][1])
gtf = (gt[:, :, ::-1].astype(np.float32) / 255.0)
gen_rgb = arr.reshape(arr.shape[0], -1, 3)
gen_rgb = gen_rgb[:, :, ::-1] if False else gen_rgb
print("arr first pixel (r,g,b?)", gen_rgb[0, 0])
print("gt  first pixel (r,g,b?)", gtf[0, 0])
print("mae vs GT: %.5f" % float(np.abs(gen_rgb - gtf).mean()))
