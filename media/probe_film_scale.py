"""Does FILM's 7.3 s/frame come from the model or from the machine?

Runs the same TorchScript FILM model at three sizes. A genuine cost scales roughly with pixel
count; a stuck/contended GPU or an accidental CPU fallback does not.
"""

import sys
import time

import torch

sys.path.insert(0, r"E:\code\VFI\VFI-gui")
from core.torch_backend.vfi_torch.base import VFIConfig, ModelType
from core.torch_backend.vfi_torch.film import FILMModel

ckpt = r"E:\code\VFI\VFI-gui\models\film\film_net_fp32.pt"
model = FILMModel(VFIConfig(model_type=ModelType.FILM, fp16=False))
model.load_model(ckpt)

for (w, h) in ((480, 270), (960, 540), (1920, 1080)):
    a = torch.rand(1, 3, h, w, device="cuda:0")
    b = torch.rand(1, 3, h, w, device="cuda:0")
    with torch.no_grad():
        model.interpolate(a, b, 0.5)  # warmup / allocator
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        model.interpolate(a, b, 0.5)
        torch.cuda.synchronize()
    print("%4dx%-4d  %8.1f ms  (%.2f Mpixel)" % (
        w, h, (time.perf_counter() - t0) * 1000, w * h / 1e6))
