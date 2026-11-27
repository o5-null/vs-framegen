"""抓「mpv 真实显示路径」的帧：IPC 驱动 + 逐帧截图。

与 ``lib/mpv.py`` 的 ``--vo=image`` 相比：
    软件 VO（image）   —— 只经过 VO 的软件色彩转换，看不到 libplacebo 的
                          色彩管理 / 缩放算法 / 抖动，GPU 侧问题会漏掉；
    真实显示（gpu-next）—— 截图取的是 VO 渲染结果（d3d11 swapchain 回读），
                          能反映实际屏幕观感。

驱动方式：``--pause=yes --keep-open=yes --idle=yes`` 停住播放，用 IPC 发
``frame-step`` 前进一帧，再发 ``screenshot-to-file``。截图参数固定
``--screenshot-sw=no --screenshot-format=png --screenshot-high-bit-depth=yes``，
gpu-next 下会得到 16bit PNG（8bit 源也会被升位，便于做数值比较）。
"""

from __future__ import annotations

import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import ipc as ipclib
from . import memory
from .runner import Result, build_env


@dataclass
class DisplayCapture:
    """一次显示路径截帧的产物。"""

    result: Result
    dir: Path
    log: str
    shots_dir: Path
    extra: dict = field(default_factory=dict)

    # ---- 帧视图 ----
    def shots(self) -> list[Path]:
        return sorted(self.shots_dir.glob("*.png"))

    def frames(self, *, limit: int | None = None):
        from .frames import load_sequence

        return load_sequence(self.shots_dir, limit=limit)

    @property
    def ok(self) -> bool:
        return self.result.ok

    @property
    def memory(self) -> dict:
        return self.result.memory

    def fatal(self) -> list[str]:
        from .mpv import FATAL_PATTERNS, NON_FATAL

        hits = []
        for line in self.log.splitlines():
            if any(n in line for n in NON_FATAL):
                continue
            if any(p in line for p in FATAL_PATTERNS):
                hits.append(line.strip())
        return hits

    def save(self, dst: Path | None = None) -> None:
        dst = dst or self.dir
        self.result.save(dst)
        (dst / "mpv.log").write_text(self.log, encoding="utf-8", errors="replace")


