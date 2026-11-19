"""Prints the proxy's structured records about kernel coverage and depth.

The proxy log is line-oriented JSON. Reading it raw is useless (megabytes), so this
pulls out only the records that say which runtime kernels were matched and which
depth-related record names exist at all.
"""
import json
import re
import sys

PATH = r"E:\code\dlssg-vfi\work\dlssg_sm86\logs\dlssg_sm86.log"
WANT = ("kernel_pack", "backend_install", "install", "depth", "initialize", "evaluate")

seen = set()
with open(PATH, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            rec = json.loads(line)
        except Exception:
            continue
        name = ""
        for key in ("record", "type", "name", "event"):
            if isinstance(rec.get(key), str):
                name = rec[key]
                break
        blob = line.lower()
        interesting = any(w in name.lower() for w in WANT) or "kernel_pack" in blob
        if not interesting:
            continue
        key = (name, line[:120])
        if key in seen:
            continue
        seen.add(key)
        if name in ("kernel_pack",) or "kernels_unused" in blob:
            print(line[:2000])
            print("-" * 60)
