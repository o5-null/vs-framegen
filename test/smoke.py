import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\vs-dlssg\bin\vdlssg.dll")

src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
out = core.vdlssg.FrameGen(src, proxy_dir=r"E:\code\dlssg-vfi\work", log=1)
out.set_output()
