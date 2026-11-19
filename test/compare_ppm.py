import hashlib
import os
import sys

BASE = r"E:\code\dlssg-vfi\work\out_base"
LITE = r"E:\code\dlssg-vfi\work\out_lite"


def digest(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def main():
    names = sorted(f for f in os.listdir(BASE) if f.lower().endswith(".ppm"))
    if not names:
        print("no ppm in", BASE)
        return 1
    same = 0
    for name in names:
        other = os.path.join(LITE, name)
        if not os.path.exists(other):
            print("%-16s missing in lite" % name)
            continue
        a, b = digest(os.path.join(BASE, name)), digest(other)
        flag = "SAME" if a == b else "DIFF"
        if a == b:
            same += 1
        print("%-16s %s %s %s" % (name, a, b, flag))
    print("---- %d/%d bit-identical ----" % (same, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
