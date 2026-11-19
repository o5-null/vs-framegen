"""Driver for the SmoothMyVideo offline host (smv-live.exe --offline).

The host loads the model once, then reports its own steady-state cost at the end:
    offline: 40 frames in, 79 out, 39 pairs, 1.23 s, 31.5 ms per pair
That number starts after the model is ready, so it is the per-pair generation
cost and not the process start-up (which is 0.8-3.2 s and would swamp a short clip).
"""

import os
import re
import subprocess

import common as C

FINAL = re.compile(r"offline: (\d+) frames in, (\d+) out, (\d+) pairs, ([\d.]+) s, ([\d.]+) ms per pair")


def _cmd(flags, w, h, frames, out_pixfmt):
    return [C.SMV_EXE, "--offline", "--w", str(w), "--h", str(h), "--multi", "2",
            "--frames", str(frames), "--pixfmt", "rgb24", "--out-pixfmt", out_pixfmt,
            "--cache", C.SMV_CACHE] + flags


def _env():
    env = dict(os.environ)
    env.pop("SMV_DLSSG_LIMIT", None)  # a leftover limit would cap the multiplier
    return env


def run(flags, raw, w, h, frames, out_file=None, timeout=3600):
    """Feed a raw RGB24 file through one backend.

    stdout goes to out_file (quality run) or to the null device (throughput run):
    the host's own timing line on stderr is all the throughput pass needs.
    """
    os.makedirs(C.TMP, exist_ok=True)
    with open(raw, "rb") as fin:
        if out_file:
            os.makedirs(os.path.dirname(out_file), exist_ok=True)
            fout = open(out_file, "wb")
        else:
            fout = open(os.devnull, "wb")
        try:
            p = subprocess.run(_cmd(flags, w, h, frames, "rgb24"), cwd=C.SMV_ENGINE,
                               stdin=fin, stdout=fout, stderr=subprocess.PIPE,
                               env=_env(), timeout=timeout)
        finally:
            fout.close()
    err = p.stderr.decode("utf-8", "replace")
    m = FINAL.search(err)
    info = {
        "exit": p.returncode,
        "pairs": int(m.group(3)) if m else 0,
        "seconds": float(m.group(4)) if m else 0.0,
        "ms_per_pair": float(m.group(5)) if m else 0.0,
        "frames_out": int(m.group(2)) if m else 0,
        "log": err.strip(),
    }
    return info


def read_frames(path, w, h, count):
    """Split a raw RGB24 capture back into HxWx3 frames."""
    import numpy as np

    size = w * h * 3
    with open(path, "rb") as fh:
        data = fh.read()
    n = len(data) // size
    return np.frombuffer(data[:n * size], np.uint8).reshape(n, h, w, 3), n
