"""``vf_playkit`` 生成的中间帧必须和源帧同范围、同原色（抓「亮度闪动」）。

现象：6x 补帧播放时原生帧与生成帧亮度不一致，整帧明暗交替。
根因在滤镜的 VideoProcessor 段：BT.2020 素材被声明成
``RGB_FULL_G22_NONE_P709 -> YCbCr studio P2020``，NVIDIA 驱动对 P2020 的
YCbCr 输出不按 studio 范围压缩，引擎给的 full-range RGBA 就原样落地，
生成帧成为 0..231（源是 16..235），每张生成帧都比源帧「拉亮」一截。

修复后（矩阵 BT.2020 且源为受限范围）滤镜改走两段式 Blt：先把 RGB 按
full→studio 压回 16..235，再转 YCbCr。这组用例同时盯两处证据：

* 滤镜自报的 ``dst`` 亮度统计（VideoProcessor 真正写进卡的像素范围）；
* 真实显示路径的截图（IPC 逐帧截屏，帧间差就是人眼看到的闪）。
"""

from __future__ import annotations

import numpy as np
import pytest

from lib import display, metrics, paths, playkit

pytestmark = [pytest.mark.mpv, pytest.mark.gpu]

#: 受限范围的合法码值（留少量容差给量化/抖动）
LO, HI = 16, 235
TOL = 6


@pytest.fixture(scope="module")
def env():
    """开发构建 + 桥接 DLL 都在才跑；缺一个就整体 skip。"""
    exe = paths.dev_mpv()
    if exe is None:
        pytest.skip("没有开发构建 mpv（work/mpvdev/mpv.exe 或 VFI_MPV_DEV）")
    bridge = playkit.bridge_dir(exe)
    if bridge is None:
        pytest.skip("找不到 vdlssg.dll / dlss45（原生滤镜桥接）")
    return exe, bridge


def _run(case, env, clip: str, *, frames: int = 2, name: str = "playkit"):
    exe, bridge = env
    r = case.run(name)
    cap = display.capture_display(
        exe,
        clip,
        cwd=r.dir,
        frames=frames,
        pre=[playkit.vf_chain(exe, bridge)],
        extra=["--hwdec=d3d11va", "--gpu-context=d3d11"],
        env={"PLAYKIT_DSTSTAT": "1"},
        log_level="all=info,playkit=trace,vf=trace",
        timeout=120,
    )
    report = playkit.parse_log(cap.log)
    reason = report.skip_reason()
    if reason:
        pytest.skip(f"playkit 未运行: {reason}")
    assert report.dst, f"滤镜没有回读生成帧，日志尾部：\n{cap.result.tail()}"
    return cap, report


def _assert_limited(stat, what: str) -> None:
    lo = min(s.lo for s in stat)
    hi = max(s.hi for s in stat)
    assert lo >= LO - TOL, f"{what} 黑端被拉伸到 {lo}（应 ≥ {LO}）"
    assert hi <= HI + TOL, f"{what} 白端被拉伸到 {hi}（应 ≤ {HI}）"


def test_bt2020_generated_frames_keep_limited_range(case, env):
    """BT.2020 SDR 源：生成帧必须留在 16..235，且亮度贴近源帧。"""
    clip = case.base / "bt2020_ramp.mp4"
    made = playkit.make_clip(clip, hdr=False)
    assert made.ok, made.tail()

    cap, report = _run(case, env, str(clip), name="bt2020")
    assert report.colours is not None, cap.log[-2000:]
    matrix, full, sys, levels = report.colours
    assert matrix == 9, f"BT.2020 应选 2020 矩阵，实际 {report.colours}"

    _assert_limited(report.dst, "生成帧")
    src_mean = float(np.mean([s.y for s in report.src]))
    dst_mean = float(np.mean([s.y for s in report.dst]))
    assert abs(dst_mean - src_mean) <= 3.0, f"src={src_mean:.2f} dst={dst_mean:.2f}"


