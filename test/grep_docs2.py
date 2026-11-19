import glob
import os

root = r"E:\code\dlssg-vfi"
keys = ("不需要光流", "不需要深度", "无需光流", "无需深度", "ignores both", "lights both")
for path in sorted(glob.glob(os.path.join(root, "*.md")) + glob.glob(os.path.join(root, "docs", "*.md"))):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            if any(k in line for k in keys):
                print(f"{os.path.relpath(path, root)}:{i}: {line.rstrip()[:180]}")
