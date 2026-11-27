"""mpv 真实显示路径（gpu-next/d3d11）的逐帧截图与软件路径对比。

``--vo=image`` 落的是软件转换后的帧；这里再抓一份「真正送给显示器的帧」
（经 libplacebo 色彩管理、缩放、抖动），用来做色彩/亮度/闪烁这类
原本只能靠人眼判断的检查。
"""

from __future__ import annotations

import pytest

from lib import display, metrics, mpv

pytestmark = [pytest.mark.mpv, pytest.mark.gpu]

#: 纯色源：逐帧内容相同，比较两条 VO 路径时不受帧序影响
FLAT = "av://lavfi:color=c=0x336699:s=320x180:r=24:d=4"
#: 运动源：验证显示路径确实在推进帧，而不是卡在某一帧上
MOTION = "av://lavfi:testsrc2=duration=2:size=320x180:rate=24"


def _means(frames) -> list[float]:
    unit = metrics.to_unit(metrics.rgb(frames))
    return [float(v) for v in unit.reshape(-1, unit.shape[-1]).mean(axis=0)]


def test_gpu_screenshot_writes_high_bit_png(case):
    r = case.run("gpu")
    cap = display.capture_display(case.mpv_exe, FLAT, cwd=r.dir, frames=3)
    assert cap.result.ok, cap.result.tail()
    assert len(cap.shots()) == 3, cap.shots()
    data = cap.frames()
    assert data.shape[1:] == (180, 320, 4), data.shape
    assert data.dtype.itemsize == 2, "gpu-next 截图应为 16bit"
    assert cap.fatal() == [], cap.fatal()


def test_gpu_path_matches_software_path_on_flat_color(case):
    """同一个纯色源，两条 VO 路径的平均色应几乎一致（只差抖动与量化）。"""
    soft_run = case.run("soft")
    soft = mpv.run_mpv(case.mpv_exe, FLAT, cwd=soft_run.dir, frames=3)
    assert soft.result.ok, soft.result.tail()

    gpu_run = case.run("gpu")
    cap = display.capture_display(case.mpv_exe, FLAT, cwd=gpu_run.dir, frames=3)
    assert cap.result.ok, cap.result.tail()
    assert cap.fatal() == [], cap.fatal()

    gpu_mean = _means(cap.frames())
    soft_mean = _means(soft.frames())
    diff = [abs(a - b) for a, b in zip(gpu_mean, soft_mean)]
    assert max(diff) < 0.01, f"gpu={gpu_mean} soft={soft_mean}"


def test_gpu_capture_advances_frames(case):
    """显示路径逐帧截图应得到不同画面（防「冻结在首帧」类回归）。"""
    r = case.run("gpu-motion")
    cap = display.capture_display(case.mpv_exe, MOTION, cwd=r.dir, frames=3)
    assert cap.result.ok, cap.result.tail()
    data = cap.frames()
    assert len(data) == 3
    deltas = metrics.deltas(metrics.rgb(data))
    assert float(max(deltas)) > 0.001, deltas
