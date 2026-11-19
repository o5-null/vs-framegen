"""Reference interpolator for the DLSS-G spike: ffmpeg minterpolate (MCI/OBMC) on the same triplets.

For every tNN it builds a 2-frame clip from tNN_a.rgb + tNN_b.rgb, interpolates a midpoint with
minterpolate, and reports MAE against the true middle frame tNN_gt.rgb, next to the DLSS-G output
(spike_tNN.ppm) and a plain 50/50 blend.
"""

import os
import subprocess
import sys

MEDIA = os.path.dirname(os.path.abspath(__file__))
FRAMES = os.path.join(MEDIA, "frames")
WORK = os.path.join(os.path.dirname(MEDIA), "work")
W, H = 1920, 1080
FRAME_BYTES = W * H * 3


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n<max>\n
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        raise ValueError("not a P6 ppm: " + path)
    return parts[3]


def mae(a, b):
    if len(a) != len(b):
        raise ValueError("size mismatch %d vs %d" % (len(a), len(b)))
    total = 0
    for i in range(0, len(a), 65536):
        chunk_a = a[i:i + 65536]
        chunk_b = b[i:i + 65536]
        total += sum(abs(x - y) for x, y in zip(chunk_a, chunk_b))
    return total / (len(a) * 255.0)


def minterp(name):
    a = os.path.join(FRAMES, name + "_a.rgb")
    b = os.path.join(FRAMES, name + "_b.rgb")
    pair = os.path.join(WORK, "pair.rgb")
    out = os.path.join(WORK, "pair_out.rgb")
    mid = os.path.join(FRAMES, name + "_minterp.rgb")
    with open(pair, "wb") as f:
        # minterpolate needs more than two frames to emit anything; a,b,b,a also gives it a
        # backward pair, and output frame 1 is the midpoint of a->b.
        for p in (a, b, b, a):
            with open(p, "rb") as g:
                f.write(g.read())
    cmd = [
        "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", "24",
        "-i", pair,
        "-vf", "minterpolate=fps=48:mi_mode=mci:mc_mode=obmc:me_mode=bidir:vsbmc=1",
        "-f", "rawvideo", "-pix_fmt", "rgb24", out,
    ]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as f:
        data = f.read()
    frames = [data[i:i + FRAME_BYTES] for i in range(0, len(data), FRAME_BYTES)]
    if len(frames) < 2:
        raise RuntimeError("%s: minterpolate produced %d frame(s)" % (name, len(frames)))
    with open(mid, "wb") as f:
        f.write(frames[1])


def main():
    names = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    rows = []
    for name in names:
        gt_path = os.path.join(FRAMES, name + "_gt.rgb")
        if not os.path.exists(gt_path):
            continue
        with open(gt_path, "rb") as f:
            gt = f.read()
        with open(os.path.join(FRAMES, name + "_a.rgb"), "rb") as f:
            a = f.read()
        with open(os.path.join(FRAMES, name + "_b.rgb"), "rb") as f:
            b = f.read()
        blend = bytes((x + y) // 2 for x, y in zip(a, b))
        try:
            minterp(name)
            with open(os.path.join(FRAMES, name + "_minterp.rgb"), "rb") as f:
                mi = f.read()
            mi_err = mae(mi, gt)
        except Exception as exc:  # noqa: BLE001 - report and keep going
            print("%s: minterpolate failed: %s" % (name, exc))
            mi_err = float("nan")
        ppm = os.path.join(WORK, "spike_" + name + ".ppm")
        gen_err = mae(read_ppm(ppm), gt) if os.path.exists(ppm) else float("nan")
        print("%s: dlssg=%.5f minterp=%.5f blend=%.5f" % (name, gen_err, mi_err, mae(blend, gt)))
        rows.append((name, gen_err, mi_err, mae(blend, gt)))

    if rows:
        n = len(rows)
        print("---- mean over %d case(s) ----" % n)
        print("dlssg    : %.5f" % (sum(r[1] for r in rows) / n))
        print("minterp  : %.5f" % (sum(r[2] for r in rows) / n))
        print("blend    : %.5f" % (sum(r[3] for r in rows) / n))
        wins = sum(1 for r in rows if r[1] < r[2])
        print("dlssg beats minterp on %d/%d cases" % (wins, n))


if __name__ == "__main__":
    sys.exit(main())
