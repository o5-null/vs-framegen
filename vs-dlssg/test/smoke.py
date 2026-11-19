"""dlss45 引擎包冒烟测试：用包自带的 dlssg2f.exe + sm_86 代理跑一趟 6x。

合成素材：24x24 亮块每输入帧右移 10 px（中心 x = 32 + 10*index），
正确插值时 5 张中间帧的质心应落在 10/(gen+1) 的等分点上。

引擎目录默认 ../bin/dlss45，可用环境变量 DLSSG_ENGINE 或第 5 个参数覆盖。

用法： python smoke.py [W H GEN N] [ENGINE_DIR]
默认： 640 360 5 6  -> 期望 5 对 × 5 = 25 张输出
"""
import os
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
W = int(sys.argv[1]) if len(sys.argv) > 1 else 640
H = int(sys.argv[2]) if len(sys.argv) > 2 else 360
GEN = int(sys.argv[3]) if len(sys.argv) > 3 else 5
N = int(sys.argv[4]) if len(sys.argv) > 4 else 6
ENGINE = (sys.argv[5] if len(sys.argv) > 5
          else os.environ.get("DLSSG_ENGINE", os.path.join(here, "..", "bin", "dlss45")))


def make_frame(index, w, h):
    """RGBA8 帧，亮块中心在 x = 32 + 10*index。"""
    f = np.zeros((h, w, 4), np.uint8)
    f[..., 3] = 255
    cx = 10 * index + 32
    y0, y1 = h // 2 - 12, h // 2 + 12
    f[y0:y1, cx - 12:cx + 12, :3] = 255
    return f


def centroid(raw, w, h):
    g = np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32).mean(axis=2)
    m = g > 64
    if not m.any():
        return float("nan")
    return float(np.nonzero(m)[1].mean())


args = [os.path.join(ENGINE, "dlssg2f.exe"), "--server", str(W), str(H), "--gen", str(GEN)]
payload = b"".join(make_frame(i, W, H).tobytes() for i in range(N))
p = subprocess.run(args, input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
err = p.stderr.decode("utf-8", "replace")
out = p.stdout
mark = out.find(b"DLSSG READY")
print("exit", p.returncode)
if mark < 0:
    print("no handshake in stdout")
    print(out[:400].decode("utf-8", "replace"))
    print(err[-4000:])
    sys.exit(1)
eol = out.find(b"\n", mark)
print("handshake", out[mark:eol].decode("utf-8", "replace"))
body = out[eol + 1:]
print("stdout bytes", len(body), "stderr bytes", len(err))
for line in err.strip().splitlines():
    if "unlocker" in line or "arch" in line or "ready" in line or "done" in line or "error" in line.lower():
        print("  |", line)

fb = W * H * 4
count = len(body) // fb
print("frames out", count, "expected", (N - 1) * GEN)
exp = [32 + 10 * k + 10 * (j + 1) / GEN for k in range(N - 1) for j in range(GEN)]
cent = [centroid(body[i * fb:(i + 1) * fb], W, H) for i in range(count)]
for i, (c, e) in enumerate(zip(cent, exp)):
    print(f"  frame {i:3d} centroid x={c:8.2f} expected {e:6.2f} err {c - e:+7.2f}")
errs = [abs(c - e) for c, e in zip(cent, exp)]
print("max centroid error", max(errs) if errs else "n/a")
