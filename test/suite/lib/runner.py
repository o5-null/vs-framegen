"""子进程封装：统一 UTF-8、超时、退出码与产物落盘。

测试里所有外部命令（mpv / vspipe / ffmpeg）都走这里，保证
失败时能拿到完整的 argv、stdout、stderr 和耗时。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

#: 子进程统一环境：不写 .pyc（stage 里会污染 verify）、强制 UTF-8
BASE_ENV = {
    "PYTHONDONTWRITEBYTECODE": "1",
    "PYTHONUTF8": "1",
    "PYTHONIOENCODING": "utf-8",
}


@dataclass
class Result:
    argv: list[str]
    cwd: Path
    rc: int
    out: str
    err: str
    seconds: float
    timed_out: bool = False
    extra: dict = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.rc == 0 and not self.timed_out

    def save(self, dst: Path, tag: str = "") -> Path:
        """把 argv/输出/元数据写进 dst，返回本次落盘的元数据文件。"""
        dst.mkdir(parents=True, exist_ok=True)
        stem = f"{tag}." if tag else ""
        (dst / f"{stem}argv.txt").write_text("\n".join(self.argv) + "\n", encoding="utf-8")
        (dst / f"{stem}stdout.log").write_text(self.out, encoding="utf-8", errors="replace")
        (dst / f"{stem}stderr.log").write_text(self.err, encoding="utf-8", errors="replace")
        meta = {
            "cwd": str(self.cwd),
            "rc": self.rc,
            "seconds": round(self.seconds, 3),
            "timed_out": self.timed_out,
            **self.extra,
        }
        path = dst / f"{stem}meta.json"
        path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
        return path

    def tail(self, lines: int = 40) -> str:
        """失败信息用：stderr 优先，拼上 stdout 尾部。"""
        text = (self.err or "").strip() or (self.out or "").strip()
        return "\n".join(text.splitlines()[-lines:])


def build_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env.update(BASE_ENV)
    if extra:
        env.update({k: str(v) for k, v in extra.items()})
    return env


def run(
    argv: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    timeout: float = 300.0,
    stdin: str | None = None,
) -> Result:
    """执行命令并完整捕获输出；超时不抛异常，交给调用方断言。"""
    cwd = Path(cwd or os.getcwd())
    started = time.perf_counter()
    try:
        proc = subprocess.run(
            argv,
            cwd=str(cwd),
            env=build_env(env),
            input=stdin,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        return Result(argv, cwd, proc.returncode, proc.stdout or "", proc.stderr or "",
                      time.perf_counter() - started)
    except subprocess.TimeoutExpired as exc:
        return Result(
            argv,
            cwd,
            -999,
            _as_text(exc.stdout),
            _as_text(exc.stderr) + f"\n[timeout] 超过 {timeout:g}s 未结束",
            time.perf_counter() - started,
            timed_out=True,
        )
    except FileNotFoundError as exc:
        return Result(argv, cwd, -998, "", f"[missing] {exc}", time.perf_counter() - started)


def _as_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value


def wrap_with_dumper(argv: list[str], dumpdir: Path) -> list[str]:
    """``VFI_DUMP=1`` 且有 procdump 时，把命令包一层拿崩溃转储。

    procdump 不在 PATH 时原样返回，由 crash.collect() 生成文字版崩溃包。
    """
    if os.environ.get("VFI_DUMP", "") not in ("1", "true", "yes"):
        return argv
    exe = shutil.which("procdump") or shutil.which("procdump64")
    if not exe:
        return argv
    dumpdir.mkdir(parents=True, exist_ok=True)
    return [exe, "-accepteula", "-ma", "-e", "1", "-x", str(dumpdir), *argv]
