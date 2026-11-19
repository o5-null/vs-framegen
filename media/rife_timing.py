"""Time rife-ncnn-vulkan on the same 4K frames the spike used, so throughput is comparable.

rife-ncnn-vulkan is the Vulkan/ncnn build, slower than vs-mlrt's TensorRT path, so this is an
upper bound on RIFE cost, not a fair fight; it is here to show the gap's order of magnitude.
"""

import os
import shutil
import subprocess
import time

ROOT = r"C:\Users\benben\tools\vsdlssg-ampere\rife4k"
EXE = r"C:\Users\benben\tools\vsdlssg-ampere\rife\rife-ncnn-vulkan-20221029-windows\rife-ncnn-vulkan.exe"


def main():
    src = os.path.join(ROOT, "in")
    for run in range(2):
        out = os.path.join(ROOT, "out%d" % run)
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        t0 = time.perf_counter()
        subprocess.run([EXE, "-i", src, "-o", out, "-m", "rife-v4.6", "-g", "0"], check=True,
                       stdout=subprocess.DEVNULL)
        total = (time.perf_counter() - t0) * 1000.0
        n = len(os.listdir(out))
        pairs = n // 2 - 1
        print("run %d: %.0f ms total, %d output frames, %.0f ms per interpolated frame"
              % (run, total, n, total / max(pairs, 1)))


if __name__ == "__main__":
    main()
