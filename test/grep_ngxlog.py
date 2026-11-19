import sys

path = r"E:\code\dlssg-vfi\work\dlssg_sm86\logs\dlssg_sm86.log"
keys = ("mvec", "MVec", "motion", "Invalid", "invalid", "reject", "rejected", "size", "Size",
        "0xbad00005", "0xBAD00005", "width", "Height")
tail = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
raw = open(path, "rb").read().decode("utf-8", "replace")
print("bytes:", len(raw))
lines = raw.splitlines()
hits = [l for l in lines if any(k in l for k in keys)]
print("hits:", len(hits))
for l in hits[-40:]:
    print(l[:400])
print("---- tail ----")
for l in lines[-30:]:
    print(l[:300])
