"""VapourSynth 无头检查：脚本能跑、元数据对、插件能加载。

这一层不碰 mpv，用 vspipe 直接驱动 .venv 里的 VS（R74），
失败信息比 mpv 的「滤镜被禁用」清楚得多。
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from lib import paths, vpy
from lib.util import require

pytestmark = pytest.mark.vs

GRAY = """
clip = core.std.BlankClip(width=64, height=48, format=vs.GRAY8, length=5, fpsnum=25, fpsden=1, color=[128])
clip.set_output()
"""


def test_clip_info(case):
    r = case.run("info")
    r.vpy(GRAY)
    res = r.vspipe(info=True)
    assert res.ok, res.tail()

    info = vpy.parse_info(res.out)
    r.write("info.json", json.dumps(info, ensure_ascii=False, indent=2))
    assert info["Width"] == 64
    assert info["Height"] == 48
    assert info["Frames"] == 5
    assert (info["fps_num"], info["fps_den"]) == (25, 1)
    assert str(info.get("Format Name", "")).upper().replace(".", "") in ("GRAY8", "GRAY")


def test_frames_are_requested(case):
    """整段请求一遍：抓滤镜/脚本在真跑的时候才炸的问题。"""
    r = case.run("drain")
    r.vpy(GRAY)
    res = r.vspipe(start=0, end=4)
    assert res.ok, res.tail()
    assert Path(r.dir, "vs.log").exists()
    # 脚本头必须真的执行过
    assert "New message handler installed" in r.vs_log()


def test_props_json(case):
    r = case.run("props")
    r.vpy(GRAY)
    res = r.vspipe(start=0, end=2, props_json="props.json")
    assert res.ok, res.tail()
    rows = r.props()
    assert len(rows) == 3, f"vspipe -j 应给出 3 帧属性，实际 {len(rows)}"


def test_vdlssg_plugin_loads(case):
    dll = paths.repo_root() / "vs-dlssg" / "bin" / "vdlssg.dll"
    require(dll.exists(), f"没有 {dll}，先构建插件")

    body = f"""
core.std.LoadPlugin(r"{dll.as_posix()}")
names = sorted(n for n in dir(core.vdlssg) if not n.startswith("_"))
with open("plugin.json", "w", encoding="utf-8") as fp:
    json.dump({{"dll": r"{dll.as_posix()}", "functions": names}}, fp, ensure_ascii=False, indent=2)
clip = core.std.BlankClip(width=64, height=48, format=vs.GRAY8, length=2)
clip.set_output()
"""
    r = case.run("plugin-load")
    r.vpy(body)
    res = r.vspipe(info=True)
    assert res.ok, res.tail()

    data = json.loads((r.dir / "plugin.json").read_text(encoding="utf-8"))
    assert data["functions"], "vdlssg 命名空间下没有任何函数"
    assert "FrameGen" in data["functions"]


def test_missing_plugins_are_reported(case):
    """负例：脚本炸掉时 vspipe 必须给非零退出码和明确报错。

    （mpv 那边不会——它会把滤镜静默禁用，见 test_30_color 的守卫用例。）
    """
    r = case.run("broken")
    r.vpy("raise RuntimeError('套件自检：这条必须失败')")
    res = r.vspipe(info=True)
    assert not res.ok
    assert "套件自检" in (res.err + res.out)


from pathlib import Path  # noqa: E402  (仅为上面的 Path 引用保持顺序清晰)
