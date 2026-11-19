# 6x smoke: 1080p Sintel through the engine45 pack (MaxGeneratedFrames=5).
# Run after rebuilding the plugin:  vspipe -p test\mf6.vpy NUL
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\plugin\vdlssg.dll")
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")
src = core.std.Trim(src, 0, 47)   # 48 source frames -> 288 output frames, quick smoke
out = core.vdlssg.FrameGen(src, multi=6, proxy_dir=r"E:\code\dlssg-vfi\dist\engine45", log=1)
out.set_output()
