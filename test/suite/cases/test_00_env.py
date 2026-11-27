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


def test_process_memory_sampler(tmp_path):
    """采样器要能拿到工作集/私有提交，并落成 mem.csv（调试现场靠它判断租约/泄漏）。"""
    from lib import memory

    alloc = (
        "import time\n"
        "b = bytearray(80 * 1024 * 1024)\n"
        "for i in range(0, len(b), 4096): b[i] = 1\n"
        "time.sleep(0.9)\n"
    )
    res = run([sys.executable, "-c", alloc], timeout=120, interval=0.1)
    assert res.ok, res.tail()
    mem = res.memory
    assert mem.get("peak_wset_mb", 0) >= 60, mem          # 80MB 的块必须体现在工作集里
    assert mem.get("peak_private_mb", 0) >= 60, mem
    assert mem.get("samples", 0) >= 3, mem                # 0.9s / 0.1s ≈ 9 次
    res.save(tmp_path)
    csv_path = tmp_path / "mem.csv"
    assert csv_path.exists() and csv_path.stat().st_size > 64
    assert memory.process_memory(0) is None               # 非法 pid 不能抛异常


def test_gpu_memory_source():
    """显存统计要有来源：Windows 的 GPU 性能计数器，或 nvidia-smi。都没有就 skip。"""
    from lib import memory

    table = memory.gpu_process_memory()
    if not table:
        pytest.skip("没有可用的按进程显存统计（性能计数器被禁用/无 GPU）")
    pid, mem = next(iter(table.items()))
    assert isinstance(pid, int) and pid > 0
    assert mem["dedicated"] >= 0 and mem["shared"] >= 0
