"""生成 .vpy 脚本、跑 vspipe、收集 VapourSynth 日志与帧属性。

约定（与 mpv 的 vf=vapoursynth 一致）：
- 脚本里用上游变量 ``video_in``（无头跑时不存在），结尾必须 ``clip.set_output()``；
  少了这一句 mpv 只会在日志里写 "Could not get script output node."，
  然后把滤镜静默禁用并继续播放——这条静默降级正是套件要抓的东西。
- 脚本开头安装 log handler，把所有 VS 消息落到 cwd 的 ``vs.log``，并写一份
  ``vs_info.json``（VS 版本 / 解释器 / 插件路径），现场可直接对比。
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from .runner import Result, run

#: 逐帧属性采集的默认字段：时长/绝对时间/切镜/色彩元数据
PROP_NAMES = (
    "_DurationNum",
    "_DurationDen",
    "_AbsoluteTime",
    "_SceneChangePrev",
    "_SceneChangeNext",
    "_Matrix",
    "_ColorRange",
    "_Primaries",
    "_Transfer",
    "_ChromaLocation",
    "_FieldBased",
)

HEADER_CORE = '''# -*- coding: utf-8 -*-
# 由 test/suite/lib/vpy.py 生成：统一日志与自检信息，勿手改
import json
import os
import sys

import vapoursynth as vs

core = vs.core

_LOG = open("vs.log", "a", encoding="utf-8", buffering=1)


def _vs_handler(level, msg):
    _LOG.write("[%d] %s\\n" % (level, msg))


core.add_log_handler(_vs_handler)

try:
    with open("vs_info.json", "w", encoding="utf-8") as _fp:
        json.dump(
            {
                "vs_version": vs.__version__,
                "python": sys.version,
                "executable": sys.executable,
                "cwd": os.getcwd(),
                "script_dir": os.path.dirname(os.path.abspath(__file__)),
                "sys_path": sys.path,
            },
            _fp,
            ensure_ascii=False,
            indent=2,
        )
except Exception as exc:  # 信息采集失败不影响被测链路
    _LOG.write("[0] vs_info 写入失败: %r\\n" % (exc,))
'''

PROPS_DEF = '''

PROP_KEYS = {names!r}


def dump_props(clip):
    """逐帧把关键属性写成 ``PROPS {{...}}`` 行，便于事后对齐时间戳/切镜。"""

    def _fn(n, f):
        row = {{"frame": n}}
        for key in PROP_KEYS:
            val = f.props.get(key)
            if val is not None:
                row[key] = val
        _LOG.write("PROPS " + json.dumps(row, ensure_ascii=False) + "\\n")
        return clip

    return clip.std.FrameEval(_fn, clip)
'''

SET_OUTPUT = "clip.set_output()"

_FPS_RE = re.compile(r"^FPS:\s*(\d+)/(\d+)", re.MULTILINE)


def header() -> str:
    return HEADER_CORE + PROPS_DEF.format(names=list(PROP_NAMES))


def render(body: str, *, props: bool = False) -> str:
    """拼出完整脚本：头部 + body；``props=True`` 会在输出前插入逐帧属性采集。"""
    text = header() + "\n" + body.strip() + "\n"
    if props:
        idx = text.rfind(SET_OUTPUT)
        if idx < 0:
            raise ValueError("props=True 时脚本正文必须以 clip.set_output() 结尾")
        text = text[:idx] + "clip = dump_props(clip)\n" + text[idx:]
    return text


def write_script(path: Path, body: str, *, props: bool = False) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(render(body, props=props), encoding="utf-8")
    return path


def run_vspipe(
    vspipe: Path,
    script: Path,
    *,
    cwd: Path,
    info: bool = False,
    start: int | None = None,
    end: int | None = None,
    props_json: str | None = None,
    timeout: float = 300.0,
    extra: list[str] | None = None,
) -> Result:
    """无头 VS 检查：info 只读元数据，否则把帧请求一遍但不输出（``--``）。

    ``props_json`` 走 vspipe 自带的 ``-j``，比在脚本里逐帧写日志更准也更快。
    ``--end`` 是**闭区间**。
    """
    argv = [str(vspipe)]
    if info:
        argv.append("--info")
    if start is not None:
        argv += ["--start", str(start)]
    if end is not None:
        argv += ["--end", str(end)]
    if props_json:
        argv += ["-j", str(props_json)]
    argv += extra or []
    argv.append(str(script))
    if not info:
        argv.append("--")  # 只请求帧，不产生输出
    return run(argv, cwd=cwd, timeout=timeout)


def parse_info(text: str) -> dict:
    """解析 ``vspipe --info`` 输出。"""
    out: dict[str, object] = {}
    for raw_line in text.splitlines():
        if ":" not in raw_line:
            continue
        key, _, value = raw_line.partition(":")
        key, value = key.strip(), value.strip()
        if key in ("Width", "Height", "Frames", "Bit Depth"):
            try:
                out[key] = int(value)
            except ValueError:
                out[key] = value
        elif key == "FPS":
            m = _FPS_RE.match(raw_line.strip())
            if m:
                out["fps_num"], out["fps_den"] = int(m.group(1)), int(m.group(2))
                out["fps"] = int(m.group(1)) / max(1, int(m.group(2)))
        elif key in ("Format Name", "Color Family", "Resolution"):
            out[key] = value
    return out


def read_vs_log(cwd: Path) -> str:
    path = Path(cwd) / "vs.log"
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def read_props(cwd: Path, name: str = "props.json") -> list[dict]:
    """优先读 vspipe -j 的产物，退回 vs.log 里的 ``PROPS`` 行。"""
    path = Path(cwd) / name
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            try:
                data = json.loads(text)
                return data if isinstance(data, list) else [data]
            except json.JSONDecodeError:
                rows = []
                for line in text.splitlines():
                    line = line.strip().rstrip(",")
                    if line.startswith("{"):
                        try:
                            rows.append(json.loads(line))
                        except json.JSONDecodeError:
                            continue
                if rows:
                    return rows

    rows = []
    for line in read_vs_log(cwd).splitlines():
        if line.startswith("PROPS "):
            try:
                rows.append(json.loads(line[len("PROPS "):]))
            except json.JSONDecodeError:
                continue
    return rows


def read_info(cwd: Path) -> dict:
    path = Path(cwd) / "vs_info.json"
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
