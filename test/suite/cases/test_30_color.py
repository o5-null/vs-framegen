"""色彩管线：mpv 是否按输入的矩阵/范围标签做正确的转换。

做法：用 VS 脚本把画面钉成已知 YUV（必须从 ``video_in`` 派生），
再由 mpv 的 ``format`` 滤镜打上矩阵/范围标签，让 VO 转成 RGB 落盘，
最后用 ``metrics.yuv_to_rgb`` 反算比对——不依赖人眼看屏幕。

重要观察：VS 帧属性里的 ``_Matrix``/``_ColorRange`` **不会**参与 mpv 的 VO
转换，标签只认输入/滤镜链（见 test/AGENT.md 的坑表）。所以这里用
``--vf=format=...`` 打标签，而不是在 vpy 里设 props。
"""

from __future__ import annotations

import json

import numpy as np
import pytest

from lib import frames as frameslib, metrics
from lib.util import require

pytestmark = pytest.mark.mpv

SRC = "av://lavfi:color=c=black:s=320x240:r=24:d=2"


def yuv_clip(y: int, cb: int, cr: int, *, fmt: str | None = None) -> str:
    """把每个平面钉成常数。

    必须从 ``video_in`` 派生：脚本输出若与 video_in 无关，mpv-lazy 的 vsscript
    桥会直接崩（退出码 0xC0000409）。
    """
    src = "video_in" if fmt is None else f"core.resize.Bicubic(video_in, format=vs.{fmt})"
    return f"""
c = {src}
c = core.std.Expr(c, ["{y}", "{cb}", "{cr}"])
clip = c
clip.set_output()
"""


def _mean_rgb(case, name, body, *, pre, frames=1):
    r = case.run(name)
    r.vpy(body)
    m = r.mpv(SRC, frames=frames, pre=pre)
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()
    data = m.frames(limit=1)
    require(data.size, f"{name}: 没有落盘帧，检查 frames/ 与 mpv.log")
    # 纯色画面：整帧取均值（8bit /255，16bit /65535）
    px = metrics.to_unit(metrics.rgb(data[0])).reshape(-1, 3)
    return px.mean(axis=0) * 255.0


def test_limited_range_black_and_white(case):
    """limited 的 16/235 必须落到 0/255 附近，否则是范围标记没生效。"""
    pre = ["format=yuv420p:colorlevels=limited"]
    black = _mean_rgb(case, "limited-black", yuv_clip(16, 128, 128), pre=pre)
    white = _mean_rgb(case, "limited-white", yuv_clip(235, 128, 128), pre=pre)

    assert np.abs(black).max() <= 2.0, f"limited 黑应≈0，实测 {black}"
    assert white.min() >= 253.0, f"limited 白应≈255，实测 {white}"


def test_full_range_is_not_stretched(case):
    """full range 的 16 应保持 ≈16：会把它拉到 0 就说明被当 limited 处理了。"""
    pre = ["format=yuv420p:colorlevels=full"]
    y16 = _mean_rgb(case, "full-y16", yuv_clip(16, 128, 128), pre=pre)
    y235 = _mean_rgb(case, "full-y235", yuv_clip(235, 128, 128), pre=pre)

    assert 12.0 <= y16.mean() <= 20.0, f"full 范围 Y=16 应≈16，实测 {y16}"
    assert 231.0 <= y235.mean() <= 239.0, f"full 范围 Y=235 应≈235，实测 {y235}"


def test_matrix_follows_input_tag(case):
    """同一组 YUV，只改输入的矩阵标签就必须换成另一套 RGB——601/709 错乱的根因。

    取值刻意避开通道裁剪：Y=100/Cb=128/Cr=210 时 601 与 709 的 R/G 差 10 以上。
    """
    y, cb, cr = 100, 128, 210
    got = {}
    for name, tag in (("709", "bt.709"), ("601", "bt.601")):
        got[name] = _mean_rgb(
            case, f"matrix-{name}", yuv_clip(y, cb, cr),
            pre=[f"format=yuv420p:colorlevels=limited:colormatrix={tag}"],
        )

    expected = {k: np.array(metrics.yuv_to_rgb(y, cb, cr, matrix=k)) for k in got}
    case.save("matrix.json", json.dumps(
        {k: {"got": [round(float(v), 2) for v in got[k]],
             "expected": [round(float(v), 2) for v in expected[k]]} for k in got},
        ensure_ascii=False, indent=2))

    for key in got:
        err = float(np.abs(got[key] - expected[key]).max())
        assert err <= 6.0, f"{key}: 实测 {got[key]} 与预期 {expected[key]} 差 {err:.1f}"

    spread = float(np.abs(got["709"] - got["601"]).max())
    assert spread > 8.0, f"两种矩阵输出几乎一样（{spread:.1f}），说明输入标签没生效"


def test_10bit_output_keeps_depth(case):
    """10bit PQ 片源要落成 16bit PNG，否则下游质量度量会丢精度。"""
    r = case.run("hdr10")
    r.vpy(yuv_clip(530, 512, 512, fmt="YUV420P10"))
    m = r.mpv(
        SRC, frames=1,
        pre=["format=yuv420p:colorlevels=limited:colormatrix=bt.2020-ncl:"
             "primaries=bt.2020:gamma=pq"],
    )
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()

    files = m.frame_files()
    require(files, "没有落盘帧")
    info = frameslib.read_png_info(files[0])
    assert info.bit_depth == 16, f"PNG 位深 {info.bit_depth}，期望 16"
    assert (info.width, info.height) == (320, 240)

    data = m.frames(limit=1)
    assert data.dtype == np.uint16


def test_script_without_set_output_is_fatal(case):
    """守卫：mpv 对失败的 VS 脚本会静默禁用滤镜并照常播放——必须由日志抓到。"""
    r = case.run("no-output")
    r.vpy("clip = core.std.Expr(video_in, ['16', '128', '128'])")
    m = r.mpv(SRC, frames=1)
    assert m.result.ok, "这条路径 mpv 通常仍返回 0，正因如此才需要日志检查"
    hits = m.fatal()
    assert any("Could not get script output node" in h for h in hits), hits


def test_script_exception_is_fatal(case):
    r = case.run("exception")
    r.vpy("raise RuntimeError('套件自检：脚本异常必须被日志抓到')")
    m = r.mpv(SRC, frames=1)
    assert m.result.ok
    hits = m.fatal()
    assert any("Script evaluation failed" in h for h in hits), hits
