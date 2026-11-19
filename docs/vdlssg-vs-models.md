# DLSS-G 与 VFI-gui 模型的质量校准

同一批十组三帧（Sintel 1080p，`E:\code\dlssg-vfi\media\frames\tNN_{a,gt,b}.rgb`），
用**真中间帧**当参考，把 vdlssg 插件和多版本 VFI 模型放在一起比较。

参考帧来自原始 24 fps 素材（`media\sintel_trailer_1080p.mp4`，1920×1080，1253 帧）：
每组取奇数帧对 a=(2k)、b=(2k+2)，gt=(2k+1) —— 也就是把物料降到 12 fps 再插回 24 fps，
gt 是真实存在的画面，不是人工合成的目标。

## 1. 结论

| 方法 | MAE ↓ | VMAF ↑ | 单帧耗时 | 后端 |
|---|---|---|---|---|
| FILM | 0.004186 | **81.05** | 7.3 s（环境异常，见 §5） | TorchScript / CUDA |
| IFRNet-vimeo | 0.004214 | 80.73 | 3.09 s | ONNX / CPU EP |
| rife47 | 0.003943 | 79.74 | 25.5 ms | PyTorch fp16 / CUDA |
| rife49（4.7 架构载 4.9 权重） | 0.003920 | 79.68 | 26.7 ms | 同上 |
| rife426 | 0.004398 | 79.65 | 27.3 ms | 同上 |
| **dlssg 2×（本插件）** | **0.00411** | **77.11** | **GPU 1.36 ms** | NGX DLSS-G |
| minterpolate | 0.00502 | 74.35 | — | ffmpeg |
| IFRNet-gopro | 0.005889 | 68.98 | 3.09 s | ONNX / CPU EP |
| 50/50 blend | 0.00707 | 57.69 | — | 线性混合 |

读法：

- **质量**：DLSS-G 落后最好的 RIFE 变体约 2.6 VMAF（77.11 / 79.7），落后 FILM 与
  IFRNet-vimeo 约 3.9 / 3.6 VMAF；但仍显著高于 minterpolate（+2.8）与 blend（+19.4）。
- **MAE 与 VMAF 会打架**：FILM 的 MAE（0.004186）比 rife47（0.003943）差，但 VMAF 高 1.3 分
  （81.05 vs 79.74）；rife426 的 MAE（0.004398）比 dlssg（0.004109）差，VMAF 却高 2.5 分。
  MAE 奖励模糊，判断插帧质量必须以 VMAF 为主、MAE 为辅。
- **速度**：这是 DLSS-G 唯一压倒性的维度 —— 1.36 ms/帧（1080p）对 25 ms（RIFE）。
  4K 上 3.9 ms/帧，2160p 2× 实测 55 fps（见 `vdlssg-throughput.md`）。

## 2. 逐组数据

| 组 | 内容 | rife47 | rife49 | rife426 | dlssg 2× | FILM | IFRNet-vimeo |
|---|---|---|---|---|---|---|---|
| t00 | 人物近景，鼻环细结构 | 0.005805 / 87.98 | 0.005015 / 89.90 | 0.008445 / 81.78 | 0.004345 / 81.83 | 0.005564 / 88.53 | 0.005836 / 90.33 |
| t01 | 慢移 | 0.003688 / 84.94 | 0.003623 / 83.91 | 0.003956 / 85.02 | 0.003583 / 80.62 | 0.003745 / 86.48 | 0.003476 / 85.47 |
| t02 | 中景 | 0.005519 / 79.23 | 0.005047 / 82.17 | 0.005721 / 84.06 | 0.005186 / 76.92 | 0.004430 / 87.09 | 0.004207 / 87.00 |
| t03 | 近乎静止 | 0.000003 / 97.43 | 0.000001 / 97.43 | 0.000004 / 97.43 | 0.000001 / 97.43 | 0.000019 / 97.43 | 0.000034 / 97.43 |
| t04 | **快速摇镜** | 0.009625 / 52.13 | 0.010679 / 48.72 | 0.009708 / 52.21 | 0.013097 / 41.53 | 0.012979 / 48.63 | 0.013507 / 47.03 |
| t05 | 中景 | 0.002877 / 89.58 | 0.002907 / 88.41 | 0.002859 / 89.87 | 0.003339 / 85.77 | 0.003384 / 90.80 | 0.003293 / 89.00 |
| t06 | **快速摇镜 + 细节** | 0.008845 / 23.79 | 0.008966 / 23.25 | 0.010221 / 22.67 | 0.008782 / 24.61 | 0.008919 / 24.50 | 0.009049 / 23.68 |
| t07 | 静止 | 0.002737 / 89.99 | 0.002634 / 91.27 | 0.002739 / 91.19 | 0.002460 / 89.69 | 0.002497 / 92.84 | 0.002405 / 92.91 |
| t08 | 近乎静止 | 0.000310 / 95.55 | 0.000304 / 95.16 | 0.000307 / 95.61 | 0.000282 / 95.66 | 0.000290 / 96.72 | 0.000290 / 96.81 |
| t09 | 近乎静止 | 0.000020 / 96.73 | 0.000024 / 96.53 | 0.000019 / 96.64 | 0.000014 / 97.00 | 0.000031 / 97.47 | 0.000040 / 97.62 |
| 均值 | | 0.003943 / 79.74 | 0.003920 / 79.68 | 0.004398 / 79.65 | 0.004109 / 77.11 | 0.004186 / 81.05 | 0.004214 / 80.73 |

