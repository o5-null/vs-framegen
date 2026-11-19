"""Why does VFG cost 37.8 ms at 1600x900 but 23.5 ms at 1920x1080, and why does it
refuse 480p..960p? Both smell like an internal size/alignment rule rather than work.

Probes a few sizes around the two anomalies: one axis kept, the other snapped to 8/16.
"""
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import common as C
import smv_run

SIZES = [
    (1024, 576),   # below the smallest size seen working so far
    (1152, 648),
    (1280, 720),   # smallest known working
    (1280, 716),   # height not a multiple of 8
    (1600, 900),   # slow outlier
    (1600, 896),   # same width, height snapped to 8
    (1596, 900),
    (2240, 1260),  # slow outlier
    (2240, 1264),
    (1920, 1080),  # fast reference
    (1920, 1076),
]


def main():
    for w, h in SIZES:
        raw = C.ensure_raw(w, h, "probe%d" % w, 12)
        info = smv_run.run(["--nvvfg"], raw, w, h, 12)
        if info["exit"] != 0 or not info["pairs"]:
            print("%4dx%-4d FAILED   %s" % (w, h, info["log"].strip().splitlines()[-1][:90]), flush=True)
            continue
        print("%4dx%-4d %7.2f ms/pair   %s" % (w, h, info["ms_per_pair"],
                                               "div8" if (w % 8 == 0 and h % 8 == 0) else "-"), flush=True)
        if info["pairs"] and info.get("frames_out"):
            print("            frames out %d" % info["frames_out"], flush=True)


if __name__ == "__main__":
    main()
