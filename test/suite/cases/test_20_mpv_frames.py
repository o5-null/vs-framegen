"""mpv 逐帧落盘：确认「屏幕内容」真的被重定向成文件。

这里只用真片源 + 可选 identity 脚本，不碰插件，属于最底层的回归网。
"""

from __future__ import annotations

import numpy as np
import pytest

from lib import frames as frameslib, paths
from lib.util import require

pytestmark = pytest.mark.mpv

IDENTITY = """
clip = video_in
clip.set_output()
"""


def _media():
    video = paths.sample_video()
    require(video is not None and video.exists(), "需要 media/sintel_trailer_1080p.mp4（或设 VFI_VIDEO）")
    return video


def test_frames_are_written(case):
    video = _media()
    r = case.run("plain")
    m = r.mpv(video, frames=3, scale=(160, 90))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()

    files = m.frame_files()
    assert [f.name for f in files] == ["00000001.png", "00000002.png", "00000003.png"]

    info = frameslib.read_png_info(files[0])
    assert (info.width, info.height) == (160, 90)
    assert info.kind == "rgb"


def test_container_fps(case):
    video = _media()
    r = case.run("fps")
    m = r.mpv(video, frames=1, scale=(64, 36))
    assert m.result.ok, m.result.tail()
    assert m.container_fps() == pytest.approx(24.0, abs=0.01)


def test_vo_reports_rgb_conversion(case):
    """VO 的 ``converted:`` 行是色域/范围问题的第一手证据，必须能解析到。"""
    video = _media()
    r = case.run("converted")
    m = r.mpv(video, frames=1, scale=(64, 36))
    conv = m.converted()
    require(conv, "mpv 日志里没有 [vo/image] converted，检查 --msg-level")
    assert conv[-1].is_rgb
    assert conv[-1].range in ("full", "limited")
    r.write("converted.json", str(conv))


def test_identity_vapoursynth_matches_plain(case):
    """identity 脚本不能让画面变化：对比同一帧的无滤镜输出。"""
    video = _media()
    plain = case.run("plain").mpv(video, frames=1, start=5, scale=(160, 90))
    assert plain.result.ok and not plain.fatal(), plain.fatal()

    run = case.run("vf")
    run.vpy(IDENTITY)
    vf = run.mpv(video, frames=1, start=5, scale=(160, 90))
    assert vf.result.ok, vf.result.tail()
    assert not vf.fatal(), vf.fatal()

    # 脚本确实执行过（否则 mpv 会静默禁用滤镜，见 fatal() 的注释）
    assert "New message handler installed" in run.vs_log()

    a = plain.frames()[0].astype(np.int16)
    b = vf.frames()[0].astype(np.int16)
    assert a.shape == b.shape
    worst = int(np.abs(a - b).max())
    assert worst <= 1, f"identity 路径最大偏差 {worst}，说明多过一次色彩转换或其他处理"
