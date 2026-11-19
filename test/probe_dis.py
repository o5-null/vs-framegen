"""Which DIS knobs does this cv2 build expose, and what does a max-quality setting cost?"""

import time

import numpy as np
import cv2

print("cv2", cv2.__version__)
print("preset constants:", [n for n in dir(cv2) if "DISOpticalFlow" in n or "DISOPTICAL" in n.upper()])
dis = cv2.DISOpticalFlow_create(cv2.DISOpticalFlow_PRESET_MEDIUM)
print("defaults:", dis.getFinestScale(), dis.getPatchSize(), dis.getPatchStride(),
      dis.getGradientDescentIterations(), dis.getVariationalRefinementIterations(),
      dis.getVariationalRefinementAlpha(), dis.getVariationalRefinementDelta(),
      dis.getVariationalRefinementGamma())

a = np.random.randint(0, 255, (1080, 1920), np.uint8)
b = np.roll(a, 3, axis=1)
for name, fin, gd, vr in (("medium", 2, 12, 5), ("max", 0, 25, 12)):
    d = cv2.DISOpticalFlow_create(cv2.DISOpticalFlow_PRESET_MEDIUM)
    d.setUseSpatialPropagation(True)
    d.setFinestScale(fin)
    d.setGradientDescentIterations(gd)
    d.setVariationalRefinementIterations(vr)
    t0 = time.perf_counter()
    f = d.calc(a, b, None)
    print("%s: %.2f s  mean|f|=%.2f" % (name, time.perf_counter() - t0,
                                        float(np.mean(np.hypot(f[:, :, 0], f[:, :, 1])))))
