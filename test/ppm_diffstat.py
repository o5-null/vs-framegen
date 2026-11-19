"""Difference statistics between two directories of 8-bit PPM frames.

compare_dirs.py only answers identical / not identical. When a run differs by a few
least-significant bits that is a completely different finding from a run that differs
visibly, so this reports the magnitude.
"""
import os
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise SystemExit(f"not a P6 ppm: {path} ({magic!r})")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        width, height = (int(v) for v in line.split())
        maxval = int(f.readline())
        if maxval != 255:
            raise SystemExit(f"unsupported maxval {maxval} in {path}")
        data = f.read()
    if len(data) != width * height * 3:
        raise SystemExit(f"truncated ppm: {path}")
    return width, height, data


def main(base, other):
    names = sorted(
        n
        for n in os.listdir(base)
        if n.endswith(".ppm") and not n.endswith("_expected.ppm") and not n.endswith("_blend.ppm")
    )
    print(f"{'case':<12} {'max':>4} {'mean':>8} {'px>1':>8} {'px>8':>8}")
    for name in names:
        p2 = os.path.join(other, name)
        if not os.path.exists(p2):
            print(f"{name:<12} missing")
            continue
        w, h, a = read_ppm(os.path.join(base, name))
        w2, h2, b = read_ppm(p2)
        if (w, h) != (w2, h2):
            print(f"{name:<12} size mismatch")
            continue
        mx = 0
        total = 0
        over1 = 0
        over8 = 0
        for x, y in zip(a, b):
            d = x - y if x > y else y - x
            if d:
                total += d
                if d > mx:
                    mx = d
                if d > 1:
                    over1 += 1
                if d > 8:
                    over8 += 1
        n = len(a)
        print(f"{name:<12} {mx:>4} {total / n:>8.4f} {over1 * 100.0 / n:>7.3f}% {over8 * 100.0 / n:>7.3f}%")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: ppm_diffstat.py <base_dir> <other_dir>")
        raise SystemExit(1)
    main(sys.argv[1], sys.argv[2])
