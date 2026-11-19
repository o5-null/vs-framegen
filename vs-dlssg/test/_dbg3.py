import numpy as np, vapoursynth as vs
core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\vs-dlssg\bin\vvfg.dll")
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 90, 95)
out = core.vvfg.FrameGen(src, multi=4, proxy_dir=r"E:\code\dlssg-vfi\vs-dlssg\engine_nvvfx", log=0)
out.set_output()
g = lambda n: [np.asarray(out.get_frame(n)[i]) for i in range(3)]
a, b = g(8), g(12)
for n in (9, 10, 11):
    f = g(n)
    da = np.mean([np.abs(f[i]-a[i]).mean() for i in range(3)])
    db = np.mean([np.abs(f[i]-b[i]).mean() for i in range(3)])
    print("tween %d: to prev %.4f  to next %.4f  ratio %.3f" % (n, da, db, da/(da+db)))
