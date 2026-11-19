import re

data = open(r"E:\code\dlssg-vfi\vs-dlssg\engine_nvvfx\nvVFXVideoFrameGeneration.dll", "rb").read()
seen = set()
for m in re.finditer(rb"[ -~]{30,900}", data):
    s = m.group().decode("ascii", "replace")
    if re.search(r"\((uint32|float|NvCVImage\*|uint8)\)", s) and s not in seen:
        seen.add(s)
        print(repr(s))
