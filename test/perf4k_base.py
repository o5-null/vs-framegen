"""Same 2160p graph without FrameGen: how much of the wall time is decode + upscale."""

import vapoursynth as vs

core = vs.core

src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
big = core.resize.Bicubic(src, width=3840, height=2160, format=vs.RGBS, matrix_in_s="709")
big.set_output()
