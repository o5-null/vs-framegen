"""小工具：需要的依赖缺失时 skip 而不是 error。"""

from __future__ import annotations

from pathlib import Path

import pytest


def require(value, why: str):
    """None/False 时跳过测试，返回原值方便链式使用。"""
    if not value:
        pytest.skip(why)
    return value


def require_file(path: Path | None, why: str) -> Path:
    if not path or not Path(path).exists():
        pytest.skip(why)
    return Path(path)
