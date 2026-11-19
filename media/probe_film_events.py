"""Re-time FILM at 1080p with CUDA events, and report free VRAM.

The perf_counter result (10.2 s at 1080p, 0.44 s at 540p) is not plausible for a model of this
size, so either the card is nearly full and thrashing, or a host-side op blocks for seconds.
CUDA events time only what the GPU executes.
"""

import sys

import torch

sys.path.insert(0, r"E:\code\VFI\VFI-gui")
from core.torch_backend.vfi_torch.base import VFIConfig, ModelType
from core.torch_backend.vfi_torch.film import FILMModel

print("device:", torch.cuda.get_device_name(0))
free, total = torch.cuda.mem_get_info()
print("vram free %.0f MB / total %.0f MB" % (free / 2**20, total / 2**20))

model = FILMModel(VFIConfig(model_type=ModelType.FILM, fp16=False))
model.load_model(r"E:\code\VFI\VFI-gui\models\film\film_net_fp32.pt")

a = torch.rand(1, 3, 1080, 1920, device="cuda:0")
b = torch.rand(1, 3, 1080, 1920, device="cuda:0")

for rep in range(3):
    start = torch.cuda.Event(enable_timing=True)
    end = torch.cuda.Event(enable_timing=True)
    with torch.no_grad():
        model.interpolate(a, b, 0.5)  # warmup
        torch.cuda.synchronize()
        start.record()
        model.interpolate(a, b, 0.5)
        end.record()
        torch.cuda.synchronize()
    print("rep %d: gpu %.1f ms  vram used %.0f MB" % (
        rep, start.elapsed_time(end), torch.cuda.memory_allocated() / 2**20))
