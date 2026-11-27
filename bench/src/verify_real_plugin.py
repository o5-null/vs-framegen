"""让真插件跑一段真实素材，统计切镜/重复帧判定密度（每 100 帧）。

用法：python bench\src\verify_real_plugin.py [样本索引] [帧数]
输出：切镜数、重复帧数、以及每 100 帧密度，便于和 python 复刻实现对照。
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"E:\code\dlssg-vfi")
BIN = ROOT / "vs-dlssg" / "bin"
VSPIPE = ROOT / ".venv" / "Scripts" / "vspipe.exe"
WORK = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\realbench")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import real_data_bench as B  # noqa: E402

SCRIPT = r'''
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"{bsdll}")
core.std.LoadPlugin(r"{vdll}")
src = core.bs.VideoSource(source=r"{src}", cachepath=r"{cache}")[{start}:{stop}]
fg = core.vdlssg.FrameGen(src, multi=2, proxy_dir=r"{bin}\dlss45",
                          scene_threshold={thr}, duplicate_threshold={dup})

def dump(n, f):
    p = f.props
    rec = dict(n=n, fb=int(p.get("_DLSSGFallback", -1)),
               slot=int(p.get("_DLSSGMultiFrameIndex", -1)))
    print("REC " + __import__("json").dumps(rec), file=sys.stderr)
    return f

fg.std.ModifyFrame(fg, dump).set_output()
'''


def main() -> int:
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    off = "--off" in sys.argv  # 关掉两套检测，用于量耗时差
    thr, dup = ("0", "0") if off else ("0.12", "0.005")
    label, path, start = B.SAMPLES[idx]
    script = WORK / f"real_check_{idx}{'_off' if off else ''}.vpy"
    script.write_text(SCRIPT.format(
        bsdll=ROOT / "测试环境" / "mpv-lazy-nv" / "vs-plugins" / "BestSource.dll",
        vdll=BIN / "vdlssg.dll", src=path, cache=WORK / "bs",
        start=start, stop=start + frames, bin=BIN, thr=thr, dup=dup), encoding="utf-8")
    import time

    t0 = time.perf_counter()
    proc = subprocess.run([str(VSPIPE), str(script), "-"], capture_output=True)
    elapsed = time.perf_counter() - t0
    recs = []
    for line in proc.stderr.decode("utf-8", "replace").splitlines():
        if line.startswith("REC "):
            recs.append(json.loads(line[4:]))
    if not recs:
        print(proc.stderr.decode("utf-8", "replace")[-3000:])
        return 1
    cuts = [r["n"] // 2 for r in recs if r["slot"] == 1 and r["fb"] == 1]
    dups = [r["n"] // 2 for r in recs if r["slot"] == 1 and r["fb"] == 4]
    print(f"{label}  {frames} 帧（源帧 {start}..{start + frames}）")
    print(f"切镜 {len(cuts)} 次 = {len(cuts) / frames * 100:.1f}/100 帧")
    print(f"重复帧 {len(dups)} 次 = {len(dups) / frames * 100:.1f}/100 帧")
    print(f"耗时 {elapsed:.2f}s = {elapsed / frames * 1000:.1f} ms/源帧")
    print("切镜边界:", cuts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
