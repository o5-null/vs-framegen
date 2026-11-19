import glob
import os
import re

root = r"E:\code\dlssg-vfi\docs"
keys = ("光流", "深度", "optical flow", "depth")
for path in sorted(glob.glob(os.path.join(root, "*.md"))):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            if any(k in line for k in keys):
                print(f"{os.path.basename(path)}:{i}: {line.rstrip()[:160]}")
