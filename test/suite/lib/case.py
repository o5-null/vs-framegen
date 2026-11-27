"""测试用例的运行句柄：一个 tested 场景一个目录。

    r = case.run("color-red")
    r.vpy("clip = core.std.BlankClip(video_in, color=[63, 102, 240])\\nclip.set_output()")
    m = r.mpv(frames=2)
    px = r.frames()

产物布局：``test/artifacts/<时间戳>/<用例名>/<场景>/``，里面固定有
``argv.txt``/``stdout.log``/``stderr.log``/``mpv.log``/``vs.log``/``frames/``。
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path

from . import crash, frames as frameslib, mpv as mpvlib, vpy as vpylib
from .runner import Result, run


def _slug(nodeid: str) -> str:
    text = nodeid.replace("::", "_")
    text = re.sub(r"\[(.+)\]", r"_\1", text)
    text = re.sub(r"[^0-9A-Za-z_.\-]+", "_", text)
    return text.strip("_")[:120] or "case"


class Case:
    def __init__(self, base: Path, *, mpv_exe: Path | None, vspipe_exe: Path | None):
        self.base = Path(base)
        self.base.mkdir(parents=True, exist_ok=True)
        self.mpv_exe = mpv_exe
        self.vspipe_exe = vspipe_exe
        self._runs: list[Run] = []

    def run(self, name: str) -> "Run":
        r = Run(self, name)
        self._runs.append(r)
        return r

    def save(self, name: str, text: str) -> Path:
        path = self.base / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def crash(self, note: str, result: Result | None = None, logs: list[Path] | None = None) -> Path:
        return crash.bundle(self.base, result=result, logs=logs, note=note)


class Run:
    def __init__(self, case: Case, name: str):
        self.case = case
        self.name = name
        self.dir = case.base / name
        self.dir.mkdir(parents=True, exist_ok=True)

    # ---- 脚本 ----
    def vpy(self, body: str, *, props: bool = False, name: str = "script.vpy") -> Path:
        return vpylib.write_script(self.dir / name, body, props=props)

    def vpy_text(self) -> str:
        path = self.dir / "script.vpy"
        return path.read_text(encoding="utf-8") if path.exists() else ""

    # ---- 执行 ----
    def mpv(self, source: str | Path | None = None, **kwargs) -> mpvlib.MpvRun:
        if self.case.mpv_exe is None:
            raise AssertionError("找不到 mpv.exe（设 VFI_MPV）")
        vf = kwargs.pop("vf_script", "script.vpy" if (self.dir / "script.vpy").exists() else None)
        return mpvlib.run_mpv(self.case.mpv_exe, source or "av://lavfi:color=c=black:s=320x240:r=24:d=2",
                              cwd=self.dir, vf_script=vf, **kwargs)

    def vspipe(self, script: Path | None = None, **kwargs) -> Result:
        if self.case.vspipe_exe is None:
            raise AssertionError("找不到 vspipe.exe（装到 .venv 或设 VFI_VSPIPE）")
        return vpylib.run_vspipe(self.case.vspipe_exe, script or (self.dir / "script.vpy"),
                                 cwd=self.dir, **kwargs)

    # ---- 产物 ----
    def frames(self, *, limit: int | None = None):
        return frameslib.load_sequence(self.dir / "frames", limit=limit)

    def vs_log(self) -> str:
        return vpylib.read_vs_log(self.dir)

    def props(self) -> list[dict]:
        return vpylib.read_props(self.dir)

    def vs_info(self) -> dict:
        return vpylib.read_info(self.dir)

    def copy(self, src: Path, name: str | None = None) -> Path:
        dst = self.dir / (name or Path(src).name)
        shutil.copy2(src, dst)
        return dst

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return path
