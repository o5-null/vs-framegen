"""2160p throughput with the plugin: decode -> upscale to 4K RGBS -> FrameGen -> sink."""

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\plugin\vdlssg.dll")

src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
big = core.resize.Bicubic(src, width=3840, height=2160, format=vs.RGBS, matrix_in_s="709")
out = core.vdlssg.FrameGen(big, proxy_dir=r"E:\code\dlssg-vfi\work", log=1)
out.set_output()
