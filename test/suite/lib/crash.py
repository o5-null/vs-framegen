"""崩溃现场收集：不依赖人眼复现，失败时把「能复现的东西」打包。

- 默认：把 argv、退出码、stdout/stderr、mpv/vs 日志和一行可直接粘贴的重现命令
  写进 ``case 目录\\crash.txt`` / ``repro.txt``。
- 有 procdump 且 ``VFI_DUMP=1``：runner 已经把进程包在 procdump 里，转储在 ``dumps\\``。
- 没有 procdump：给出开启 WER LocalDumps 的一条命令，用户自己跑一次即可拿到 .dmp。
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

from .runner import Result


def procdump() -> str | None:
    return shutil.which("procdump") or shutil.which("procdump64")


def wer_localdump_hint(exe_name: str = "mpv.exe") -> str:
    """管理员 PowerShell 跑一次，之后崩溃会落到 %LOCALAPPDATA%\\CrashDumps。"""
    key = rf"HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\{exe_name}"
    return (
        f'New-Item -Force -Path "{key}" | Out-Null; '
        f'Set-ItemProperty -Path "{key}" -Name DumpFolder -Value "%LOCALAPPDATA%\\CrashDumps"; '
        f'Set-ItemProperty -Path "{key}" -Name DumpType -Value 2'
    )


def bundle(
    dst: Path,
    *,
    result: Result | None = None,
    logs: list[Path] | None = None,
    note: str = "",
) -> Path:
    """把失败现场写进 dst，返回主文件路径。"""
    dst = Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    lines: list[str] = ["# 崩溃/失败现场", ""]
    if note:
        lines += [note, ""]

    if result is not None:
        lines += [
            f"退出码: {result.rc}" + ("（超时）" if result.timed_out else ""),
            f"耗时: {result.seconds:.3f}s",
            f"工作目录: {result.cwd}",
            "",
            "## 重现命令",
            "",
            f'    cd "{result.cwd}"',
            f"    {subprocess.list2cmdline(result.argv)}",
            "",
            "## stderr",
            "```",
            (result.err or "")[-8000:],
            "```",
            "",
            "## stdout",
            "```",
            (result.out or "")[-8000:],
            "```",
            "",
        ]

    for log in logs or []:
        log = Path(log)
        if log.exists():
            body = log.read_text(encoding="utf-8", errors="replace")
            lines += [f"## {log.name}", "```", body[-20000:], "```", ""]

    lines += ["## 转储工具", ""]
    if procdump():
        lines += [f"- procdump: {procdump()}（设 VFI_DUMP=1 跑一次即可拿到 .dmp）"]
    else:
        lines += [
            "- 未找到 procdump。需要 .dmp 时用管理员 PowerShell 执行：",
            "```powershell",
            wer_localdump_hint(),
            "```",
        ]

    path = dst / "crash.txt"
    path.write_text("\n".join(lines), encoding="utf-8")

    if result is not None:
        (dst / "repro.txt").write_text(
            subprocess.list2cmdline(result.argv) + "\n", encoding="utf-8"
        )
        (dst / "result.json").write_text(
            json.dumps(
                {"rc": result.rc, "cwd": str(result.cwd), "seconds": result.seconds},
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
    return path
