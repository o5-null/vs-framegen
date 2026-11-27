"""用合成裁切片验证插件的切镜/重复帧判定（fallback 1 = 切镜, 4 = 重复帧）。

运行：python bench\src\verify_cut_detect.py
片源来自 scene_detect_bench 的产物（已知标注：29/77/159/179 切镜，49 闪光，
129/139/219..224 重复帧）。输出每个源帧对应 slot1 的判定。
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"E:\code\dlssg-vfi")
BIN = ROOT / "vs-dlssg" / "bin"
VSPIPE = ROOT / ".venv" / "Scripts" / "vspipe.exe"
WORK = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\scbench")
SRC = WORK / "clip.mkv"

SCRIPT = r'''
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"{bsdll}")
core.std.LoadPlugin(r"{vdll}")
sys.path.insert(0, r"{k7}")

src = core.bs.VideoSource(source=r"{src}", cachepath=r"{cache}")
fg = core.vdlssg.FrameGen(src, multi=2, proxy_dir=r"{bin}\dlss45",
                          scene_threshold={thr}, duplicate_threshold={dup})

def dump(n, f):
    p = f.props
    rec = dict(n=n, fb=int(p.get("_DLSSGFallback", -1)),
               cut=int(p.get("_DLSSGSceneCut", -1)),
               dup=int(p.get("_DLSSGDuplicate", -1)),
               slot=int(p.get("_DLSSGMultiFrameIndex", -1)))
    print("REC " + __import__("json").dumps(rec), file=sys.stderr)
    return f

out = fg.std.ModifyFrame(fg, dump)
out.set_output()
'''


def main() -> int:
    if not SRC.exists():
        print(f"缺少片源 {SRC}")
        return 1
    script = WORK / "verify.vpy"
    script.write_text(
        SCRIPT.format(k7=ROOT / "k7sfunc" / "src", src=SRC, cache=WORK / "bs",
                      bsdll=ROOT / "测试环境" / "mpv-lazy-nv" / "vs-plugins" / "BestSource.dll",
                      vdll=BIN / "vdlssg.dll",
                      bin=BIN, thr=0.12, dup=0.005),
        encoding="utf-8")
    proc = subprocess.run([str(VSPIPE), str(script), "-"], capture_output=True)
    recs = []
    for line in proc.stderr.decode("utf-8", "replace").splitlines():
        if line.startswith("REC "):
            recs.append(json.loads(line[4:]))
    if not recs:
        print(proc.stderr.decode("utf-8", "replace")[-2000:])
        return 1
    # slot1 才是生成帧；把源帧号换算回来
    cuts, dups = [], []
    for r in recs:
        if r["slot"] != 1:
            continue
        src_n = r["n"] // 2
        if r["fb"] == 1:
            cuts.append(src_n)
        elif r["fb"] == 4:
            dups.append(src_n)
    print("切镜判定:", cuts)
    print("重复帧判定:", dups)
    expect_cuts = [29, 77, 159, 179]
    print("标注切镜:", expect_cuts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
