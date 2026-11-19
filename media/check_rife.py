import os

W = r"C:\Users\benben\tools\vsdlssg-ampere\work"
F = r"C:\Users\benben\tools\vsdlssg-ampere\media\frames"


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def mae(x, y):
    return sum(abs(p - q) for p, q in zip(x, y)) / len(x) / 255.0


for name in ("t00", "t04"):
    a = rd(os.path.join(F, name + "_a.rgb"))
    b = rd(os.path.join(F, name + "_b.rgb"))
    g = rd(os.path.join(F, name + "_gt.rgb"))
    r = rd(os.path.join(W, name + "_rife.rgb"))
    print("%s: rife-a %.5f  rife-b %.5f  rife-gt %.5f  a-b %.5f  a-gt %.5f"
          % (name, mae(r, a), mae(r, b), mae(r, g), mae(a, b), mae(a, g)))
