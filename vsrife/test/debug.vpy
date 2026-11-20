import sys
import vapoursynth as vs

print("[t] start", flush=True)
core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")
print("[t] plugin loaded", flush=True)

src = core.std.BlankClip(width=1920, height=1080, format=vs.YUV420P8, length=8, fpsnum=24000, fpsden=1001)
print("[t] blank clip", flush=True)

with open(r"C:\Users\benben\AppData\Local\Temp\opencode\rifemodels\x\rife_v4.26.onnx", "rb") as f:
    model = f.read()
print("[t] model", len(model), flush=True)

out = core.rife_native.Rife(clip=src, model=model, factor=2, sc=0.0)
print("[t] filter created", out.num_frames, out.width, out.height, flush=True)
out.set_output()
print("[t] output set", flush=True)
