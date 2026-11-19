"""Shared helpers for the 测试数据 experiments.

Conventions: every array this module hands out is **RGB** float32 in [0,1] unless the name says
otherwise. OpenCV reads and writes BGR, so the conversion happens here, once. Non-ASCII paths are
handled with buffer IO because cv2.imread/imwrite cannot open them.
"""

import os

import numpy as np

DATA = r"E:\code\dlssg-vfi\测试数据"
PNG_DIR = os.path.join(DATA, "1080p")
RESULTS = os.path.join(DATA, "results")
PLUGIN = r"E:\code\dlssg-vfi\vs-dlssg\bin\vdlssg.dll"
PROXY = r"E:\code\dlssg-vfi\work"

METHODS = ("rife422", "dlssg", "dlssg_mv", "dlssg_mvenc")


def load_plugin(core, path):
    """Load a VS plugin unless an autoloaded copy of it is already registered."""
    try:
        core.std.LoadPlugin(path=path)
    except Exception as exc:  # "already loaded" is the expected case for vsrepo plugins
        if "already loaded" not in str(exc):
            raise


def png_names():
    """All 40 source frames in order."""
    return sorted(os.path.join(PNG_DIR, n) for n in os.listdir(PNG_DIR) if n.endswith(".png"))


def triplets():
    """19 (A, GT, B) path triples: A=0001/GT=0002/B=0003, A=0003/GT=0004/B=0005, ...

    The 40 frames are consecutive, so every even frame is a true intermediate of its neighbours:
    that gives ground truth for free, which is what makes the flow comparison meaningful.
    """
    names = png_names()
    return [(names[2 * k], names[2 * k + 1], names[2 * k + 2]) for k in range(19)]


def _decode(path, flags):
    import cv2

    buf = np.fromfile(path, np.uint8)
    img = cv2.imdecode(buf, flags)
    if img is None:
        raise RuntimeError("cannot decode " + path)
    return img


def _encode(path, img):
    import cv2

    os.makedirs(os.path.dirname(path), exist_ok=True)
    ext = os.path.splitext(path)[1]
    ok, buf = cv2.imencode(ext, img)
    if not ok:
        raise RuntimeError("cannot encode " + path)
    buf.tofile(path)


def read_rgb(path):
    """8-bit source PNG -> RGB float32 [0,1]."""
    return _decode(path, 1)[:, :, ::-1].astype(np.float32) / 255.0


def read_rgb16(path):
    """16-bit intermediate -> RGB float32 [0,1]. 1/65535 is far below the differences we measure."""
    return _decode(path, -1)[:, :, ::-1].astype(np.float32) / 65535.0


def write_rgb16(path, rgb):
    """Save an interpolated frame losslessly enough for metrics, without 25 MB of float32."""
    q = np.clip(rgb * 65535.0 + 0.5, 0.0, 65535.0).astype(np.uint16)
    _encode(path, q[:, :, ::-1])


def write_rgb8(path, rgb):
    q = np.clip(rgb * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    _encode(path, q[:, :, ::-1])


def write_gray8(path, gray):
    _encode(path, np.clip(gray * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8))


def to_gray(rgb):
    """Luma float32 in [0,1]: what the dense flow estimators actually see."""
    import cv2

    return cv2.cvtColor(rgb.astype(np.float32), cv2.COLOR_RGB2GRAY)


def to_u8(rgb):
    return np.clip(rgb * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)


def gt_name(gt_path):
    """'0002.png' - output frames are named after the ground-truth frame they stand in for."""
    return os.path.basename(gt_path)


def frame_path(method, gt_path):
    return os.path.join(RESULTS, method, "frames", gt_name(gt_path))
