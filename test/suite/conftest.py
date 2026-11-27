"""pytest 配置：产物目录、外部可执行文件、用例句柄。"""

from __future__ import annotations

import datetime
from pathlib import Path

import pytest

from lib import paths
from lib.case import Case, _slug


@pytest.fixture(scope="session")
def artifacts() -> Path:
    """本次运行的产物根目录：test/artifacts/<时间戳>/。"""
    root = paths.artifacts_root() / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True, exist_ok=True)
    return root


@pytest.fixture(scope="session")
def mpv_exe() -> Path | None:
    return paths.mpv()


@pytest.fixture(scope="session")
def vspipe_exe() -> Path | None:
    return paths.vspipe()


@pytest.fixture(scope="session")
def stage() -> Path | None:
    """mpv-playkit-pre 的 stage（含 mpv.exe + portable_config），没有就 None。"""
    return paths.playkit_stage()


@pytest.fixture(scope="session")
def media() -> Path | None:
    return paths.sample_video()


@pytest.fixture
def case(request, artifacts: Path) -> Case:
    """一条用例专属的目录 + 运行句柄。"""
    return Case(
        artifacts / _slug(request.node.nodeid),
        mpv_exe=paths.mpv(),
        vspipe_exe=paths.vspipe(),
    )
