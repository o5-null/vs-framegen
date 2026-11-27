"""对照 k7f.SCDetect2(algo=5, HOG) 与插件内置 HOG 检测的切镜判定。

用法：
	python bench\\src\\verify_k7_hog.py synthetic [帧数]
	python bench\\src\\verify_k7_hog.py <样本索引> [帧数]

同一段素材分别跑：
	1) k7f.SCDetect2(algo=5, threshold=0.12) —— 统计 _SceneChangeNext
	2) core.vdlssg.FrameGen(scene_threshold=0.12) —— 统计 fallback=1 的 slot1
两条链都要求属性一致；输出边界集合与差异。
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(r"E:\code\dlssg-vfi")
BIN = ROOT / "vs-dlssg" / "bin"
VSPIPE = ROOT / ".venv" / "Scripts" / "vspipe.exe"
BSDLL = ROOT / "测试环境" / "mpv-lazy-nv" / "vs-plugins" / "BestSource.dll"
K7SRC = ROOT / "k7sfunc" / "src"
WORK = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\k7hog")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import real_data_bench as B  # noqa: E402

SYNTH = Path(r"C:\Users\benben\AppData\Local\Temp\opencode\scbench\clip.mkv")
THRESHOLD = 0.12

K7_SCRIPT = r'''
import sys
import vapoursynth as vs

sys.path.insert(0, r"{k7src}")
import k7sfunc as k7f

core = vs.core
core.std.LoadPlugin(r"{bsdll}")
src = core.bs.VideoSource(source=r"{src}", cachepath=r"{cache}")[{start}:{stop}]
out = k7f.SCDetect2(src, algo=5, threshold={thr})


def dump(n, f):
	print("SC " + __import__("json").dumps(dict(n=n,
		next=int(f.props.get("_SceneChangeNext", 0)),
		prev=int(f.props.get("_SceneChangePrev", 0)))), file=sys.stderr)
	return f


out.std.ModifyFrame(out, dump).set_output()
'''

PLUGIN_SCRIPT = r'''
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"{bsdll}")
core.std.LoadPlugin(r"{vdll}")
src = core.bs.VideoSource(source=r"{src}", cachepath=r"{cache}")[{start}:{stop}]
fg = core.vdlssg.FrameGen(src, multi=2, proxy_dir=r"{bin}\dlss45",
                          scene_threshold={thr}, duplicate_threshold=0)


def dump(n, f):
	print("REC " + __import__("json").dumps(dict(n=n,
		fb=int(f.props.get("_DLSSGFallback", -1)),
		slot=int(f.props.get("_DLSSGMultiFrameIndex", -1)))), file=sys.stderr)
	return f


fg.std.ModifyFrame(fg, dump).set_output()
'''


def _run(script: Path, prefix: str) -> list[dict]:
	proc = subprocess.run([str(VSPIPE), str(script), "-"], capture_output=True)
	recs = []
	for line in proc.stderr.decode("utf-8", "replace").splitlines():
		if line.startswith(prefix):
			recs.append(json.loads(line[len(prefix):]))
	if not recs:
		raise RuntimeError(proc.stderr.decode("utf-8", "replace")[-3000:])
	return recs


def main() -> int:
	if len(sys.argv) < 2:
		print(__doc__)
		return 2
	arg = sys.argv[1]
	frames = int(sys.argv[2]) if len(sys.argv) > 2 else 240
	if arg == "synthetic":
		label, path, start = "synthetic", SYNTH, 0
	else:
		label, path, start = B.SAMPLES[int(arg)]

	WORK.mkdir(parents=True, exist_ok=True)
	cache = WORK / "bs"
	k7_vpy = WORK / "k7hog.vpy"
	plug_vpy = WORK / "plughog.vpy"
	common = dict(bsdll=BSDLL, src=path, cache=cache, start=start,
				  stop=start + frames, thr=THRESHOLD)
	k7_vpy.write_text(K7_SCRIPT.format(k7src=K7SRC, **common), encoding="utf-8")
	plug_vpy.write_text(PLUGIN_SCRIPT.format(vdll=BIN / "vdlssg.dll", bin=BIN, **common),
						encoding="utf-8")

	t0 = time.perf_counter()
	sc = _run(k7_vpy, "SC ")
	t1 = time.perf_counter()
	rec = _run(plug_vpy, "REC ")
	t2 = time.perf_counter()

	k7_cuts = [r["n"] for r in sc if r["next"]]
	plug_cuts = [r["n"] // 2 for r in rec if r["slot"] == 1 and r["fb"] == 1]
	both = sorted(set(k7_cuts) & set(plug_cuts))
	only_k7 = sorted(set(k7_cuts) - set(plug_cuts))
	only_plug = sorted(set(plug_cuts) - set(k7_cuts))

	print(f"{label}  {frames} 帧（源帧 {start}..{start + frames}） 阈值 {THRESHOLD}")
	print(f"k7f algo5 切镜 {len(k7_cuts)} 次  耗时 {t1 - t0:.2f}s")
	print(f"插件 HOG  切镜 {len(plug_cuts)} 次  耗时 {t2 - t1:.2f}s")
	print(f"一致 {len(both)}  仅 k7f {len(only_k7)}  仅插件 {len(only_plug)}")
	print("k7f :", k7_cuts)
	print("plug:", plug_cuts)
	if only_k7 or only_plug:
		print("差异 k7f-only:", only_k7, " plug-only:", only_plug)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
