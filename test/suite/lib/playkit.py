"""原生滤镜 ``vf_playkit`` 的定位、素材生成与日志解析。

与 VS 滤镜不同，``vf_playkit`` 只吃 D3D11 硬件帧：``--vo=image`` 或者
``--hwdec=no`` 会让它拿不到纹理，所以这条链路只能走真实显示路径
（``gpu-next`` + ``d3d11va``），证据有两个来源：

* ``PLAYKIT_DSTSTAT=1`` 时滤镜自己回读 Y 平面，打印
  ``playkit: src[i]/dst[k] sub=… y=… min=… max=…``——``src`` 是送进桥接的
  源帧，``dst`` 是 VideoProcessor 写回卡的生成帧，范围对不对一眼可见；
* IPC 截图（``lib/display.py``），拿真正送给显示器的像素做帧间比较。

生成帧和源帧的范围/原色一旦不一致，6x 补帧时两者交替出现就是「画面闪」，
这类回归靠这里的数值断言抓，不靠人眼。
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from . import paths
from .runner import Result, run

#: 滤镜没真正跑起来（静默降级）时的日志特征
SKIP_MARKERS = (
    "no D3D11VA device",
    "playkit: cannot load",
    "playkit: Bridge create failed",
    "bridge unavailable",
    "CreateDlssgFeature failed",
    "unsupported D3D11 subformat",
    "Disabling filter playkit",
    "does not export the vdlssg bridge",
    "proxy version.dll not found",
    "NGX",
)

_LUMA_RE = re.compile(
    r"playkit: (src|dst)\[(\d+)\] sub=\d+ y=([0-9.]+) min=(\d+) max=(\d+)"
)
_CHROMA_RE = re.compile(
    r"playkit: (src|dst)\[(\d+)\] cb=([0-9.]+) cr=([0-9.]+)"
)
_CS_RE = re.compile(r"playkit: colorspace cs1 in=0x([0-9a-fA-F]+) out=0x([0-9a-fA-F]+)")
_COLOURS_RE = re.compile(
    r"playkit: bridge colours matrix=(\d+) full=(\d) \(sys=(\d+) levels=(\d+)\)"
)


@dataclass
class Luma:
    """一条 ``pk_luma_stat`` 采样。"""

    tag: str  # "src" 或 "dst"
    index: int
    y: float
    lo: int
    hi: int


@dataclass
class Chroma:
    """一条 ``pk_luma_stat`` 的色度采样（Cb/Cr 均值，中性灰应≈128）。"""

    tag: str  # "src" 或 "dst"
    index: int
    cb: float
    cr: float


@dataclass
class PlaykitLog:
    """一次运行里滤镜自报的关键数值。"""

    luma: list[Luma]
    chroma: list[Chroma]
    cs: tuple[int, int] | None  # (in_cs, out_cs)
    colours: tuple[int, int, int, int] | None  # (matrix, full, sys, levels)
    text: str

    def of(self, tag: str) -> list[Luma]:
        return [s for s in self.luma if s.tag == tag]

    def chroma_of(self, tag: str) -> list[Chroma]:
        return [s for s in self.chroma if s.tag == tag]


    @property
    def dst(self) -> list[Luma]:
        return self.of("dst")

    @property
    def src(self) -> list[Luma]:
        return self.of("src")

    def skip_reason(self) -> str | None:
        for line in self.text.splitlines():
            if any(m in line for m in SKIP_MARKERS):
                return line.strip()
        return None


def bridge_dir(mpv_exe: Path | None = None) -> Path | None:
    """``vdlssg.dll`` 与 ``dlss45/``（version.dll 代理 + NGX 运行时）所在目录。

    先看开发构建旁边，再看打包 stage 的 ``vs-plugins``。
    """
    exe = mpv_exe or paths.dev_mpv()
    cand: list[Path] = []
    if exe is not None:
        cand.append(Path(exe).parent)
    stage = paths.playkit_stage()
    if stage is not None:
        cand.append(stage / "vs-plugins")

    for d in cand:
        if (d / "vdlssg.dll").exists() and (d / "dlss45" / "version.dll").exists():
            return d
    return None


def vf_chain(mpv_exe: Path, bridge: Path, *, multi: int = 2) -> str:
    """``--vf`` 里的 playkit 条目。

    只给**相对 mpv.exe 的路径**：mpv 的滤镜参数用 ``:`` 分隔，绝对 Windows
    路径里的盘符冒号会被当成分隔符（``E\\:`` 转义在 0.42 上也解析失败）。
    滤镜自己会先按 cwd、再按 exe 目录找，所以相对路径两种情况都能命中。
    """
    base = Path(mpv_exe).resolve().parent
    try:
        rel = Path(bridge).resolve().relative_to(base)
        dll = (rel / "vdlssg.dll").as_posix()
        proxy = (rel / "dlss45").as_posix()
    except ValueError:
        dll, proxy = "vdlssg.dll", "dlss45"
    return f"playkit:multi={multi}:log=no:dll={dll}:proxy_dir={proxy}"


def parse_log(text: str) -> PlaykitLog:
    luma = [
        Luma(tag=m.group(1), index=int(m.group(2)), y=float(m.group(3)),
             lo=int(m.group(4)), hi=int(m.group(5)))
        for m in _LUMA_RE.finditer(text)
    ]
    chroma = [
        Chroma(tag=m.group(1), index=int(m.group(2)), cb=float(m.group(3)),
               cr=float(m.group(4)))
        for m in _CHROMA_RE.finditer(text)
    ]
    cs = None
    m = _CS_RE.search(text)
    if m:
        cs = (int(m.group(1), 16), int(m.group(2), 16))
    colours = None
    m = _COLOURS_RE.search(text)
    if m:
        colours = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return PlaykitLog(luma=luma, chroma=chroma, cs=cs, colours=colours,
                      text=text)


def make_clip(dest: Path, *, hdr: bool, width: int = 320, height: int = 240,
              frames: int = 48, matrix: str = "bt2020nc",
              primaries: str = "bt2020") -> Result:
    """造一条**静止**的受限范围斜坡片（生成帧应和源帧同范围、同色度）。

    默认打 BT.2020 标签，因为这条路径上曾经把 limited 源渲染成 full（16→0、
    215→231）；静止画面让「生成帧 ≈ 源帧」这个前提成立，断言才稳。
    ``matrix``/``primaries`` 可改，用来做 BT.709 对照。
    """
    exe = paths.ffmpeg()
    if exe is None:
        raise AssertionError("找不到 ffmpeg（装到 PATH 或设 VFI_FFMPEG）")

    seconds = max(1.0, frames / 24.0)
    grad = "geq=lum='16+(X/W)*200':cb=128:cr=128"
    if hdr:
        body = f"{grad},format=yuv420p10le,setparams=color_primaries={primaries}:color_trc=smpte2084:colorspace={matrix}:range=limited"
        enc = ["-c:v", "libx265", "-x265-params", "log-level=error", "-pix_fmt", "yuv420p10le"]
    else:
        body = f"{grad},format=yuv420p,setparams=color_primaries={primaries}:color_trc=bt709:colorspace={matrix}:range=limited"
        enc = ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "6", "-pix_fmt", "yuv420p"]

    argv = [
        exe, "-hide_banner", "-y",
        "-f", "lavfi", "-i", f"color=c=black:s={width}x{height}:r=24:d={seconds:g}",
        "-vf", body, *enc, str(dest),
    ]
    dest.parent.mkdir(parents=True, exist_ok=True)
    return run(argv, cwd=dest.parent, timeout=120, monitor=False)
