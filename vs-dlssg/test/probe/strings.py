import re

data = open(r"E:\code\dlssg-vfi\vs-dlssg\vfg\nvVFXVideoFrameGeneration.dll", "rb").read()
pat = rb"[ -~]{50,800}"
seen = set()
for m in re.finditer(pat, data):
    s = m.group().decode("ascii", "replace")
    if re.search(r"(?i)timestep|frameindex|framemultiplier|shot change|interpolat", s):
        if s not in seen:
            seen.add(s)
            print(repr(s))
