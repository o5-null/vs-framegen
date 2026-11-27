"""k7sfunc 用例：导入面、切镜检测（SCDetect2）、debug 模式的逐帧数据。

k7sfunc 不在 ``.venv`` 里（只有 playkit stage 里装了），所以脚本开头把
``paths.k7sfunc_src()`` 挂进 ``sys.path``。

``SCDetect2`` 用 ``std.ModifyFrame`` 写切镜属性——这条链在 vspipe 无头下正常，
在 mpv 的 ``vf=vapoursynth`` 桥下会静默 EOF（见 test/AGENT.md 的实测坑），
所以切镜断言只放在无头路径，mpv 路径留一条 xfail 记录该缺陷。
"""

from __future__ import annotations

import json

import pytest

from lib import paths

pytestmark = pytest.mark.vs

#: 4 帧黑 + 4 帧白：只有 3→4 之间有硬切
CUT_CLIP = """
a = core.std.BlankClip(width=64, height=48, format=vs.GRAY8, length=4, fpsnum=24, fpsden=1, color=16)
b = core.std.BlankClip(width=64, height=48, format=vs.GRAY8, length=4, fpsnum=24, fpsden=1, color=235)
clip = core.std.Splice([a, b])
"""

STATIC_CLIP = """
clip = core.std.BlankClip(width=64, height=48, format=vs.GRAY8, length=8, fpsnum=24, fpsden=1, color=126)
"""


def _prelude(*, need: bool = True) -> str:
    """把 k7sfunc 挂进 sys.path 并 import，返回脚本前缀。"""
    src = paths.k7sfunc_src()
    if src is None:
        if need:
            pytest.skip("找不到 k7sfunc（设 VFI_K7 或构建 playkit stage）")
        return ""
    return f"sys.path.insert(0, {src.as_posix()!r})\nimport k7sfunc as k7f\n"


def test_k7_imports(case):
    """k7sfunc 可导入，导出面齐全（PLAYKIT 入口 + SCDetect2）。"""
    body = _prelude() + """
open("k7.json", "w", encoding="utf-8").write(json.dumps({
    "version": k7f.__version__,
    "presets": list(k7f.PLAYKIT_PRESETS),
    "playkit_run": callable(getattr(k7f, "PLAYKIT_RUN", None)),
    "scdetect2": callable(getattr(k7f, "SCDetect2", None)),
}, ensure_ascii=False))
""" + STATIC_CLIP + "clip.set_output()"

    r = case.run("imports")
    r.vpy(body)
    run = r.vspipe()
    assert run.ok, run.tail()

    info = json.loads((r.dir / "k7.json").read_text(encoding="utf-8"))
    assert info["version"].startswith("1.9"), info
    assert info["playkit_run"] and info["scdetect2"], info
    assert len(info["presets"]) >= 5, info


def test_scdetect_marks_hard_cut(case):
    """硬切前后一帧分别打上 _SceneChangeNext / _SceneChangePrev。"""
    body = _prelude() + CUT_CLIP + """
clip = k7f.SCDetect2(clip, algo=1, threshold=0.1)
clip.set_output()
"""
    r = case.run("cut")
    r.vpy(body, props=True)
    run = r.vspipe(end=7)
    assert run.ok, run.tail()

    rows = {int(p["frame"]): p for p in r.props()}
    assert rows, "没有采到逐帧属性"
    assert rows[3].get("_SceneChangeNext") == 1, rows[3]
    assert rows[4].get("_SceneChangePrev") == 1, rows[4]
    for frame in (0, 1, 2, 5, 6, 7):
        assert not rows[frame].get("_SceneChangePrev"), rows[frame]
        assert not rows[frame].get("_SceneChangeNext"), rows[frame]


def test_scdetect_ignores_static_clip(case):
    """静态画面不该报切镜。"""
    body = _prelude() + STATIC_CLIP + """
clip = k7f.SCDetect2(clip, algo=1, threshold=0.1)
clip.set_output()
"""
    r = case.run("static")
    r.vpy(body, props=True)
    run = r.vspipe(end=7)
    assert run.ok, run.tail()

    for p in r.props():
        assert not p.get("_SceneChangePrev"), p
        assert not p.get("_SceneChangeNext"), p


def test_scdetect_rejects_bad_threshold(case):
    """阈值越界要立刻报错，而不是静默降级。"""
    body = _prelude() + CUT_CLIP + """
clip = k7f.SCDetect2(clip, algo=1, threshold=1.5)
clip.set_output()
"""
    r = case.run("bad-threshold")
    r.vpy(body)
    run = r.vspipe()
    assert not run.ok
    assert "threshold must be between 0 and 1" in (run.out + run.err + r.vs_log())


def test_debug_reports_cut_and_duplicate(case):
    """debug 模式逐帧输出切镜标志与重复帧检测（DEBUG 行 + meta.json）。"""
    # 逐帧灰度依次为 10,20,30,30,200,...：索引 3 与 2 相同（重复帧），
    # 3→4 跳变 170/255（切镜），其余相邻帧步进 10/255 不触发 0.1 阈值。
    body = _prelude() + """
parts = [
    core.std.BlankClip(width=64, height=48, length=1, format=vs.GRAY8,
                       fpsnum=24, fpsden=1, color=v)
    for v in (10, 20, 30, 30, 200, 210, 220, 230, 240)
]
clip = core.std.Splice(parts)
clip = k7f.SCDetect2(clip, algo=1, threshold=0.1)
clip.set_output()
"""
    r = case.run("debug")
    r.vpy(body, props=True, debug=True)
    run = r.vspipe(end=8)
    assert run.ok, run.tail()

    summary = r.debug()
    assert summary["samples"] == 9, summary
    assert summary["dups"] == [3], summary
    assert summary["scene_next"] == [3], summary
    assert summary["scene_prev"] == [4], summary
    assert summary["max_diff"] > summary["median_diff"] >= 0.0

    rows = r.debug_rows()
    assert rows[3]["dup"] is True and rows[3]["diff"] == 0.0
    assert rows[2]["dup"] is False and rows[4]["dup"] is False

    meta = r.meta()
    assert meta["debug"]["dups"] == [3], meta.get("debug")


def test_debug_works_in_mpv(case):
    """debug 采样不用 std.ModifyFrame，所以 mpv 桥下也能跑。"""
    r = case.run("mpv")
    r.vpy("clip = video_in\nclip.set_output()", debug=True)
    m = r.mpv("av://lavfi:testsrc2=duration=1:size=96x54:rate=24", frames=6, scale=(96, 54))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()

    summary = m.debug
    assert summary.get("samples", 0) >= 3, summary
    assert summary["dups"] == [], summary


def test_scdetect_survives_mpv_bridge(case):
    """切镜检测在 mpv 真链路里可用（SCDetect2 用 clips=[...] 关键字调用，不受绑定错位影响）。"""
    r = case.run("mpv-sc")
    r.vpy(_prelude() + "clip = k7f.SCDetect2(video_in, algo=1, threshold=0.1)\nclip.set_output()")
    m = r.mpv("av://lavfi:testsrc2=duration=1:size=96x54:rate=24", frames=6, scale=(96, 54))

    assert m.result.ok, m.result.tail()
    assert m.saved_frames(), "mpv 没落盘任何帧"
    assert not m.fatal(), m.fatal()
    assert "Filter error" not in m.log
