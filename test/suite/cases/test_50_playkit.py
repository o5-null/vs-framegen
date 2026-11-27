"""playkit 包自检：用打包 stage 里的 mpv/python 跑，而不是开发目录。

这类用例的价值是「用户拿到的那个包到底能不能跑」，
所以断言的是 stage 内的解释器路径与预置脚本，不是 .venv。
"""

from __future__ import annotations

import json

import pytest

from lib import paths
from lib.runner import BASE_ENV, run
from lib.util import require

pytestmark = [pytest.mark.mpv, pytest.mark.playkit]

IDENTITY = """
clip = video_in
clip.set_output()
"""

PY_CHECK = """
import json, sys
info = {"python": sys.executable, "version": sys.version.split()[0]}
import vapoursynth as vs
info["vapoursynth"] = vs.__version__
info["std"] = hasattr(vs.core, "std")
try:
    import k7sfunc
    info["k7sfunc"] = getattr(k7sfunc, "__version__", "?")
    info["playkit_run"] = hasattr(k7sfunc, "PLAYKIT_RUN")
    presets = getattr(k7sfunc, "PLAYKIT_PRESETS", None)
    info["presets"] = sorted(presets) if isinstance(presets, dict) else list(presets or [])
except Exception as exc:
    info["k7sfunc_error"] = repr(exc)
print("PYINFO " + json.dumps(info, ensure_ascii=False))
"""


def _stage():
    stage = paths.playkit_stage()
    require(stage is not None, "找不到 mpv-playkit stage（build 一次或设 VFI_PLAYKIT）")
    return stage


def test_stage_python(case):
    stage = _stage()
    py = stage / "python.exe"
    require(py.exists(), f"stage 里没有 python.exe：{stage}")

    res = run([str(py), "-c", PY_CHECK], cwd=stage, env=BASE_ENV, timeout=180)
    case.save("pycheck.txt", res.out + "\n--- stderr ---\n" + res.err)
    assert res.ok, res.tail()

    line = next((ln for ln in res.out.splitlines() if ln.startswith("PYINFO ")), None)
    require(line is not None, "stage python 没有输出 PYINFO，检查 vapoursynth/k7sfunc 是否在包里")
    info = json.loads(line[len("PYINFO "):])
    case.save("pycheck.json", json.dumps(info, ensure_ascii=False, indent=2))

    assert info["std"] is True
    assert info.get("k7sfunc_error") is None, info.get("k7sfunc_error")
    assert info.get("playkit_run") is True, "k7sfunc 缺 PLAYKIT_RUN"
    assert info.get("presets"), "k7sfunc 没有 PLAYKIT_PRESETS"


def test_stage_mpv_runs_identity(case):
    """stage 的 mpv 必须能加载它自带的 vsscript 与插件，并真的输出帧。"""
    from lib import mpv as mpvlib

    stage = _stage()
    mpv = stage / "mpv.exe"
    require(mpv.exists(), f"stage 里没有 mpv.exe：{stage}")

    r = case.run("identity")
    r.vpy(IDENTITY)
    # 这里必须显式用 stage 的 mpv，不能用 paths.mpv()（后者优先开发目录）
    m = mpvlib.run_mpv(mpv, "av://lavfi:color=c=0x204060:s=64x48:r=24:d=1",
                       cwd=r.dir, frames=2, vf_script="script.vpy", scale=(64, 48))
    assert m.result.ok, m.result.tail()
    assert not m.fatal(), m.fatal()
    assert m.frame_files(), "stage mpv 没有落盘任何帧"

    info = r.vs_info()
    require(info, "没有 vs_info.json：脚本没执行（滤镜可能被静默禁用）")
    exe = str(info.get("executable", ""))
    assert str(stage).lower() in exe.lower(), f"VS 解释器不在 stage 内：{exe}"


def test_stage_has_presets(case):
    stage = _stage()
    vpy_dir = stage / "portable_config" / "vs"
    require(vpy_dir.is_dir(), f"stage 缺 portable_config/vs：{stage}")
    scripts = sorted(p.name for p in vpy_dir.glob("*.vpy"))
    case.save("presets.json", json.dumps(scripts, ensure_ascii=False, indent=2))
    assert scripts, "stage 里一个预设脚本都没有"