dlssg 的逐组 MAE 取自 `test\parity2.py` 的十组窗口，逐组 VMAF 取自
`docs\dlssg-on-3080-spike.md` 的同一批十组（同一组素材、同一份 spike 输出，插件输出与
spike 逐位相同，所以两张表可以对齐）。**窗口纪律**：同一份数据里 dlssg 另有一个
`parity3 start=300` 的十六组窗口（MAE 0.006143），与这里的十组不可混用。

共同规律：所有方法在 t06 都崩到 24 分左右（快速摇镜 + 高频细节，几乎没有可插值的信息），
t04 也一样；差别只在崩到什么程度 —— dlssg 在 t04 掉得最深（41.53 vs rife 52.13），
这正是它需要自算光流而运行库对**大位移**估计更保守的体现。

## 3. 复现命令

```bat
:: VFI-gui PyTorch 路径（RIFE 三版本）
E:\code\VFI\.venv\Scripts\python.exe media\rife_vfigui.py ^
    --version 4.7 --ckpt E:\code\VFI\VFI-gui\models\rife\rife47.pth --tag rife47

:: 其它 PyTorch 模型（film / ifrnet / amt）
E:\code\VFI\.venv\Scripts\python.exe media\vfi_models.py ^
    --model film --ckpt E:\code\VFI\VFI-gui\models\film\film_net_fp32.pt --tag film

:: ONNX 模型（自带 edge padding，见 §4）
E:\code\VFI\.venv\Scripts\python.exe media\onnx_vfi.py ^
    --ckpt E:\code\VFI\VFI-gui\models\ifrnet\ifrnet-vimeo.onnx --tag ifrnet_vimeo

:: DLSS-G 插件
E:\code\VFI\.venv\Scripts\python.exe test\parity2.py
```

每个脚本末行都是 `---- <tag> mean: mae=%.6f vmaf=%.2f  %.1f ms ----`。

## 4. 测量陷阱（三个都会伪造结论）

### 4.1 ONNX 模型自己 zero-pad 到 32 的倍数

IFRNet 的 ONNX 图内部把 1080 行补到 1088 行，**补的是黑边**。那 8 行黑色被 encoder 当成
硬边，再经 decoder 抹回画面。

`media\onnx_pad_probe.py --ckpt ...\ifrnet-vimeo.onnx --case t07`（喂 a,a 让模型自插）：

| 输入准备 | 自插残差 |
|---|---|
| raw 1080 行 + 居中裁剪 1088 行输出 | 0.004699（最佳偏移 4 行） |
| **edgepad**（复制末行补 8 行，插完再裁） | **0.000106** |
| 只取 1056 行（已对齐） | 0.000110 |

修正前 ifrnet-vimeo 十组 VMAF **60.86**，修正后 **80.73** —— 近 20 分全是 padding 伪影。
`media\onnx_align.py` 印证：残差最小值出现在 offset=4（1080 的半补位），而 1080%32=24，
padder 上下各补 4 行。

→ `media\onnx_vfi.py` 现在入口就 `pad_h = (-H) % 32`、`pad_w = (-W) % 32`，用**末行复制**
补齐，输出再裁回 `[:H, :W]`。任何新加 ONNX 模型都要先跑 `onnx_pad_probe.py` 验一遍。

对照：走 PyTorch 路径的模型没有这个问题。`media\probe_selfconsistency.py` 喂 a,a：

| 模型 | 自插 MAE | 输出尺寸 | 首 8 行 / 末 8 行残差 |
|---|---|---|---|
| FILM | 0.000249 | 1080×1920 | 0.00000 / 0.00000 |
| rife47 | 0.000228 | 1080×1920 | 0.00000 / 0.00000 |

