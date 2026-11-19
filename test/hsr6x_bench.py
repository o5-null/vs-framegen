"""Speed on the HSR PV at multi = 2/4/6 (native 4K and optionally 1080p), best of N.

Each configuration is run through vspipe with log=1, so the numbers come from the same harness as
docs\vdlssg-throughput.md. Usage: python test\hsr6x_bench.py [runs] [native|1080p|both]
"""

import os
import re
import statistics
import subprocess
import sys

VSPIPE = r"E:\code\VFI\.venv\Scripts\vspipe.exe"
ROOT = r"E:\code\dlssg-vfi"
SCRIPT = os.path.join(ROOT, "test", "hsr6x_perf.py")
SRC = os.path.join(ROOT, "测试数据",
                   "《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv")
START = 3510
GROUPS = 30  # 30 source pairs per run, so every multi renders the same number of source frames

FRAME_MS = re.compile(r"\[vdlssg\] frame \d+ from \d+/\d+ slot (\d+)/(\d+): ([\d.]+) ms")
TOTAL = re.compile(r"Output (\d+) frames in ([\d.]+) seconds \(([\d.]+) fps\)")

RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 2
WHICH = sys.argv[2] if len(sys.argv) > 2 else "native"


def bench(multi, width, tag, runs):
    frames = GROUPS * multi
    results = []
    for i in range(runs):
        err = os.path.join(ROOT, "work", "hsr_bench_%s_m%d.err" % (tag, multi))
        env = dict(os.environ, VDLSSG_SRC=SRC, VDLSSG_MULTI=str(multi),
                   VDLSSG_START=str(START), VDLSSG_END=str(START + frames),
                   VDLSSG_W=str(width))
        with open(err, "wb") as fh:
            subprocess.run([VSPIPE, "-p", SCRIPT, "NUL"],
                           stdout=subprocess.DEVNULL, stderr=fh, check=False, env=env)
        text = open(err, "rb").read().decode("utf-8", "replace")
        total = TOTAL.search(text)
        times = [float(m.group(3)) for m in FRAME_MS.finditer(text)]
        if not total:
            print("multi=%d %s run %d: failed (see %s)" % (multi, tag, i + 1, err))
            print(text[-800:])
            continue
        out_fps = float(total.group(3))
        results.append((out_fps, statistics.fmean(times) if times else float("nan")))
        print("multi=%d %s run %d: %s frames %.2f s = %.2f fps out, evaluate mean %.2f ms"
              % (multi, tag, i + 1, total.group(1), float(total.group(2)), out_fps,
                 results[-1][1]))
    if results:
        best = max(results, key=lambda r: r[0])
        print("multi=%d %s BEST: %.2f fps out = %.2f source frames/s, evaluate mean %.2f ms\n"
              % (multi, tag, best[0], best[0] / multi, best[1]))


if __name__ == "__main__":
    for multi in (2, 4, 6):
        if WHICH in ("native", "both"):
            bench(multi, 0, "4k", RUNS)
        if WHICH in ("1080p", "both"):
            bench(multi, 1920, "1080p", RUNS)
