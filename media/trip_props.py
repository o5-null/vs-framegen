"""Why does the plugin diverge from the spike on some triplets?

Prints the generated frame's own properties (_DLSSGFallback tells us whether the frame was
generated at all) plus how far each candidate is from the true middle frame.
"""

import os

import numpy as np
import vapoursynth as vs

MEDIA = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(MEDIA)
FRAMES = os.path.join(MEDIA, "frames")
TRI = os.path.join(ROOT, "work", "tri")

core = vs.core
core.std.LoadPlugin(path=os.path.join(ROOT, "plugin", "vdlssg.dll"))


def rgb(path):
    return np.fromfile(path, dtype=np.uint8)


def mae(x, y):
    n = min(x.size, y.size)
    return float(np.abs(x[:n].astype(np.int16) - y[:n].astype(np.int16)).mean()) / 255.0


def as_rgb(frame):
    out = []
    for p in range(3):
        plane = np.asarray(frame[p], dtype=np.float32)
        out.append(np.clip(plane * 255.0 + 0.5, 0, 255).astype(np.uint8).ravel())
    return np.concatenate(out)


def main():
    tags = sorted(n[:3] for n in os.listdir(FRAMES) if n.endswith("_a.rgb"))
    for tag in tags:
        clip = os.path.join(TRI, tag + ".mkv")
        if not os.path.exists(clip):
            print("%s: no clip" % tag)
            continue
        src = core.resize.Bicubic(core.ffms2.Source(clip), format=vs.RGBS)
        out = core.vdlssg.FrameGen(src, proxy_dir=os.path.join(ROOT, "work"), log=0)
        f = out.get_frame(1)
        props = f.props
        a = rgb(os.path.join(FRAMES, tag + "_a.rgb"))
        gt = rgb(os.path.join(FRAMES, tag + "_gt.rgb"))
        gen = as_rgb(f)
        print(
            "%s: fallback=%s cut=%s gen=%.6f(a) %.6f(gt) | a-vs-gt=%.6f"
            % (
                tag,
                props.get("_DLSSGFallback"),
                props.get("_DLSSGSceneCut"),
                mae(gen, a),
                mae(gen, gt),
                mae(a, gt),
            )
        )


if __name__ == "__main__":
    main()
