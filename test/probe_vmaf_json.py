import json

p = r"E:\code\dlssg-vfi\work\vseq\vmaf_rife422.json"
d = json.load(open(p, encoding="utf-8"))
print("top:", list(d))
print("pooled:", list(d.get("pooled_metrics", {})))
print("frames:", len(d.get("frames", [])))
