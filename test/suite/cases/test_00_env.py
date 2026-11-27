"""环境自检：先确认「能测」，再谈「测得对」。

这一组不跑被测链路，只回答：解释器/依赖/mpv/vspipe/ffmpeg/素材/stage 在不在，
以及 mpv 这个构建到底有没有编进 vapoursynth。缺任何一项，后面的用例 会跳过而不是报错。
"""

from __future__ import annotations

import sys

import pytest

from lib import paths
from lib.runner import run
from lib.util import require

pytestmark = pytest.mark.env


@pytest.fixture(scope="module")
def mpv_version(mpv_exe) -> str:
    exe = require(mpv_exe, "找不到 mpv.exe（设 VFI_MPV 或放 测试环境\\mpv-lazy）")
    res = run([str(exe), "--version"], timeout=60)
    assert res.ok, res.tail()
    return res.out + res.err


@pytest.fixture(scope="module")
def mpv_filters(mpv_exe) -> str:
    """``--vf=help`` 列出的滤镜表：--version 不带构建选项，只能这样确认 VS 桥在不在。"""
    exe = require(mpv_exe, "找不到 mpv.exe（设 VFI_MPV 或放 测试环境\\mpv-lazy）")
    res = run([str(exe), "--no-config", "--vf=help"], timeout=60)
    assert res.ok, res.tail()
    return res.out + res.err


def test_python_deps():
    import numpy
    import pytest as pt

    assert sys.version_info >= (3, 10), sys.version
    assert numpy.__version__
    assert int(pt.__version__.split(".")[0]) >= 7


def test_vapoursynth_binding():
    vs = pytest.importorskip("vapoursynth")
    api = getattr(vs, "__api_version__", None)
    major = getattr(api, "api_major", getattr(api, "value", 4))
    assert int(major) >= 4, f"VS API {api}：R82 起拒绝加载 API3 插件（akarin 等）"
    # 套件的 vs.log 采集依赖这个 API
    assert hasattr(vs.core, "add_log_handler"), "VS 缺少 add_log_handler"
    assert hasattr(vs.core, "std")


def test_mpv_build(mpv_version: str, mpv_filters: str):
    assert "mpv v" in mpv_version
    # 没编进 vapoursynth 的 mpv 会让所有 vpy 用例变成假通过
    assert "vapoursynth" in mpv_filters.lower(), "该 mpv 没有 vapoursynth 滤镜"


def test_vspipe(vspipe_exe):
    exe = require(vspipe_exe, "找不到 vspipe.exe")
    res = run([str(exe), "--version"], timeout=60)
    assert res.ok, res.tail()
    assert "Core" in (res.out + res.err) or "API" in (res.out + res.err)


def test_ffmpeg():
    exe = require(paths.ffmpeg(), "找不到 ffmpeg（装到 PATH 或设 VFI_FFMPEG）")
    res = run([exe, "-version"], timeout=60)
    assert res.ok, res.tail()
    assert "ffmpeg version" in res.out


def test_sample_media(media):
    path = require(media, "没有默认测试片（放 media\\sintel_trailer_1080p.mp4 或设 VFI_VIDEO）")
    assert path.stat().st_size > 1 << 20


def test_playkit_stage(stage):
    """stage 不是必需项；存在时要能自洽（mpv.exe + portable_config + VS 插件目录）。"""
    if stage is None:
        pytest.skip("没有 mpv-playkit-pre stage（可设 VFI_PLAYKIT）")
    assert (stage / "mpv.exe").exists()
    assert (stage / "portable_config").is_dir()
    assert (stage / "Lib" / "site-packages" / "vapoursynth" / "plugins").is_dir()
