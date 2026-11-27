"""帧序列读取：mpv 的 vo=image 只会写 png/jpg/webp/jxl/avif，
所以这里读 PNG 头拿到尺寸/位深，再用 ffmpeg 一次性解成裸 RGB，交给 numpy。

只做「读」，不做断言；度量在 metrics.py。
"""

from __future__ import annotations

import re
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

PNG_SIG = b"\x89PNG\r\n\x1a\n"
_PNG_RE = re.compile(r"^(\d+)\.png$", re.IGNORECASE)

#: PNG color type -> (通道数, 名字)
COLOR_TYPES = {0: (1, "gray"), 2: (3, "rgb"), 4: (2, "graya"), 6: (4, "rgba")}

#: (PNG 布局, 是否 16bit) -> ffmpeg 裸像素格式；mpv 的截图是 RGBA，不能当 RGB 读
_PIX_FMTS = {
    ("rgb", False): "rgb24",
    ("rgb", True): "rgb48le",
    ("rgba", False): "rgba",
    ("rgba", True): "rgba64le",
    ("gray", False): "gray",
    ("gray", True): "gray16le",
    ("graya", False): "ya8",
    ("graya", True): "ya16le",
}


@dataclass(frozen=True)
class FrameInfo:
    path: Path
    width: int
    height: int
    bit_depth: int
    color_type: int

    @property
    def channels(self) -> int:
        return COLOR_TYPES[self.color_type][0]

    @property
    def kind(self) -> str:
        return COLOR_TYPES[self.color_type][1]


def read_png_info(path: Path) -> FrameInfo:
    """仅解析 IHDR，不依赖 PIL。"""
    head = Path(path).read_bytes()[:33]
    if len(head) < 33 or head[:8] != PNG_SIG:
        raise ValueError(f"不是 PNG: {path}")
    if head[12:16] != b"IHDR":
        raise ValueError(f"PNG 缺少 IHDR: {path}")
    width, height = struct.unpack(">II", head[16:24])
    return FrameInfo(Path(path), width, height, head[24], head[25])


def list_frames(frames_dir: Path) -> list[Path]:
    """按序号排序返回 vo-image 写出的帧，忽略非 8 位序号命名的杂文件。"""
    items = []
    for p in Path(frames_dir).glob("*.png"):
        m = _PNG_RE.match(p.name)
        if m:
            items.append((int(m.group(1)), p))
    return [p for _, p in sorted(items)]


def load_sequence(frames_dir: Path, *, limit: int | None = None) -> np.ndarray:
    """读取帧序列，返回 (N, H, W, C) 的 uint8/uint16 数组。

    用 ffmpeg 解 PNG -> raw 到临时文件，避免几十张图反复起进程。
    """
    files = list_frames(frames_dir)
    if not files:
        raise AssertionError(f"没有帧输出: {frames_dir}")
    if limit is not None:
        files = files[:limit]

    info = read_png_info(files[0])
    pix_fmt = _PIX_FMTS[(info.kind, info.bit_depth == 16)]
    channels = info.channels

    raw = Path(frames_dir) / "_decode.raw"
    exe = _ffmpeg()
    start = int(files[0].stem)
    argv = [
        exe, "-v", "error", "-y",
        "-start_number", str(start),
        "-i", str(Path(frames_dir) / "%08d.png"),
        "-frames:v", str(len(files)),
        "-f", "rawvideo", "-pix_fmt", pix_fmt,
        str(raw),
    ]
    proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise AssertionError(f"ffmpeg 解码帧失败: {proc.stderr.strip()}")

    dtype = np.uint16 if info.bit_depth == 16 else np.uint8
    expected = len(files) * info.height * info.width * channels
    data = np.fromfile(raw, dtype=dtype, count=expected)
    raw.unlink(missing_ok=True)
    if data.size != expected:
        raise AssertionError(f"解码帧数不足: {data.size}/{expected}")

    shape = (len(files), info.height, info.width, channels)
    return data.reshape(shape)


def _ffmpeg() -> str:
    from . import paths

    exe = paths.ffmpeg()
    if not exe:
        raise AssertionError("找不到 ffmpeg（装到 PATH 或设 VFI_FFMPEG）")
    return exe
