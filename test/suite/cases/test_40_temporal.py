"""时间轴：重复帧 / 黑帧 / 亮度抽动。

补帧与插值链路的典型故障是「帧数够了但内容是错的」：
重复帧、错位取帧、亮度泵动。这些在 PNG 序列上都能数值化。
"""

from __future__ import annotations

import json

import pytest

from lib import metrics
from lib.util import require

pytestmark = pytest.mark.mpv

TESTSRC = "av://lavfi:testsrc2=duration=2:size=128x72:rate=24"
BLACK = "av://lavfi:color=c=black:s=128x72:r=24:d=2"


def test_motion_has_no_duplicates(case):
    r = case.run("testsrc2")
    m = r.mpv(TESTSRC, frames=12, scale=(128, 72))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()

    data = m.frames()
    require(data.shape[0] == 12, f"期望 12 帧，实际 {data.shape[0]}")

    dup = metrics.duplicate_frames(data)
    assert dup == [], f"运动画面出现重复帧：{dup}"
    assert metrics.black_frames(data) == []
    # testsrc2 变化平缓，用一阶差分而不是亮度极差来确认「画面在动」
    rep = metrics.flicker_report(data)
    assert rep["median_delta"] > 0.0005, rep

    case.save("summary.json", json.dumps(metrics.summarize(data), ensure_ascii=False, indent=2))


def test_black_source_is_detected(case):
    r = case.run("black")
    m = r.mpv(BLACK, frames=4, scale=(128, 72))
    assert m.result.ok, m.result.tail()
    data = m.frames()
    require(data.shape[0] == 4, f"期望 4 帧，实际 {data.shape[0]}")
    assert metrics.black_frames(data) == [0, 1, 2, 3]


def test_frozen_frames_are_detected(case):
    """负例：把真实画面冻住，重复帧检测必须报出来。

    注意 mpv 会按**输入**的时间轴请求帧，所以 VS 脚本的帧数不能比输入短，
    否则就是 "Invalid frame number N requested"；这里循环到远多于片源长度。
    """
    r = case.run("freeze")
    r.vpy("""
clip = core.std.Loop(video_in[0:1], 400)
clip.set_output()
""")
    m = r.mpv(TESTSRC, frames=4, scale=(128, 72))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()
    data = m.frames()
    require(data.shape[0] == 4, f"期望 4 帧，实际 {data.shape[0]}")
    assert metrics.duplicate_frames(data) == [1, 2, 3]


def test_static_color_is_stable(case):
    """静止画面不应有亮度抽动，也不应被判成黑帧。"""
    r = case.run("static")
    m = r.mpv("av://lavfi:color=c=0x336699:s=128x72:r=24:d=2", frames=6, scale=(128, 72))
    assert m.result.ok, m.result.tail()
    data = m.frames()
    rep = metrics.flicker_report(data)
    assert rep["range"] < 0.01, rep
    assert metrics.black_frames(data) == []


def test_real_clip_advances(case):
    """真片源抽样：帧在推进，没有黑帧、没有卡住。"""
    from lib import paths

    video = paths.sample_video()
    require(video is not None and video.exists(), "需要 media/sintel_trailer_1080p.mp4（或设 VFI_VIDEO）")

    r = case.run("sintel")
    m = r.mpv(video, frames=6, start=20, scale=(192, 108))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()
    data = m.frames()
    require(data.shape[0] == 6, f"期望 6 帧，实际 {data.shape[0]}")
    assert metrics.duplicate_frames(data) == []
    assert metrics.black_frames(data) == []
