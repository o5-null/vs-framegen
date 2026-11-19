"""Does core.imwri.Read keep 16-bit PNG precision with float_output=1?"""

import os
import sys

import numpy as np

sys.path.insert(0, r"E:\code\dlssg-vfi\test")
import vd_common as vd

import vapoursynth as vs

PATH = os.path.join(vd.RESULTS, "_probe16.png")


def main():
    # One row of known codes: 0, 1, 257, 32768, 65534, 65535
    codes = np.array([[0, 1, 257, 32768, 65534, 65535, 12345, 40000]], np.uint16)
    img = np.repeat(codes[:, :, None], 3, axis=2)  # H=1, W=8, 3 channels
    import cv2

    ok, buf = cv2.imencode(".png", img)
    assert ok
    buf.tofile(PATH)

    core = vs.core
    clip = core.imwri.Read([PATH], firstnum=1, float_output=1)
    print("format:", clip.format.name, clip.width, "x", clip.height)
    frame = clip.get_frame(0)
    plane = np.asarray(frame[0])[0]
    print("codes:", codes[0].tolist())
    print("value*65535:", [round(float(v) * 65535.0, 1) for v in plane])
    print("value*255:", [round(float(v) * 255.0, 2) for v in plane])


if __name__ == "__main__":
    main()
