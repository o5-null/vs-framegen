"""Summarise a vspipe stderr log: total fps plus the plugin's per-frame evaluate time."""

import re
import statistics
import sys

FRAME_MS = re.compile(
    r"\[vdlssg\] frame (\d+) from (\d+)/(\d+) slot (\d+)/(\d+): ([\d.]+) ms")
SLOT = re.compile(r"slot (\d+)/(\d+):")
TOTAL = re.compile(r"Output (\d+) frames in ([\d.]+) seconds \(([\d.]+) fps\)")


def main(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")

    times = [float(m.group(6)) for m in FRAME_MS.finditer(text)]
    if times:
        ordered = sorted(times)
        p90 = ordered[int(len(ordered) * 0.9)] if len(ordered) > 1 else ordered[0]
        print("generated frames: %d" % len(times))
        print("evaluate ms: min %.2f  p50 %.2f  p90 %.2f  mean %.2f  max %.2f" % (
            ordered[0], statistics.median(times), p90,
            statistics.fmean(times), ordered[-1]))
        print("generation rate: %.1f frames/s (per generated frame)" % (1000.0 / statistics.fmean(times)))

    for m in TOTAL.finditer(text):
        print("vspipe: %s frames in %s s = %s fps (output frames)" % m.groups())

    for line in text.splitlines():
        if "still allocated" in line or "Core freed" in line:
            print("LEAK:", line.strip())


if __name__ == "__main__":
    main(sys.argv[1])
