"""k7f_bridge.py —— vpy ⇄ mpv 的公用参数桥（vpy 侧，只用标准库）

两条通道：
- 下行（mpv → vpy）：`vf=...:user-data="k=v;k=v"`，mpv 把字符串注入全局
  `user_data`。这是唯一能改初始化参数的运行期通道，改值要重建滤镜。
- 上行（vpy → mpv）：vpy 把**实际生效**的参数写到脚本同目录的
  `<脚本文件名>.k7f.json`，mpv 侧 `k7f_bridge.lua` 轮询读取并显示到 OSD。
  vpy 读不到 mpv 属性、也没有 command 回调，写文件是最稳的回传方式。

本模块放在 vpy 同目录即可 import；所有写盘异常都会吞掉并只提示一次，
上报失败最多让 OSD 少一行，绝不能影响播放。
"""

import json
import os
import sys
import time

# 报告格式版本：字段只增不改，mpv 侧按需取
SCHEMA_VERSION = 1

# 报告文件后缀：与 vpy 同目录、同名，例如 MEMC_DLSSG_NV.vpy.k7f.json
SIDECAR_SUFFIX = ".k7f.json"

_warned = set()


def _warn(message: str) -> None:
	"""同一个错误只提示一次，避免每次 seek 重跑脚本时刷屏。"""

	if message in _warned:
		return
	_warned.add(message)
	print(f"[k7f_bridge] {message}", flush=True)


def parse_user_data(text) -> dict:
	"""把 user-data 字符串解析成小写键的字典。

	兼容 mpv 的两种分隔符：`;` 与 `,`；键和值两端的空白会被去掉，
	但值内部不能有空白、逗号、分号、冒号——真需要时请改用环境变量。
	"""

	opts = {}
	if not text:
		return opts
	for part in str(text).replace(",", ";").split(";"):
		key, sep, val = part.partition("=")
		key = key.strip().lower()
		if sep and key:
			opts[key] = val.strip()
	return opts


def as_bool(value) -> bool:
	"""OSD/环境变量里的布尔写法统一：0/false/off/no/空 为假，其余为真。"""

	return str(value).strip().lower() not in ("0", "false", "off", "no", "")


def get(ud: dict, key: str, default, cast=None, lo=None, hi=None):
	"""从 user-data 取一个值：带类型转换与上下限钳制，非法值保持默认。

	示例：
		Multi = k7fb.get(_ud, "multi", Multi, int, 2, 6)
		Scene = k7fb.get(_ud, "scene", Scene, float, 0.0)
		Log   = k7fb.get(_ud, "log", Log, k7fb.as_bool)
	"""

	if not ud or key not in ud:
		return default

	raw = ud[key]
	try:
		if cast is bool:
			value = as_bool(raw)
		elif cast is int:
			# 允许 "4" 和 "4.0"：OSD 菜单里两种写法都出现过
			value = int(float(raw))
		elif cast is float:
			value = float(raw)
		elif cast is None:
			value = raw
		else:
			value = cast(raw)
	except (TypeError, ValueError):
		_warn(f"忽略非法取值 {key}={raw!r}")
		return default

	if lo is not None and value < lo:
		value = lo
	if hi is not None and value > hi:
		value = hi

	return value


def sidecar_path(script_file) -> str:
	"""报告文件的绝对路径：与脚本同目录，避免传绝对路径时被 vf 的 `:` 截断。"""

	return os.path.abspath(os.fspath(script_file)) + SIDECAR_SUFFIX


def clip_info(clip, fps=None):
	"""从 VS clip 取宽高与帧率；拿不到就返回 None（不抛异常）。

	video_in 的 clip.fps 往往是 0（容器帧率只在 mpv 的 container_fps 里），
	所以调用方可以把 fps_in 显式传进来。
	"""

	try:
		info = {"w": int(clip.width), "h": int(clip.height)}
	except Exception:
		return None

	if fps is None:
		try:
			fps = clip.fps
		except Exception:
			fps = None

	try:
		if fps and float(fps) > 0:
			info["fps"] = round(float(fps), 3)
	except Exception:
		pass

	return info


def publish(
	script_file,
	*,
	title=None,
	engine=None,
	params=None,
	summary=None,
	in_info=None,
	out_info=None,
	ok=True,
	note=None,
	**extra,
) -> bool:
	"""把实际生效的参数写到 `<script_file>.k7f.json`（原子替换）。

	参数：
		title    OSD 前缀，如 "DLSSG"
		engine   实际使用的引擎，如 "vdlssg"
		params   实际生效的参数字典（JSON 可序列化）
		summary  一行摘要；省略时用 `k=v · k=v` 自动拼
		in_info/out_info  `clip_info()` 的结果
		ok       初始化是否成功；失败时 OSD 会带 ⚠
		note     额外提示，如 "已预缩到 1080p"
		extra    其它要一起带给 mpv 的字段
	"""

	payload = {
		"v": SCHEMA_VERSION,
		"ts": round(time.time(), 3),
		"pid": os.getpid(),
		"script": os.path.basename(os.fspath(script_file)),
		"ok": bool(ok),
	}

	if title:
		payload["title"] = str(title)
	if engine:
		payload["engine"] = str(engine)
	if params:
		payload["params"] = params
	if summary:
		payload["summary"] = str(summary)
	elif params:
		payload["summary"] = " · ".join(f"{k}={v}" for k, v in params.items())
	if in_info:
		payload["input"] = in_info
	if out_info:
		payload["output"] = out_info
	if note:
		payload["note"] = str(note)
	payload.update(extra)

	try:
		text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
	except Exception as exc:
		_warn(f"报告序列化失败：{exc}")
		return False

	return _write_atomic(sidecar_path(script_file), text)


def read_status(script_file):
	"""读回报告（调试/测试用；mpv 侧由 Lua 读同一个文件）。"""

	try:
		with open(sidecar_path(script_file), "r", encoding="utf-8") as fh:
			return json.load(fh)
	except Exception:
		return None


def _write_atomic(path: str, text: str) -> bool:
	"""先写临时文件再 os.replace；mpv 恰好在读时重试几次。

	Windows 上读取方没开 FILE_SHARE_DELETE 时 replace 会失败，所以最多退 3 次，
	再不行就直接覆盖写：报告延迟/错一帧都能接受，丢报告才是问题。
	"""

	tmp = f"{path}.{os.getpid()}.tmp"

	try:
		with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
			fh.write(text)
	except Exception as exc:
		_warn(f"报告临时文件写入失败：{exc}")
		return False

	for _ in range(3):
		try:
			os.replace(tmp, path)
			return True
		except OSError:
			time.sleep(0.01)

	try:
		os.remove(tmp)
	except OSError:
		pass

	try:
		with open(path, "w", encoding="utf-8", newline="\n") as fh:
			fh.write(text)
		return True
	except Exception as exc:
		_warn(f"报告写入失败：{exc}")
		return False


if __name__ == "__main__":
	# 最小自检：不依赖 VapourSynth，方便 CI/命令行验证解析逻辑
	_sample = parse_user_data("multi=4; scene=0.35,log=1")
	assert _sample == {"multi": "4", "scene": "0.35", "log": "1"}, _sample
	assert parse_user_data(" Multi=3 ; scene = 0.5 ") == {"multi": "3", "scene": "0.5"}
	assert get(_sample, "multi", 3, int, 2, 6) == 4
	assert get(_sample, "scene", 0.25, float, 0.0) == 0.35
	assert get(_sample, "log", False, bool) is True
	assert get(_sample, "camera", 7, int) == 7
	print("k7f_bridge.py 自检通过", file=sys.stderr)
