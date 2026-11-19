import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\code\dlssg-vfi\work\mf.log"
raw = open(path, "rb").read().decode("utf-8", "replace")
keep = ("idx", "differ", "MF", "mf_", "mean over", "all indices", "improvement", "adapter",
        "CreateFeature", "MultiFrameCountMax", "resolution", "loaded", "NGX init", "proxy",
        "suppressed", "FAILED", "no frame")
for line in raw.splitlines():
    line = line.replace("\x00", " ").rstrip()
    if not line.strip():
        continue
    if any(k in line for k in keep):
        print(line[:220])
