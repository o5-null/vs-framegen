import numpy as np

path = "fsr4_vk_probe.ppm"
with open(path, "rb") as f:
    data = f.read()
header_end = data.index(b"255\n") + 4
w, h = 1920, 1080
img = np.frombuffer(data[header_end:], dtype=np.uint8).reshape(h, w, 3)[:, :, 0].astype(np.float32) / 255.0
print("global mean", round(float(img.mean()), 4), "std", round(float(img.std()), 4))
for row in (0, 1, 2, 100, 540, 900, 1077, 1078, 1079):
    print("row", row, "mean", round(float(img[row].mean()), 3), "std", round(float(img[row].std()), 3))
print("row0 vs row540 mad", round(float(np.abs(img[0] - img[540]).mean()), 4))
print("row0 vs row1079 mad", round(float(np.abs(img[0] - img[1079]).mean()), 4))
print("col mean", [round(float(img[:, x].mean()), 3) for x in range(0, 64, 8)])
print("col std", [round(float(img[:, x].std()), 3) for x in range(0, 64, 8)])
