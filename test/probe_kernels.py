"""List the DLSS-G runtime's kernel entry-point names that hint at its own motion handling.

fg_kernels.dll is the compute kernel blob the runtime loads; its exported/embedded symbol names
are the only public evidence of what stages it runs internally.
"""

import re
import sys

PATH = r"E:\code\dlssg-vfi\work\dlssg_sm86\kernels\fg_kernels.dll"

NEEDLES = ("mvec", "Mvec", "flow", "Flow", "warp", "Warp", "optical", "Optical",
           "disoccl", "Disoccl", "blend", "Blend", "menu", "Menu")


def main():
    data = open(PATH, "rb").read()
    print("bytes:", len(data))

    # Kernel names in these blobs are plain ASCII, NUL-terminated, e.g. "Kernel_EstimateIntermMvecsScatter".
    names = set()
    for m in re.finditer(rb"[ -~]{6,96}", data):
        s = m.group().decode("ascii", "replace")
        if re.search(r"[A-Za-z]", s) and re.search(r"[a-z][A-Z]|_", s):
            names.add(s)
    hits = sorted(n for n in names if any(k in n for k in NEEDLES))
    print("candidate symbols:", len(hits))
    for n in hits[:120]:
        print("  ", n)


if __name__ == "__main__":
    main()
