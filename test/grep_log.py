"""Filter the noisy vspipe/NGX log (it contains NUL bytes and one enormous line).

Usage: python grep_log.py [logfile] [--all]
Prints only the lines this project cares about, truncated to a sane width.
"""

import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\code\dlssg-vfi\test\smoke.log"
show_all = "--all" in sys.argv

keys = ("[vdlssg]", "still allocated", "frames in", "FrameGeneration", "CreateFeature")
raw = open(path, "rb").read().decode("utf-8", "replace")

for line in raw.replace("\x00", " ").splitlines():
    line = line.rstrip()
    if not line.strip():
        continue
    if show_all or any(k in line for k in keys):
        # Guard against the single huge NGX line: keep it informative but readable.
        print(line[:200])
