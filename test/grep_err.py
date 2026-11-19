"""Filters the vspipe stderr for the lines that matter.

vspipe's stderr mixes NGX's megabyte of chatty logging with our own prints, so the
interesting lines are pulled out by marker instead of reading the file by hand.
"""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\code\dlssg-vfi\test\smoke2.err"
keys = (
    "vdlssg]",
    "still allocated",
    "Failed to retrieve",
    "Error",
    "error",
    "Output ",
    "frames in",
    "Traceback",
)
raw = open(path, "rb").read().decode("utf-8", "replace")
lines = [l for l in raw.splitlines() if any(k in l for k in keys)]
print(f"total lines {len(raw.splitlines())}, showing {len(lines)}")
for line in lines:
    print(line)