def capture_display(
    exe: Path,
    source: str | Path,
    *,
    cwd: Path,
    frames: int = 3,
    vo: str = "gpu-next",
    gpu_api: str = "d3d11",
    vf_script: str | Path | None = None,
    pre: list[str] | None = None,
    scale: tuple[int, int] | str | None = None,
    extra: list[str] | None = None,
    config: bool = False,
    start: float | None = None,
    timeout: float = 180.0,
    step_timeout: float = 20.0,
    env: dict[str, str] | None = None,
    gpu: bool | None = None,
    probe: list[str] | None = None,
    log_level: str = "all=info,vf=v,vapoursynth=debug",
) -> DisplayCapture:
    """启动 mpv（显示 VO）→ 逐帧截图 → 退出，全过程带内存采样。

    ``probe`` 里的属性名会在每帧截图后各查一次，结果按帧存进
    ``extra["props"]``（用来看 ``vf-metadata/...`` 这类只有播放中才能读到的值）。
    """
    cwd = Path(cwd)
    cwd.mkdir(parents=True, exist_ok=True)
    shots = cwd / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    for old in shots.glob("*.png"):
        old.unlink()

    chain: list[str] = list(pre or [])
    if vf_script is not None:
        chain.append(f"vapoursynth={Path(vf_script).as_posix()}")
    if scale is not None:
        w, h = _parse_scale(scale)
        chain.append(f"scale={w}:{h}")

    name = ipclib.pipe_name(cwd)
    argv = [
        str(exe),
        Path(source).as_posix() if isinstance(source, Path) else str(source),
        "--terminal=no",
        f"--vo={vo}",
        "--ao=null",
        "--no-audio",
        "--pause=yes",
        "--keep-open=yes",
        "--idle=yes",
        "--osc=no",
        "--osd-level=0",
        "--hwdec=no",
        f"--input-ipc-server=\\\\.\\pipe\\{name}",
        f"--screenshot-directory={shots.as_posix()}",
        "--screenshot-format=png",
        "--screenshot-high-bit-depth=yes",
        "--screenshot-sw=no",
        "--log-file=mpv.log",
        f"--msg-level={log_level}",
    ]
    if gpu_api and vo in ("gpu", "gpu-next"):
        argv.append(f"--gpu-api={gpu_api}")
    if start is not None:
        argv.append(f"--start={start}")
    if chain:
        argv.append("--vf=" + ",".join(chain))
    if not config:
        argv += ["--no-config", "--load-scripts=no"]
    argv.extend(extra or [])

    started = time.perf_counter()
    proc = subprocess.Popen(
        argv,
        cwd=str(cwd),
        env=build_env(env),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    sampler = None
    if memory.monitor_enabled():
        sampler = memory.Sampler(
            proc.pid,
            interval=memory.default_interval(),
            gpu=bool(gpu) if gpu is not None else memory.gpu_enabled(),
        ).start()

    replies: list[dict] = []
    props: list[dict] = []
    failure = ""
    try:
        with ipclib.MpvIpc.connect(name, timeout=step_timeout, alive=lambda: proc.poll() is None) as conn:
            _wait_loaded(conn, step_timeout)
            for idx in range(frames):
                conn.command("frame-step", timeout=step_timeout)
                target = f"shots/{idx + 1:08d}.png"
                reply = _shot_with_retry(conn, target, step_timeout)
                replies.append(reply)
                if probe:
                    snap = {}
                    for prop in probe:
                        try:
                            snap[prop] = conn.get_property(prop, timeout=step_timeout)
                        except ipclib.IpcError as exc:
                            snap[prop] = f"[error] {exc}"
                    props.append(snap)
            conn.command("quit", timeout=5.0)
    except ipclib.IpcError as exc:
        failure = str(exc)
        proc.kill()

    timed_out = False
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        out, err = proc.communicate()
        err = (err or "") + f"\n[timeout] 超过 {timeout:g}s 未结束"

    summary = sampler.stop() if sampler else None
    result_extra: dict = {}
    if summary is not None:
        result_extra["memory"] = summary.as_dict()
    log_path = cwd / "mpv.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    if failure:
        err = (err or "") + f"\n[ipc] {failure}"
    result = Result(
        argv,
        cwd,
        -999 if timed_out else proc.returncode,
        out or "",
        err or "",
        time.perf_counter() - started,
        timed_out=timed_out,
        extra=result_extra,
        samples=summary.rows if summary else [],
    )
    cap = DisplayCapture(result=result, dir=cwd, log=log, shots_dir=shots)
    cap.extra = {
        "source": str(source),
        "frames_requested": frames,
        "vo": vo,
        "pipe": name,
        "screenshots": [p.name for p in cap.shots()],
        "ipc_error": failure,
    }
    if props:
        cap.extra["props"] = props
        result.extra["props"] = props
    cap.save()
    return cap


def _safe_prop(conn, name: str):
    try:
        return conn.get_property(name)
    except ipclib.IpcError:
        return None


def _wait_loaded(conn, timeout: float) -> None:
    """等 mpv 把文件真正打开后再发 ``frame-step``。

    ``--pause=yes`` 下 mpv 仍要先解出首帧才会接受逐帧命令；在加载完成前发
    ``frame-step`` 会得到 ``error running command``，整次截帧就白跑了。
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        width = _safe_prop(conn, "video-params/w")
        if isinstance(width, int) and width > 0:
            return
        time.sleep(0.02)


def _shot_with_retry(conn, target: str, timeout: float) -> dict:
    """截图取的是「已渲染的帧」，``frame-step`` 返回时画面往往还没出来。

    直接重试比轮询帧号可靠：不是所有 VO 都暴露 ``estimated-frame-number``。
    """
    deadline = time.monotonic() + timeout
    while True:
        try:
            return conn.command("screenshot-to-file", target, "video", timeout=timeout)
        except ipclib.IpcError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def _parse_scale(scale: tuple[int, int] | str) -> tuple[int, int]:
    if isinstance(scale, tuple):
        return scale
    w, _, h = str(scale).lower().partition("x")
    return int(w), int(h)
