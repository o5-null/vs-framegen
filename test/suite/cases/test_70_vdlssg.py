"""vdlssg 插件的端到端行为：帧数/帧率翻倍、时长属性、经 mpv 链路可用。

这些用例真的会初始化 NGX（DLSS-G），需要 NVIDIA GPU 与引擎包里的运行库，
所以一律带 ``gpu`` 标记；引擎包缺失或驱动初始化失败时跳过而不是判失败。
"""

from __future__ import annotations

import re
import warnings

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

# NVIDIA DLSS-G 后端 sm86_backend.dll 在进程收尾时随机访问违例（0xc0000005，
# 事件日志里的出错模块），帧其实已经全部输出，vspipe 退出码却变成 -1。
# 这是第三方缺陷：同一份源码不含本轮色彩改动的基线 DLL 也会复现，插件侧无法修。
# 所以只对"帧已输出但没有干净退出"的情况重跑，其它失败照旧判错。
_FG_OUTPUT = re.compile(r"Output \d+ frames? in [\d.]+ seconds")
# 崩溃时 stderr 依旧只有那条 Warning，出现下面这些字样才算真正的插件/脚本错误
_FG_ERROR = re.compile(r"(vdlssg:|vapoursynth\.|Failed to|Traceback)", re.I)


def _needs_engine():
    require(PLUGIN.is_file(), f"缺少插件 {PLUGIN}")
    require(ENGINE.is_dir(), f"缺少引擎包 {ENGINE}")


def _check_or_skip(run, info: bool = False, tolerate_exit_crash: bool = False):
    text = run.out + run.err
    if run.ok:
        return vpy.parse_info(text) if info else text
    if any(m.lower() in text.lower() for m in SKIP_MARKERS):
        pytest.skip(f"NGX/引擎不可用，跳过：{run.tail(6)}")
    if tolerate_exit_crash and _FG_OUTPUT.search(run.err) and not _FG_ERROR.search(run.err):
        warnings.warn(f"vspipe 已输出帧，随后在 DLSS-G 后端 sm86_backend.dll 收尾时崩溃"
                      f"（rc={run.rc}，第三方缺陷，不是插件错误）", stacklevel=2)
        return vpy.parse_info(text) if info else text
    raise AssertionError(f"vspipe 失败：{run.tail()}")


def _vspipe_fg(r, attempts: int = 3, **kwargs):
    """跑 vspipe，遇到 DLSS-G 后端退出崩溃（帧已输出但 rc=-1）时重跑。

    重跑前清掉上一次的属性采集产物，否则 ``vs.log`` 是追加写入，
    ``props()`` 会把崩掉那次的 4 行和重跑的一起读出来。
    """
    last = None
    for _ in range(attempts):
        for name in ("vs.log", "props.json"):
            path = r.dir / name
            if path.exists():
                path.unlink()
        last = r.vspipe(**kwargs)
        if last.ok or not _FG_OUTPUT.search(last.err):
            return last
    return last


def test_frame_gen_doubles_frames_and_rate(case):
    _needs_engine()
    r = case.run("fg2x")
    r.vpy(BODY)
    info = _check_or_skip(_vspipe_fg(r, info=True), info=True, tolerate_exit_crash=True)

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
    _check_or_skip(_vspipe_fg(r, end=3, gpu=True), tolerate_exit_crash=True)

    props = r.props()
    require(props, "没有解析到帧属性（检查 dump_props 与 vspipe -j）")
    assert len(props) == 4, f"end=3 应拿到 4 帧属性，实测 {len(props)}"
    for row in props:
        assert (row["_DurationNum"], row["_DurationDen"]) == (1, 48), row

    # 引擎帧生成是显存大户：产物里应该留下占用曲线（本机取不到时留空）
    meta = r.meta()
    mem = meta.get("memory") or {}
    require(mem, "没有采集到内存数据（检查 VFI_MEM）")
    assert mem["peak_wset_mb"] > 0, mem

    vram = meta.get("vram") or {}
    if not vram:
        pytest.skip("VS 侧没有采到显存（检查 nvml.dll / VFI_VRAM）")
    assert vram["peak_mb"] > 0, vram
    assert vram["delta_mb"] >= 0, vram
    assert vram["samples"] >= 2, vram


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
