"""仓库/工具/数据的路径解析。

所有路径都从本文件位置推导，或由 ``VFI_*`` 环境变量覆盖，
不在代码里写死 ``E:\\code\\dlssg-vfi``。
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

# test/suite/lib/paths.py -> test/suite -> test -> <repo root>
ROOT = Path(__file__).resolve().parents[3]

#: 缓存，避免每次 fixture 都去扫目录
_cache: dict[str, Path | None] = {}


def env_dir(name: str, default: Path | None) -> Path | None:
    raw = os.environ.get(name)
    if raw:
        p = Path(raw).expanduser()
        return p if p.exists() else default
    return default


def repo_root() -> Path:
    return env_dir("VFI_ROOT", ROOT) or ROOT


def venv_python() -> Path | None:
    """仓库虚拟环境解释器（uv 管理）。"""
    if "exe" not in _cache:
        cand = repo_root() / ".venv" / "Scripts" / "python.exe"
        _cache["exe"] = cand if cand.exists() else None
    return _cache["exe"]


def vspipe() -> Path | None:
    """优先用 .venv 里的 vspipe，保证与本仓库 R74 一致。"""
    if "vspipe" not in _cache:
        cand = []
        if venv_python():
            cand.append(venv_python().parent / "vspipe.exe")
        cand.append(repo_root() / "测试环境" / "mpv-lazy" / "VSPipe.exe")
        _cache["vspipe"] = next((p for p in cand if p.exists()), None)
    return _cache["vspipe"]


def ffmpeg() -> str | None:
    """帧序列解码靠 ffmpeg，可执行文件走 PATH 或 VFI_FFMPEG。"""
    if "ffmpeg" not in _cache:
        raw = os.environ.get("VFI_FFMPEG")
        if raw and Path(raw).exists():
            _cache["ffmpeg"] = Path(raw)
        else:
            found = shutil.which("ffmpeg")
            _cache["ffmpeg"] = Path(found) if found else None
    return str(_cache["ffmpeg"]) if _cache["ffmpeg"] else None


def playkit_stage() -> Path | None:
    """mpv-playkit-pre 的 stage 目录（含 mpv.exe + portable_config）。

    查找顺序：``VFI_PLAYKIT`` → ``mpv-playkit/dist/stage/*`` → ``repos/dist/mpv-playkit-pre-*``。
    取最新修改的那个，方便刚构建完就测。
    """
    if "playkit" in _cache:
        return _cache["playkit"]

    candidates: list[Path] = []
    raw = os.environ.get("VFI_PLAYKIT")
    if raw:
        candidates.append(Path(raw).expanduser())

    for pattern in ("mpv-playkit/dist/stage/*", "repos/dist/mpv-playkit-pre-*"):
        candidates.extend(
            p for p in sorted(repo_root().glob(pattern)) if p.is_dir() and (p / "mpv.exe").exists()
        )

    live = [p for p in candidates if (p / "mpv.exe").exists()]
    live.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    _cache["playkit"] = live[0] if live else None
    return _cache["playkit"]


def mpv() -> Path | None:
    """默认 mpv：``VFI_MPV`` → 测试环境\\mpv-lazy → 目录下载解压版。"""
    if "mpv" in _cache:
        return _cache["mpv"]

    cand: list[Path] = []
    raw = os.environ.get("VFI_MPV")
    if raw:
        cand.append(Path(raw).expanduser())
    cand.append(repo_root() / "测试环境" / "mpv-lazy" / "mpv.exe")
    if playkit_stage():
        cand.append(playkit_stage() / "mpv.exe")

    found = next((p for p in cand if p.exists()), None)
    _cache["mpv"] = found
    return found


def k7sfunc_src() -> Path | None:
    """k7sfunc 包的**可导入目录**（其下应有 ``k7sfunc/__init__.py``）。

    查找顺序：``VFI_K7`` → ``k7sfunc/src`` → stage 的 ``Lib/site-packages``。
    ``.venv`` 里没装 k7sfunc，所以无头 vspipe 脚本得自己把它挂进 ``sys.path``。
    """
    if "k7" in _cache:
        return _cache["k7"]

    cand: list[Path] = []
    raw = os.environ.get("VFI_K7")
    if raw:
        cand.append(Path(raw).expanduser())
    cand.append(repo_root() / "k7sfunc" / "src")
    if playkit_stage():
        cand.append(playkit_stage() / "Lib" / "site-packages")

    found = next((p for p in cand if (p / "k7sfunc" / "__init__.py").exists()), None)
    _cache["k7"] = found
    return found


def sample_video() -> Path | None:
    """默认测试片：``VFI_VIDEO`` → media\\sintel_trailer_1080p.mp4。"""
    raw = os.environ.get("VFI_VIDEO")
    cand = [Path(raw).expanduser()] if raw else []
    cand.append(repo_root() / "media" / "sintel_trailer_1080p.mp4")
    return next((p for p in cand if p.exists()), None)


def data_dir() -> Path:
    """真实素材目录（gitignore，不保证存在）。"""
    return env_dir("VFI_DATA", repo_root() / "测试数据") or repo_root()


def artifacts_root() -> Path:
    """测试产物根目录（gitignore）。``VFI_ARTIFACTS`` 可覆盖。"""
    return env_dir("VFI_ARTIFACTS", repo_root() / "test" / "artifacts") or repo_root() / "test" / "artifacts"
