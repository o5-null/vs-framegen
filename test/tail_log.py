import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\code\dlssg-vfi\test\mvdh.log"
tail = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
raw = open(path, "rb").read().decode("utf-8", "replace")
print("bytes:", len(raw))
print(raw[-tail:])
