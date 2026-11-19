import os, numpy as np, vapoursynth as vs
core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\vs-dlssg\bin\vvfg.dll")
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 0, 5)
out = core.vvfg.FrameGen(src, multi=2, proxy_dir=r"E:\code\dlssg-vfi\vs-dlssg\engine_nvvfx", log=1)
out.set_output()
f = out.get_frame(0)
p = f[0]
a = np.asarray(p)
print(type(p), a.dtype, a.shape, a.min(), a.max(), a.mean())
