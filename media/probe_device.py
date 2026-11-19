"""Check that the VFI-gui models really run on the GPU.

vfi_models.py prints model.device, which is a property of torch.cuda.is_available() and says
nothing about where the weights actually live. This reads the weights directly.
"""

import sys

import torch

sys.path.insert(0, r"E:\code\VFI\VFI-gui")
from core.torch_backend.vfi_torch.base import VFIConfig, ModelType
from core.torch_backend.vfi_torch.film import FILMModel

ckpt = r"E:\code\VFI\VFI-gui\models\film\film_net_fp32.pt"
model = FILMModel(VFIConfig(model_type=ModelType.FILM, fp16=False))
model.load_model(ckpt)
m = model._model
try:
    dev = next(m.parameters()).device
except StopIteration:
    dev = "no parameters (pure TorchScript constants?)"
print("param device:", dev)
print("is ScriptModule:", isinstance(m, torch.jit.ScriptModule))
print("cuda allocated MB:", torch.cuda.memory_allocated() / 2**20)

a = torch.rand(1, 3, 1080, 1920, device="cuda:0")
b = torch.rand(1, 3, 1080, 1920, device="cuda:0")
model.interpolate(a, b, 0.5)
torch.cuda.synchronize()
import time
t0 = time.perf_counter()
with torch.no_grad():
    model.interpolate(a, b, 0.5)
torch.cuda.synchronize()
print("one forward: %.1f ms" % ((time.perf_counter() - t0) * 1000))
