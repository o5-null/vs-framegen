"""mpv 驱动：把「实际显示的帧」写成 PNG，再配上完整日志。

核心套路：
    --vo=image --vo-image-outdir=frames --vo-image-format=png
这样 mpv 走完解码 / vf 链 / VO 色彩转换后逐帧落盘，等于把屏幕内容重定向成文件，
不依赖人眼，也不需要 GPU 显示。日志里 ``[vo/image] converted:`` 一行记录了
VO 实际用的像素格式与色彩标签，是色域/范围问题的第一手证据。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .runner import Result, run

#: 出现即代表被测链路没有真正生效（VS 脚本失败会被 mpv 静默禁用）
FATAL_PATTERNS = (
    "Script evaluation failed",
    "Could not get script output node",
    "Could not get our own input node",
    "Disabling filter",
    "Fatal error",
    "Python exception",
    "Failed to load script",
    "cannot open shared object",
    "Could not open/initialize audio device",  # 静音环境常见，仍视为可疑
)

#: 已知噪声，不算失败
NON_FATAL = (
    "Error parsing option",
    "Protocol not found",
    "Could not open/initialize video output",
)

CONVERTED_RE = re.compile(
    r"converted:\s+(\d+)x(\d+)\s+(\S+)\s+(\S+)/(\S+)/(\S+)/(\S+)/(\S+)"
)
VFPS_RE = re.compile(r"Container reported FPS:\s*([0-9.]+)")


@dataclass
class Converted:
    width: int
    height: int
    pix_fmt: str
    family: str
    primaries: str
    transfer: str
    range: str
    location: str

    @property
    def is_rgb(self) -> bool:
        return self.family == "rgb"


@dataclass
class MpvRun:
    result: Result
    dir: Path
    log: str
    frames_dir: Path
    extra: dict = field(default_factory=dict)

    # ---- 日志视图 ----
    @property
    def log_lines(self) -> list[str]:
        return self.log.splitlines()

    def converted(self) -> list[Converted]:
        out = []
        for line in self.log_lines:
            m = CONVERTED_RE.search(line)
            if m:
                out.append(
                    Converted(
                        int(m.group(1)), int(m.group(2)), m.group(3), m.group(4),
                        m.group(5), m.group(6), m.group(7), m.group(8),
                    )
                )
        return out

    def container_fps(self) -> float | None:
        m = VFPS_RE.search(self.log)
        return float(m.group(1)) if m else None

    def fatal(self) -> list[str]:
        """返回命中的致命行（去掉已知噪声）。"""
        hits = []
        for line in self.log_lines:
            if any(n in line for n in NON_FATAL):
                continue
            if any(p in line for p in FATAL_PATTERNS):
                hits.append(line.strip())
        return hits

    def saved_frames(self) -> list[str]:
        return [ln.split("Saving ", 1)[1].strip() for ln in self.log_lines if "Saving " in ln]

    # ---- 帧视图 ----
    def frame_files(self) -> list[Path]:
        from .frames import list_frames

        return list_frames(self.frames_dir)

    def frames(self, *, limit: int | None = None):
        from .frames import load_sequence

        return load_sequence(self.frames_dir, limit=limit)

    def save(self, dst: Path | None = None) -> None:
        dst = dst or self.dir
        self.result.save(dst)
        (dst / "mpv.log").write_text(self.log, encoding="utf-8", errors="replace")


def run_mpv(
    exe: Path,
    source: str | Path,
    *,
    cwd: Path,
    frames: int = 1,
    start: float | None = None,
    end: float | None = None,
    vf_script: str | Path | None = None,
    pre: list[str] | None = None,
    scale: tuple[int, int] | str | None = None,
    extra: list[str] | None = None,
    config: bool = False,
    timeout: float = 300.0,
    env: dict[str, str] | None = None,
    log_level: str = "all=info,vo/image=v,vapoursynth=debug,vf=debug",
) -> MpvRun:
    """跑一次 mpv 并把帧写到 ``cwd/frames``。

    ``vf_script`` 必须是相对 ``cwd`` 的路径：mpv 的滤镜参数用 ``:`` 分隔，
    绝对路径的盘符会被当成分隔符，反斜杠还会被当转义。
    """
    cwd = Path(cwd)
    cwd.mkdir(parents=True, exist_ok=True)

    chain: list[str] = list(pre or [])
    if vf_script is not None:
        chain.append(f"vapoursynth={Path(vf_script).as_posix()}")
    if scale is not None:
        w, h = _parse_scale(scale)
        chain.append(f"scale={w}:{h}")

    argv = [
        str(exe),
        Path(source).as_posix() if isinstance(source, Path) else str(source),
        "--terminal=no",
        "--vo=image",
        "--vo-image-outdir=frames",
        "--vo-image-format=png",
        "--vo-image-png-compression=0",
        f"--frames={frames}",
        "--ao=null",
        "--no-audio",
        "--idle=no",
        "--keep-open=no",
        "--osc=no",
        "--osd-level=0",
        "--hwdec=no",
        "--log-file=mpv.log",
        f"--msg-level={log_level}",
    ]
    if start is not None:
        argv.append(f"--start={start}")
    if end is not None:
        argv.append(f"--end={end}")
    if chain:
        argv.append("--vf=" + ",".join(chain))
    if not config:
        argv.append("--no-config")
        argv.append("--load-scripts=no")
    argv.extend(extra or [])

    result = run(argv, cwd=cwd, env=env, timeout=timeout)
    log_path = cwd / "mpv.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""

    frames_dir = cwd / "frames"
    out = MpvRun(result=result, dir=cwd, log=log, frames_dir=frames_dir)
    out.extra = {"source": str(source), "frames_requested": frames, "scale": scale}
    out.save()
    return out


def _parse_scale(scale: tuple[int, int] | str) -> tuple[int, int]:
    if isinstance(scale, tuple):
        return scale
    w, _, h = str(scale).lower().partition("x")
    return int(w), int(h)
