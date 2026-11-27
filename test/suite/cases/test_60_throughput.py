"""吞吐与稳定性：长一点、慢一点的跑法。

不算基准（基准在 bench/），只回答两个问题：
- 整条链路在当前机器上还能不能实时；
- 长时间跑会不会崩（配合 VFI_DUMP=1 抓 dump）。
"""

from __future__ import annotations

import json

import pytest

from lib import paths
from lib.util import require

pytestmark = [pytest.mark.mpv, pytest.mark.slow]

FRAMES = 60
SIZE = (640, 360)


def test_identity_throughput(case):
    video = paths.sample_video()
    require(video is not None and video.exists(), "需要 media/sintel_trailer_1080p.mp4（或设 VFI_VIDEO）")

    r = case.run("identity-60f")
    r.vpy("clip = video_in\nclip.set_output()")
    m = r.mpv(video, frames=FRAMES, start=5, scale=SIZE)
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()

    saved = len(m.saved_frames())
    fps = FRAMES / m.result.seconds if m.result.seconds > 0 else 0.0
    report = {
        "frames_requested": FRAMES,
        "frames_saved": saved,
        "seconds": round(m.result.seconds, 3),
        "fps_wall": round(fps, 2),
        "container_fps": m.container_fps(),
        "size": list(SIZE),
    }
    case.save("throughput.json", json.dumps(report, ensure_ascii=False, indent=2))

    assert saved == FRAMES, f"只落盘 {saved}/{FRAMES} 帧"
    # 只作为明显劣化的警报（真基准在 bench/），机器慢时用 --deselect 摘掉
    assert fps >= 10.0, f"identity 链路只有 {fps:.1f} fps，明显异常：{report}"