即 2.5e-4 量级的自洽误差完全来自**运动分支在零位移时的不完美**（中间 64 行也有 0.0003），
不是边界伪影。

### 4.2 本机 onnxruntime 只有 CPU EP

`media\ort_providers.py` 输出 `['AzureExecutionProvider', 'CPUExecutionProvider']`
（onnxruntime 1.24.4）。所以 ONNX 那 3.09 s/帧是 **CPU 时间**，不能与 DLSS-G / RIFE 的
GPU 数字并列，只能作质量参考。要拿 ONNX 的吞吐必须自己编 `onnxruntime-gpu` 或用
`onnxruntime-genai` / TRT EP。

### 4.3 FILM 的 7.3 s/帧不是模型性质

三个独立探针都指向"机器被占用"，不是算力：

- `media\probe_device.py`：`param device: cuda:0`、`is ScriptModule: True`、
  `cuda allocated MB: 133.7` —— 权重确实在 GPU 上，没有静默回落 CPU。
- `media\probe_film_events.py`：`vram free 9097 MB / total 10240 MB`（显存充足）；
  CUDA event 三次重复 **7723.8 / 7363.3 / 7317.6 ms** —— 稳定地慢，不是抖动。
- `media\probe_film_scale.py`：480×270 → 617 ms、960×540 → **443 ms**、1920×1080 → 10231 ms。
  **540p 比 270p 还快**，完全不单调 —— 真实的算力曲线不可能这样。

本机桌面进程常占 GPU 95%（`nvidia-smi` 可见），这类计时没有意义。FILM 的
MAE/VMAF 可信（逐位确定性），耗时不可用；要评 FILM 吞吐得在空闲卡上跑。

## 5. VFI-gui 里跑不起来的模型（不是我们脚本的问题）

| 模型 | 症状 | 根因 |
|---|---|---|
| `amt-g.pth` | `RuntimeError: Given groups=1, weight of size [144, 260, 3, 3], expected input[1, 388, 136, 240] to have 260 channels, but got 388 channels instead` | `AMT_ARCH_CONFIGS` 把 `amt-g` 映射到 `AMT_S(num_flows=5)`；`load_state_dict(strict=False)` 静默吞掉不匹配，前向才在 `vfi_torch\amt\__init__.py:226` 的 decoder3 崩 |
| `IFRNet_S_Vimeo90K.pth` | `RuntimeError: Error(s) in loading state_dict for IFRNet_S` | 权重键名是 `encoder.pyramid1.0.0.weight` / `decoder1.convblock.1.conv1.0.weight`，而 `vfi_torch\ifrnet\__init__.py` 的 `IFRNet_S` 期望 `encoder.conv1.weight`。**VFI-gui 的 IFRNet PyTorch 实现与它自带的权重架构不符** |
| `rife417.pth` | `size mismatch … [96, 23, 3, 3] … [96, 15, 3, 3]` | 23 通道权重 vs 15 通道架构（4.17 的 head 不同） |
| `--ensemble` | 加与不加输出**逐位相同** | VFI-gui 的实现是空操作 → 不可声称"跑过 ensemble 版" |

`vfi_torch\` 只有 4 类实现（`MODEL_REGISTRY = {RIFE, FILM, IFRNET, AMT}`），
其余 10 类（XVFI/ATM/MoMo/M2M/stmfnet/FLAVR/cain/sepconv/…）只在
`core\backends\adapters\` 的 ONNX/TRT 侧，本机没有对应权重或没有跑通的 EP。

## 6. 对 DLSS-G 的定性判断

- **质量定位**：中间档。比传统算法（minterpolate/blend）好得多，比所有现代 VFI 网络
  差 2.6–3.9 VMAF。差距的形态很具体：ts 全在**大位移快摇**（t04）和**高频细节**
  （t00 鼻环重影、t06），即光流精度不足，而不是色彩/亮度偏差（静止组 t03/t08/t09 全部 96+ 分）。
- **速度定位**：唯一能实时 4K 2× 的方案（3.9 ms/帧 GPU），比最快的 RIFE 变体快近一个数量级。
- **用途**：24 fps 素材实时预览/直播、或 GPU 档期紧张时批量预处理；追求成片质量仍应用 RIFE。
- **可调性差距**：DLSS-G 插值时刻锁死在等分点（2× 只能 0.5，3× 只能 1/3、2/3），
  而 RIFE/FILM 可给任意 timestep —— 变速、升格这类用法 DLSS-G 做不到。
