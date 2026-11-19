import vapoursynth as vs

core = vs.core
src = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
print("src format:", src.format, "id:", src.format.id, "family:", src.format.color_family)
print("props:", src.get_frame(0).props)
