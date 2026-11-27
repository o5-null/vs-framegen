"""vdlssg 插件的端到端行为：帧数/帧率翻倍、时长属性、经 mpv 链路可用。

这些用例真的会初始化 NGX（DLSS-G），需要 NVIDIA GPU 与引擎包里的运行库，
所以一律带 ``gpu`` 标记；引擎包缺失或驱动初始化失败时跳过而不是判失败。
"""

from __future__ import annotations

import pytest

from lib import paths, vpy
from lib.util import require

pytestmark = [pytest.mark.vs, pytest.mark.gpu]

PLUGIN = paths.repo_root() / "vs-dlssg" / "bin" / "vdlssg.dll"
ENGINE = paths.repo_root() / "vs-dlssg" / "bin" / "dlss45"

# 5 帧 24fps 的 RGBS 片源；帧数是判断 multi 的依据
SOURCE = """
clip = core.std.BlankClip(width=320, height=240, format=vs.RGBS,
                          length=5, fpsnum=24, fpsden=1, color=[0.2, 0.4, 0.6])
"""

BODY = SOURCE + """
core.std.LoadPlugin(r"{plugin}")
clip = core.vdlssg.FrameGen(clip, proxy_dir=r"{engine}", multi=2, log=0)
clip.set_output()
""".format(plugin=PLUGIN.as_posix(), engine=ENGINE.as_posix())

# 这些字样出现在失败输出里，说明是环境问题（缺运行库/无 GPU），不该判为产品缺陷
SKIP_MARKERS = ("proxy version.dll not found", "NGX", "CUDA", "D3D12", "adapter")


def _needs_engine():
    require(PLUGIN.is_file(), f"缺少插件 {PLUGIN}")
    require(ENGINE.is_dir(), f"缺少引擎包 {ENGINE}")


def _check_or_skip(run, info: bool = False):
    text = run.out + run.err
    if run.ok:
        return vpy.parse_info(text) if info else text
    if any(m.lower() in text.lower() for m in SKIP_MARKERS):
        pytest.skip(f"NGX/引擎不可用，跳过：{run.tail(6)}")
    raise AssertionError(f"vspipe 失败：{run.tail()}")


def test_frame_gen_doubles_frames_and_rate(case):
    _needs_engine()
    r = case.run("fg2x")
    r.vpy(BODY)
    info = _check_or_skip(r.vspipe(info=True), info=True)

    assert info["Frames"] == 10, f"5 帧 × multi=2 应为 10 帧，实测 {info.get('Frames')}"
    assert abs(info["fps"] - 48.0) < 0.01, f"帧率应为 48，实测 {info.get('fps')}"
    assert (info["Width"], info["Height"]) == (320, 240)


def test_frame_gen_writes_duration_props(case):
    """插件自己乘帧率写 _Duration——下游靠它计时，必须核对。

    注意：输入帧没有 ``_AbsoluteTime`` 时输出也不会有（属性沿输入传递），
    所以这里只核对时长；时间戳一致性由 mpv 真实片源链路覆盖。
    """
    _needs_engine()
    r = case.run("fg-props")
    r.vpy(BODY, props=True)
    _check_or_skip(r.vspipe(end=3))

    props = r.props()
    require(props, "没有解析到帧属性（检查 dump_props 与 vspipe -j）")
    assert len(props) == 4, f"end=3 应拿到 4 帧属性，实测 {len(props)}"
    for row in props:
        assert (row["_DurationNum"], row["_DurationDen"]) == (1, 48), row


def test_frame_gen_survives_mpv_chain(case):
    """经 mpv 的 vf=vapoursynth 链路必须照常出帧，且日志里没有滤镜被禁用。

    mpv 的 VS 桥只接受整数采样格式：FrameGen 的 RGBS 输出必须自己转回 YUV420P8，
    否则日志只有 ``Unsupported output format.`` 然后滤镜被禁用。
    """
    _needs_engine()
    r = case.run("fg-mpv")
    r.vpy(
        "clip = core.resize.Bicubic(video_in, format=vs.RGBS, matrix_in_s=\"709\")\n"
        + BODY.replace(SOURCE, "").replace(
            "clip.set_output()",
            "clip = core.resize.Bicubic(clip, format=vs.YUV420P8, matrix_s=\"709\",\n"
            "                          range_in_s=\"full\", range_s=\"limited\")\nclip.set_output()",
        )
    )
    m = r.mpv("av://lavfi:testsrc2=duration=1:size=160x90:rate=24", frames=3)
    if not m.result.ok and any(x.lower() in (m.result.out + m.result.err).lower()
                              for x in SKIP_MARKERS):
        pytest.skip("NGX 在 mpv 链路下不可用")
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()
    assert len(m.frame_files()) >= 3
