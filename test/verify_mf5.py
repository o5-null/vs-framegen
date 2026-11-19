"""Verify that a multi-frame run put each index at the right time position.

For --len 5 (multiFrameCount = 3) index i should land at i/4 of the way from f0 to f4, so
index 1/2/3 must be closest to f1/f2/f3. Reads the ppm files the last run left in work\.
"""

import os

WORK = r"E:\code\dlssg-vfi\work"
SEQ = r"E:\code\dlssg-vfi\media\seq"


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, idx = [], 0
    while len(fields) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(data[start:idx])
    idx += 1
    return int(fields[1]), int(fields[2]), data[idx:]


def mae(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(n)) / n


missing = 0
for tag in ("t00", "t01", "t05", "t06", "t08"):
    print("=== " + tag)
    frames = []
    for i in range(5):
        p = os.path.join(SEQ, "%s_f%d.rgb" % (tag, i))
        if not os.path.exists(p):
            print("  missing sequence frame", p)
            missing += 1
            frames = []
            break
        frames.append(open(p, "rb").read())

    for idx in (1, 2, 3):
        p = os.path.join(WORK, "mf_%s_i%d.ppm" % (tag, idx))
        if not os.path.exists(p) or not frames:
            print("  i%d: no output" % idx)
            continue
        _, _, px = read_ppm(p)
        d = [mae(px, f) for f in frames]
        best = min(range(len(d)), key=lambda i: d[i])
        print("  i%d -> f%d  %s" % (idx, best, " ".join("f%d=%.3f" % (i, v) for i, v in enumerate(d))))

print("done, missing=%d" % missing)
