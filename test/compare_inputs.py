"""比较同一下载/运行参数下，只改运动矢量或深度后输出是否逐字节相同。"""
import hashlib
import os
import sys

ROOT = r"E:\code\dlssg-vfi\work"
REF = "out_base"
OTHERS = sys.argv[1:] or ["out_mvzero", "out_mvflip", "out_mvhuge", "out_depthrand", "out_blockmv"]


def digest(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def main():
    ref_dir = os.path.join(ROOT, REF)
    names = sorted(f for f in os.listdir(ref_dir) if f.endswith(".ppm") and "_expected" not in f
                   and "_blend" not in f)
    if not names:
        print("no generated ppm in", ref_dir)
        return 1
    print("reference:", REF, "cases:", len(names))
    for other in OTHERS:
        d = os.path.join(ROOT, other)
        if not os.path.isdir(d):
            print("%-16s (no directory)" % other)
            continue
        same = 0
        missing = 0
        diffs = []
        for name in names:
            p = os.path.join(d, name)
            if not os.path.exists(p):
                missing += 1
                continue
            if digest(p) == digest(os.path.join(ref_dir, name)):
                same += 1
            else:
                diffs.append(name)
        print("%-16s identical %d/%d  missing %d  diff %s"
              % (other, same, len(names), missing, ",".join(x.replace("spike_", "").replace(".ppm", "")
                                                            for x in diffs) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
