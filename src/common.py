"""Shared setup for the eight-engine frame-generation benchmark.

Everything here is about *how the benchmark is fed*, not what is measured:
the source clip (测试数据/1080p, 40 consecutive frames) is rescaled to each test
resolution, and each engine list carries the flag / model file that selects it.

Two families, because the two hosts are different programs:
  * SMV backends   - smv-live.exe --offline, one flag each (dlssg / xess / vfg / fsr4)
  * RIFE variants  - the vs-mlrt ONNX models, run on TensorRT with the 11-plane
                     input documented in vsmlrt.RIFEMerge (see rife_trt.py)

Paths: everything host-side (host program, ONNX models, source PNGs, TRT engine
cache, result folder) is resolved against VFI_ROOT, which defaults to the parent
of this repository. Set it when the benchmark lives next to a different tree.
"""

import os
import tempfile

import cv2
import numpy as np

# ------------------------------------------------------------------ paths
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.environ.get("VFI_ROOT") or os.path.dirname(REPO)

DATA = os.environ.get("VFI_DATA") or os.path.join(ROOT, "测试数据", "engines")
SRC_PNG = os.environ.get("VFI_SRC_PNG") or os.path.join(ROOT, "测试数据", "1080p")
PNG_DIR = SRC_PNG  # legacy alias

SMV_ENGINE = os.environ.get("SMV_ENGINE") or os.path.join(
    ROOT, "参考插件", "SmoothMyVideo-protable", "resources", "engine")
SMV_EXE = os.path.join(SMV_ENGINE, "live", "smv-live.exe")
SMV_CACHE = os.environ.get("SMV_CACHE") or os.path.join(ROOT, "work", "smv", "probe_cache")

RIFE_ONNX = os.environ.get("RIFE_ONNX") or os.path.join(
    ROOT, "测试环境", "mpv-lazy", "vs-plugins", "models", "rife")
TRT_CACHE = os.environ.get("TRT_CACHE") or os.path.join(ROOT, "work", "engines")

# ffmpeg cannot parse non-ASCII paths or a drive colon inside a filter graph,
# so the scratch folder (raw streams, numbered PNG sequences) stays ASCII.
if os.environ.get("VFI_TMP"):
    TMP = os.environ["VFI_TMP"]
else:
    TMP = os.path.join(tempfile.gettempdir(), "engine_bench")
    if not TMP.isascii():
        TMP = os.path.join(os.environ.get("SystemDrive", "C:") + os.sep, "vfi_bench_tmp")

RESOLUTIONS = [
    (480, 270, "w480"),
    (640, 360, "w640"),
    (800, 450, "w800"),
    (960, 540, "w960"),
    (1280, 720, "720p"),
    (1600, 900, "w1600"),
    (1920, 1080, "1080p"),
    (2240, 1260, "w2240"),
    (2560, 1440, "1440p"),
    (3200, 1800, "w3200"),
    (3840, 2160, "4k"),
]

# the four headline resolutions used by the report's tables and curves
MAIN_RES = ["720p", "1080p", "1440p", "4k"]

# (name, smv-live flag)
SMV_ENGINES = [
    ("dlssg", ["--dlssg"]),
    ("xess", ["--xess-fg"]),
    ("vfg", ["--nvvfg"]),
    ("fsr4", ["--fsrfg"]),
]

# (name, onnx stem, tile multiple required by that model)
RIFE_MODELS = [
    ("rife4.6", "rife_v4.6", 32),
    ("rife4.25lite", "rife_v4.25_lite", 128),
    ("rife4.26", "rife_v4.26", 64),
    ("rife4.26heavy", "rife_v4.26_heavy", 64),
]

ENGINES = [e[0] for e in SMV_ENGINES] + [e[0] for e in RIFE_MODELS]

# label -> colour, used by the charts and the comparison panels
COLORS = {
    "dlssg": "#76b900",
    "xess": "#0068b5",
    "vfg": "#1a1a1a",
    "fsr4": "#e01b24",
    "rife4.6": "#c061cb",
    "rife4.25lite": "#ff7800",
    "rife4.26": "#26a269",
    "rife4.26heavy": "#1c71d8",
}


def png_names():
    return sorted(os.path.join(SRC_PNG, n) for n in os.listdir(SRC_PNG) if n.endswith(".png"))


def _imread(path):
    """cv2.imread cannot open the non-ASCII paths this project uses."""
    buf = np.fromfile(path, np.uint8)
    img = cv2.imdecode(buf, 1)
    if img is None:
        raise RuntimeError("cannot decode " + path)
    return img[:, :, ::-1]  # BGR -> RGB


def load_frame(path, w, h):
    """One source frame as uint8 RGB at the requested size."""
    img = _imread(path)
    if img.shape[1] == w and img.shape[0] == h:
        return img
    interp = cv2.INTER_AREA if w * h < img.shape[0] * img.shape[1] else cv2.INTER_CUBIC
    return cv2.resize(img, (w, h), interpolation=interp)


def source_frames(w, h, count=40):
    names = png_names()
    if count > len(names):
        raise ValueError("only %d source frames" % len(names))
    return [load_frame(p, w, h) for p in names[:count]]


def odd_frames(w, h):
    """The 20 odd-indexed frames: interpolating between neighbours gives the even ones back,
    so every generated tween has a real ground-truth frame (this is test/vd_common.triplets)."""
    names = png_names()
    return [load_frame(names[2 * k], w, h) for k in range(20)], names


def raw_path(w, h, tag):
    return os.path.join(TMP, "src_%s_x%d.raw" % (tag, w))


def write_raw(frames, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        for f in frames:
            fh.write(f.tobytes())
    return path


def ensure_raw(w, h, tag, count=40):
    """Raw RGB24 stream shared by every SMV backend, so the input cost is identical."""
    path = raw_path(w, h, tag)
    want = w * h * 3 * count
    if os.path.exists(path) and os.path.getsize(path) == want:
        return path
    return write_raw(source_frames(w, h, count), path)


def gt_names():
    """[name for tween i] - tween i stands in for source frame 2i+1."""
    names = png_names()
    return [os.path.basename(names[2 * i + 1]) for i in range(19)]


def quality_dir(engine):
    return os.path.join(DATA, "quality", "frames", engine)


def read_rgb(path):
    """8-bit PNG -> RGB float32 [0,1] (cv2 reads BGR and cannot open the non-ASCII paths here)."""
    img = _imread(path)
    return img.astype(np.float32) / 255.0


def to_u8(rgb):
    return np.clip(rgb * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)


def to_gray(rgb):
    """Luma float32 [0,1] - what the flow estimators and tile pickers actually look at."""
    return cv2.cvtColor(rgb.astype(np.float32), cv2.COLOR_RGB2GRAY)


def triplets():
    """19 (A, GT, B) path triples: A=0001/GT=0002/B=0003, ... - every even frame is a
    true intermediate of its neighbours, so ground truth comes for free."""
    names = png_names()
    return [(names[2 * k], names[2 * k + 1], names[2 * k + 2]) for k in range(19)]
