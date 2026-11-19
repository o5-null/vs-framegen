import sys

import vapoursynth as vs

core = vs.core
print("vapoursynth", vs.__version__)
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\vs-dlssg\bin\vfsr4.dll")
print("loaded", core.vfsr4)
print(sorted(a for a in dir(core.vfsr4) if not a.startswith("_")))
sys.stdout.flush()
