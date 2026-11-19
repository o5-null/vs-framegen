"""Did the supplied motion field change the output at all?

Compares results\<a>\frames against results\<b>\frames pixel by pixel, 16-bit domain.
Usage: python vd_diff_methods.py [a] [b]
"""

import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd


def main():
    a = sys.argv[1] if len(sys.argv) > 1 else "dlssg"
    b = sys.argv[2] if len(sys.argv) > 2 else "dlssg_mv"
    triples = vd.triplets()
    diffs = []
    for _, gt, _ in triples:
        pa = vd.frame_path(a, gt)
        pb = vd.frame_path(b, gt)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print("missing", vd.gt_name(gt))
            continue
        ia = np.clip(vd.read_rgb16(pa) * 65535.0, 0, 65535)
        ib = np.clip(vd.read_rgb16(pb) * 65535.0, 0, 65535)
        d = np.abs(ia - ib)
        diffs.append((vd.gt_name(gt), float(d.mean()), float(d.max()),
                      float(np.mean(d > 0.5)) * 100.0))
    print("%-10s %10s %8s %8s" % ("gt", "mean", "max", "pixels%"))
    for name, m, mx, pct in diffs:
        print("%-10s %10.2f %8.0f %7.2f%%" % (name, m, mx, pct))
    if diffs:
        print("frames with any change: %d/%d  mean diff %.2f code" %
              (sum(1 for d in diffs if d[1] > 0.0), len(diffs),
               float(np.mean([d[1] for d in diffs]))))


if __name__ == "__main__":
    main()