def test_hdr10_generated_frames_use_pq_and_limited_range(case, env):
    """HDR10(PQ) 源：走 G2084 色彩空间，范围同样不能被拉伸。"""
    clip = case.base / "hdr10_ramp.mp4"
    made = playkit.make_clip(clip, hdr=True)
    assert made.ok, made.tail()

    cap, report = _run(case, env, str(clip), name="hdr10")
    assert report.colours is not None, cap.log[-2000:]
    matrix, full, sys, levels = report.colours
    assert matrix == 9 and levels == 1, f"HDR10 limited 标签识别错误: {report.colours}"
    assert report.cs == (0xC, 0xD), f"PQ 应走 RGB_FULL_G2084_P2020 → YCBCR_STUDIO_G2084_P2020，实际 {report.cs}"

    _assert_limited(report.dst, "生成帧")
    src_mean = float(np.mean([s.y for s in report.src]))
    dst_mean = float(np.mean([s.y for s in report.dst]))
    assert abs(dst_mean - src_mean) <= 3.0, f"src={src_mean:.2f} dst={dst_mean:.2f}"


def test_generated_frames_keep_neutral_chroma_bt709(case, env):
    """BT.709 对照：同一段静止灰阶，生成帧色度必须保持中性（≈128）。

    灰阶片没有颜色，任何色度偏离都是转换环节自己引入的偏差；709 路径是
    已知正确的基线，用它保证下面这条 HDR 断言不是空转。
    """
    clip = case.base / "bt709_ramp.mp4"
    made = playkit.make_clip(clip, hdr=False, matrix="bt709", primaries="bt709")
    assert made.ok, made.tail()

    cap, report = _run(case, env, str(clip), name="bt709_chroma")
    chroma = report.chroma_of("dst")
    assert chroma, f"滤镜没有回读生成帧色度，日志尾部：\n{cap.result.tail()}"
    for c in chroma:
        assert abs(c.cb - 128) <= 2 and abs(c.cr - 128) <= 2, (
            f"生成帧色度偏移 cb={c.cb:.2f} cr={c.cr:.2f}（中性灰应≈128）"
        )


def test_generated_frames_keep_neutral_chroma_hdr(case, env):
    """HDR10 源：生成帧的 Cb/Cr 同样必须中性（≈128）。

    实测缺陷：驱动对 BT.2020 的 YCbCr 输出既不做 studio 范围压缩，又把
    中性灰写成 cb/cr≈115——生成帧相对源帧偏绿偏暗，6x 补帧时交替出现即
    「画面闪」。范围断言抓不到这一半，这里直接盯色度。
    """
    clip = case.base / "hdr10_chroma.mp4"
    made = playkit.make_clip(clip, hdr=True)
    assert made.ok, made.tail()

    cap, report = _run(case, env, str(clip), name="hdr10_chroma")
    chroma = report.chroma_of("dst")
    assert chroma, f"滤镜没有回读生成帧色度，日志尾部：\n{cap.result.tail()}"
    for c in chroma:
        assert abs(c.cb - 128) <= 2 and abs(c.cr - 128) <= 2, (
            f"生成帧色度偏移 cb={c.cb:.2f} cr={c.cr:.2f}（中性灰应≈128）"
        )


def test_generated_frames_do_not_flicker_on_display(case, env):
    """真实显示路径：静止素材补帧后，相邻帧画面差应接近 0。

    范围不一致时源帧/生成帧交替出现，帧间差会被顶起来——这就是人眼看到的闪。
    """
    clip = case.base / "hdr10_static.mp4"
    made = playkit.make_clip(clip, hdr=True)
    assert made.ok, made.tail()

    cap, report = _run(case, env, str(clip), frames=4, name="display")
    data = cap.frames()
    assert len(data) == 4, data.shape

    deltas = metrics.deltas(metrics.rgb(data))
    worst = float(deltas.max())
    assert worst < 0.004, f"补帧后帧间差过大（闪）：{deltas}"
