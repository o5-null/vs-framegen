import hashlib
import os

MEDIA = r"E:\code\dlssg-vfi\media"
SEQ = os.path.join(MEDIA, "seq")
FRAMES = os.path.join(MEDIA, "frames")


def h(p):
    if not os.path.exists(p):
        return "MISSING"
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()[:10]


print("tag  f0/a  f1/gt  f2/b  f3")
for t in range(10):
    tag = "t%02d" % t
    print(tag, h(os.path.join(SEQ, tag + "_f0.rgb")),
          h(os.path.join(FRAMES, tag + "_a.rgb")),
          h(os.path.join(SEQ, tag + "_f1.rgb")),
          h(os.path.join(FRAMES, tag + "_gt.rgb")),
          h(os.path.join(SEQ, tag + "_f2.rgb")),
          h(os.path.join(FRAMES, tag + "_b.rgb")),
          h(os.path.join(SEQ, tag + "_f3.rgb")))

sizes = {os.path.getsize(os.path.join(SEQ, f)) for f in os.listdir(SEQ)}
print("seq file sizes:", sorted(sizes), "count:", len(os.listdir(SEQ)))
