"""子进程封装：统一 UTF-8、超时、退出码与产物落盘。

测试里所有外部命令（mpv / vspipe / ffmpeg）都走这里，保证
失败时能拿到完整的 argv、stdout、stderr 和耗时。
"""

from __future__ import annotations

import csv
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
    samples: list[dict] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.rc == 0 and not self.timed_out

    @property
    def memory(self) -> dict:
        """峰值内存/显存（MB）；没开采样时是空字典。"""
        return self.extra.get("memory", {})

    def save(self, dst: Path, tag: str = "") -> Path:
        """把 argv/输出/元数据写进 dst，返回本次落盘的元数据文件。"""
        dst.mkdir(parents=True, exist_ok=True)
        stem = f"{tag}." if tag else ""
        (dst / f"{stem}argv.txt").write_text("\n".join(self.argv) + "\n", encoding="utf-8")
        (dst / f"{stem}stdout.log").write_text(self.out, encoding="utf-8", errors="replace")
        (dst / f"{stem}stderr.log").write_text(self.err, encoding="utf-8", errors="replace")
        if self.samples:
            _write_samples(dst / f"{stem}mem.csv", self.samples)
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
    monitor: bool | None = None,
    gpu: bool | None = None,
    interval: float | None = None,
) -> Result:
    """执行命令并完整捕获输出；超时不抛异常，交给调用方断言。

    ``monitor`` 默认跟随 ``VFI_MEM``（默认开）：跑的过程中后台采样**整棵进程树**的
    内存，结果进 ``Result.samples``，``Result.save()`` 会写成 ``mem.csv``。
    用 ``VFI_MEM=0`` 关掉，``VFI_MEM_INTERVAL`` 调采样间隔（秒）。

    ``gpu`` 默认关：按进程读显存要走性能计数器，一次约 1s，只在真的会用 GPU 的
    调用上打开（vspipe 跑引擎、mpv 带 GPU 滤镜）。``VFI_MEM_GPU=1`` 可全局打开。

    按进程树求和是必须的：uv 建出来的 ``.venv\\Scripts\\python.exe`` 是启动器，
    真正的解释器是它的子进程，只盯 Popen 的 pid 会一直读到 5 MB 的空壳。
    """
    from . import memory

    if monitor is None:
        monitor = memory.monitor_enabled()
    if gpu is None:
        gpu = memory.gpu_enabled()
    cwd = Path(cwd or os.getcwd())
    started = time.perf_counter()
    try:
        proc = subprocess.Popen(
            argv,
            cwd=str(cwd),
            env=build_env(env),
            stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except FileNotFoundError as exc:
        return Result(argv, cwd, -998, "", f"[missing] {exc}", time.perf_counter() - started)

    sampler = None
    if monitor:
        sampler = memory.Sampler(
            proc.pid,
            interval=interval or memory.default_interval(),
            gpu=gpu,
        ).start()

    timed_out = False
    try:
        out, err = proc.communicate(input=stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        out, err = proc.communicate()
        err = (err or "") + f"\n[timeout] 超过 {timeout:g}s 未结束"

    summary = sampler.stop() if sampler else None
    extra = {}
    if summary is not None:
        extra["memory"] = summary.as_dict()
    return Result(
        argv,
        cwd,
        -999 if timed_out else proc.returncode,
        out or "",
        err or "",
        time.perf_counter() - started,
        timed_out=timed_out,
        extra=extra,
        samples=summary.rows if summary else [],
    )


def _write_samples(path: Path, rows: list[dict]) -> Path:
    """内存采样序列落盘（列同 memory.Summary.write_csv）。"""
    cols = ["t_s", "procs", "wset_mb", "private_mb", "gpu_dedicated_mb", "gpu_shared_mb"]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return path


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
