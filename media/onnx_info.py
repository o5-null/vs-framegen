r"""Inspect ONNX VFI models: input/output names, shapes, dtypes. Used to decide whether a
model can be driven directly through onnxruntime for the ten-triplet comparison.
"""

import argparse
import os

import onnxruntime as ort


def show(path):
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    print("==", os.path.basename(path))
    for i in sess.get_inputs():
        print("  in  %-12s %-14s %s" % (i.name, i.type, i.shape))
    for o in sess.get_outputs():
        print("  out %-12s %-14s %s" % (o.name, o.type, o.shape))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    args = ap.parse_args()
    for p in args.paths:
        show(p)


if __name__ == "__main__":
    main()
