"""Probe the 测试数据 set before building the comparison pipeline.

One OpenCV trap on this machine: imread/imwrite cannot open paths containing Chinese
characters, so every image goes through imdecode/fromfile instead.

Answers:
  * where the 40 PNGs sit inside test.mp4 (or whether they are unrelated material)
  * which onnxruntime providers exist (decides how fast rife 4.22 runs)
  * the input/output signature of rife_v4.22.onnx
"""

import os
import subprocess
import sys

import cv2
import numpy as np

DATA = r"E:\code\dlssg-vfi\测试数据"
PNG_DIR = os.path.join(DATA, "1080p")
MP4 = os.path.join(DATA, "test.mp4")


def imread_u(path, flags=cv2.IMREAD_COLOR):
    data = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(data, flags)
    if img is None:
        raise SystemExit("cannot decode " + path)
    return img


def main():
    names = sorted(os.listdir(PNG_DIR))
    first = imread_u(os.path.join(PNG_DIR, names[0]), cv2.IMREAD_UNCHANGED)
    print("png count", len(names), "first", names[0], "shape", first.shape, first.dtype)

    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0",
         "-show_entries", "stream=nb_read_frames,r_frame_rate,duration", "-of", "default=nw=1", MP4],
        capture_output=True, text=True, check=True)
    print("mp4:", probe.stdout.strip().replace("\n", " | "))

    # Locate the PNG sequence in the mp4 by comparing 32x18 grayscale thumbnails of all frames.
    tw, th = 32, 18
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", MP4, "-vf", "scale=%d:%d" % (tw, th),
         "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, check=True).stdout
    thumbs = np.frombuffer(raw, np.uint8).reshape(-1, th, tw).astype(np.float32)
    print("mp4 decoded frames", thumbs.shape[0])

    ref = cv2.resize(cv2.cvtColor(first, cv2.COLOR_BGR2GRAY), (tw, th)).astype(np.float32)
    err = np.abs(thumbs - ref).mean(axis=(1, 2))
    order = np.argsort(err)[:5]
    print("best mp4 matches for 0001.png:", [(int(i), round(float(err[i]), 3)) for i in order])

    best = int(order[0])
    if err[best] < 3.0:
        for k in range(min(4, len(names))):
            rawf = subprocess.run(
                ["ffmpeg", "-v", "error", "-i", MP4, "-vf", "select=eq(n\\,%d)" % (best + k),
                 "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                capture_output=True, check=True).stdout
            if not rawf:
                break
            frame = np.frombuffer(rawf, np.uint8).reshape(1080, 1920, 3).astype(np.int16)
            png = cv2.cvtColor(imread_u(os.path.join(PNG_DIR, names[k])), cv2.COLOR_BGR2RGB).astype(np.int16)
            d = np.abs(frame - png)
            print("mp4[%d] vs %s: max=%d mean=%.4f identical=%s"
                  % (best + k, names[k], d.max(), d.mean(), bool((d == 0).all())))

    import onnxruntime as ort

    print("ort providers:", ort.get_available_providers())
    import onnx

    model_path = r"E:\code\VFI\VFI-gui\models\rife_v2\rife_v4.22.onnx"
    m = onnx.load(model_path)
    print("rife_v4.22 inputs:",
          [(i.name, [d.dim_value or d.dim_param for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
    print("rife_v4.22 outputs:",
          [(o.name, [d.dim_value or d.dim_param for d in o.type.tensor_type.shape.dim]) for o in m.graph.output])
    print("size MB", round(os.path.getsize(model_path) / 1e6, 2))


if __name__ == "__main__":
    sys.exit(main())
