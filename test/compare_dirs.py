import hashlib
import os
import sys

ROOT = r"E:\code\dlssg-vfi\work"


def digest(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ppm_files(d):
    if not os.path.isdir(d):
        return None
    out = []
    for name in sorted(os.listdir(d)):
        if not name.endswith(".ppm"):
            continue
        # Generated frames only: the baselines live beside them.
        if name.endswith("_expected.ppm") or name.endswith("_blend.ppm"):
            continue
        out.append(name)
    return out


def compare(base, other):
    names = ppm_files(base)
    if names is None:
        print(f"reference missing: {base}")
        return
    cand = ppm_files(other)
    if cand is None:
        print(f"{os.path.basename(other):<20} (no directory)")
        return
    same, diff, missing = 0, [], 0
    for name in names:
        p2 = os.path.join(other, name)
        if not os.path.exists(p2):
            missing += 1
            continue
        if digest(os.path.join(base, name)) == digest(p2):
            same += 1
        else:
            diff.append(name.split(".")[0])
    tag = ",".join(diff) if diff else "-"
    print(f"{os.path.basename(other):<20} identical {same}/{len(names)}  missing {missing}  diff {tag}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: compare_dirs.py <base> <dir> [dir...]")
        raise SystemExit(1)
    base = sys.argv[1] if os.path.isabs(sys.argv[1]) else os.path.join(ROOT, sys.argv[1])
    print(f"reference: {base}")
    for arg in sys.argv[2:]:
        compare(base, arg if os.path.isabs(arg) else os.path.join(ROOT, arg))
