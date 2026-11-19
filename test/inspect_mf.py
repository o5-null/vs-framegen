import os
import struct

WORK = r"E:\code\dlssg-vfi\work"
SEQ = r"E:\code\dlssg-vfi\media\seq"


def read_ppm(p):
    with open(p, "rb") as f:
        data = f.read()
    # header: P6\n<w> <h>\n255\n
    fields = []
    idx = 0
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
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[idx:]


def stats(name):
    p = os.path.join(WORK, name)
    if not os.path.exists(p):
        return None
    w, h, px = read_ppm(p)
    n = len(px)
    s = sum(px)
    mn, mx = min(px), max(px)
    return w, h, s / n, mn, mx


def read_raw(p):
    with open(p, "rb") as f:
        return f.read()


def mae_rgb(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(n)) / n


for tag in ("t00", "t05"):
    print("=== " + tag)
    for suf in ("_i0", "_i1", "_i2"):
        s = stats("mf_" + tag + suf + ".ppm")
        print(" ", suf, s)
    s = stats("mf_" + tag + "_gt1.ppm")
    print("  _gt1", s)
    # compare idx1/idx2 against the real frames f0..f3 (rgb24, same scale as ppm payload)
    frames = {}
    for i in range(4):
        frames[i] = read_raw(os.path.join(SEQ, tag + "_f%d.rgb" % i))
    for suf, label in (("_i1", "i1"), ("_i2", "i2")):
        p = os.path.join(WORK, "mf_" + tag + suf + ".ppm")
        if not os.path.exists(p):
            continue
        w, h, px = read_ppm(p)
        row = []
        for i in range(4):
            row.append("f%d=%.4f" % (i, mae_rgb(px, frames[i])))
        print(" ", label, " ".join(row))
