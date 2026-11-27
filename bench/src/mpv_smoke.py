r"""mpv 播放链路冒烟：用部署目录里的新 DLL 跑 MEMC 脚本，确认不崩、帧数倍增。

用法：python bench\src\mpv_smoke.py [脚本名] [源] [--frames N]
默认 MEMC_DLSSG_NV.vpy + 测试数据\hdr10.mkv，播放 30 帧后退出。
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(r"E:\code\dlssg-vfi")
MPV = ROOT / "测试环境" / "mpv-lazy-nv" / "mpv.exe"
# 播放时实际用的是部署目录里的脚本（portable_config\vs）
VS_SCRIPT = ROOT / "测试环境" / "mpv-lazy-nv" / "portable_config" / "vs"
LOG = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\mpv_smoke.log")

KEYS = ("vapoursynth", "vdlssg", "vvfg", "error", "fail", "fps")


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    frames = "30"
    for a in sys.argv[1:]:
        if a.startswith("--frames="):
            frames = a.split("=", 1)[1]
    script = args[0] if args else "MEMC_DLSSG_NV.vpy"
    src = args[1] if len(args) > 1 else str(ROOT / "测试数据" / "hdr10.mkv")

    cmd = [
        str(MPV), "--no-config", "--vo=null", "--hwdec=no", "--no-audio",
        "--msg-level=all=info,vapoursynth=v", f"--frames={frames}",
        f'--vf=vapoursynth="{VS_SCRIPT / script}"',
        src,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=600)
    text = proc.stdout + proc.stderr
    LOG.write_text(text, encoding="utf-8")
    keep = [l for l in text.splitlines() if any(k in l.lower() for k in KEYS)]
    print("\n".join(keep[:60]))
    print(f"exit={proc.returncode} log={LOG}")
    return 0 if proc.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
