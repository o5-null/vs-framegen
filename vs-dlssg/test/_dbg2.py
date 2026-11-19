import numpy as np, vapoursynth as vs
core = vs.core
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
for n in (0, 5, 10, 50, 100):
    a = np.asarray(src.get_frame(n)[0])
    print(n, a.mean(), a.max())
