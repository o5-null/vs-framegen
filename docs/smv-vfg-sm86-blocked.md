# SmoothMyVideo 的 NVIDIA VFG 在 RTX 3080 上加载失败（status -1999）

日期：2026-11-18 · 结论：**硬件门，不是配置问题，也无法像 DLSS-G 那样补内核解锁**

## 现象

`SmoothMyVideo.exe` 渲染 `测试数据\test.mp4` → 100 fps（4×）+ Sharpen 1，插帧后端选
「NVIDIA Video Frame Generation」时：

```
[native] native: vfg: the model's load failed: unknown error (status -1999)
[native] native: vfg session: 0 tweens
native offline host failed (exit 2)
```

上采样（DLSS dlaa）和编码（hevc_nvenc）都正常，**只有 VFG 的加载这一步失败**：
`nativeVfgSetup()` 里 create / setStream / InputWidth / InputHeight / Mode /
AutomaticShotChangeDetectionEnabled 全部返回 0，唯独 `NvVFX_Load()` 返回 -1999，
`NvCV_GetErrorStringFromCode(-1999)` 查不到这个码，于是打印 `unknown error`。

## 根因：模型 DLL 只带 sm_89 与 sm_120 的内核

`engine\nvvfx\nvVFXVideoFrameGeneration.dll`（207,321,712 B，来自 nvidia-vfx 0.2.0.0
wheel，Video Effects SDK 1.3.0）静态取证：

| 取证项 | 结果 |
| --- | --- |
| 门禁字符串（偏移 240,960） | `GPU SM %d is not supported by this Video Frame Generation build` |
| NVRTC 编译选项串 | 62 处，全部 `-arch sm_120`（如 `-e model_medium_conv1 -v -disable-optimizer-consts -arch sm_120 -m 64`），无 sm_86/89/90 |
| 内嵌 ELF(cubin) | 124 个，e_flags 只有两种：`0x590559`(sm_89) 与 `0x6007802`(sm_120)，交替出现 |
| PTX（可 JIT 源） | 无 `.version` 串 → 没有 PTX 后备路线 |
| 模型命名 | `model_small_*` / `model_medium_*` 的 conv/attn/out/depth_to_space/uncertainty_p95，部分带 `_fp8` 后缀 |

RTX 3080 是 sm_86：既没有对应 cubin，也没有 PTX 可编译，于是架构检查直接拒绝，
返回一个未登记的状态码 -1999（VFG 自己的错误表里没有它，所以成了 `unknown error`）。

旁证：app 自己也写明了 —— `README.md:100` "Needs an RTX 40 or 50"、
`docs\DEVELOPMENT.md:401-408` "NVIDIA lists RTX 40 and 50 GPUs for this model"、
UI 文案 `nvvfg.hint` "（NVIDIA 为视频打造的 AI 帧生成，任意帧率；RTX 40 和 50）"。

## 为什么这个选项还能勾上

app 的可用性判定（`renderer/app.js`）只看文件存在：

```js
let vfgReady = false;
function modelIsVfg() { return $('modelnvvfg').checked && vfgReady; }
// refreshVfgState() → ipcRenderer.invoke('nvvfg-ready')
```

`nvvfg-ready` 只检查 `engine/nvvfx` 下四个文件（`NVVideoEffects.dll`、`NVCVImage.dll`、
`nvVFXVideoFrameGeneration.dll`、`cudart64_12.dll`）是否齐；`dist/render/native.js` 同样
只按文件名比对。**没有 GPU 能力检查**，所以 3080 上四文件下载齐了就能选中，直到运行才炸。

## 为什么不能像 DLSS-G 那样解锁

`docs/smv-dlssg-unlock.md` 的解锁能成，是因为 `dlssg_for_sm86` 提供了**真实的 sm_86 kernel
pack**（`fg_kernels.dll`，manifest `fg-kernels 310.9.16 7710da62f151`，71/71 内核命中）。
VFG 这边：

1. 没有 sm_86 内核 —— 改掉门禁只会把错误换成 `CUDA_ERROR_NO_BINARY_FOR_GPU`；
2. 没有 PTX —— 连“让驱动现编”这条退路都没有；
3. 模型走 FP8（`_fp8` 内核、`-arch sm_120`）—— Ampere 没有 FP8 张量核，
   即使骗过门禁，数值路径也不成立。

要真解锁得拿到 NVIDIA 的模型源码重编到 sm_86。"补内核"这条路线确实走不通。

## 修正：还有第二条路线（伪装架构 + 改内核标记）

2026-10-17 有人在 GitHub 上放了 `Xchen991/RTX30-Broadcast-FrameGen`（MIT，v1.0.0，
`version.dll` 105,984 B），目标是 **NVIDIA Broadcast 的 Video Frame Generation**，同样是
`nvVFXVideoFrameGeneration.dll`，同样在 RTX 30 / sm_86 上解锁。它的做法不是补内核，而是：

1. 监视线程轮询 `GetModuleHandleW(L"nvVFXVideoFrameGeneration.dll")`，模块出现后扫它的
   IAT（作者按区间 `base+0x3B000 .. base+0x3B800` 扫），命中两个槽就替换：
   - `cuDeviceGetAttribute` → 当 `attrib == 76`（`CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR`）
     且返回 6 时改成 9 —— 把设备伪装成 8.9（Ada）；
   - `cuModuleLoadData` → 加载前就地改内核镜像：fatbin（magic `0xBA55ED50`）里
     `pSm == 89`（cubin 头 +28）改成 86，ELF 段里 `e_flags & 0xFF == 89` 改成
     `0x00560556`（sm_86 的 e_flags）。
2. patch `NvBroadcastCore.dll` 的 UI 灰化：12 字节 `81 BB B8 BD 00 00 60 01 00 00 7C 04`
   → `C7 83 B8 BD 00 00 B0 01 00 00 90 90`。

也就是说 Ada 的 SASS 直接被当成 sm_86 加载运行，NVIDIA 没保证、但作者实测出帧。

**这份 DLL 与上面 patch 的契合点是逐位对得上的**（本机核对）：

- 我们的 cubin `e_flags = 0x00590559`，低字节 `0x59 = 89` —— 正是它 patch 的那个字节；
  它写的 `0x00560556` 就是同一套 `(sm<<16)|(0x05<<8)|sm` 编码换成 86。
- 我们的 `nvVFXVideoFrameGeneration.dll` 导入表里静态导入 `nvcuda.dll` 34 个符号，
  **包含 `cuDeviceGetAttribute` 和 `cuModuleLoadData`**，且没有 NVRTC / `cuLink*` 导入
  （`work\smv\diag_pe_imports.py` 可复现），hook 点与 Broadcast 那份完全一致。
- 62 个 sm_89 内核 = Ada 的 fp16 路径；`_fp8` 那套只在 sm_120 镜像里，本来也用不上。

差别只在门禁位置：Broadcast 靠 UI 判断可用性（所以要 patch `NvBroadcastCore.dll`），
SMV 的门禁在 DLL 内部（`GPU SM %d is not supported by this Video Frame Generation build`），
绕过它只需两个 hook，不需要改 UI。

注入点：VFG 宿主是 `engine\live\smv-live.exe --offline`（`native.js` 的 `NATIVE_EXE`）。
它导入 `WINMM.dll` 而不导入 `version.dll`，所以代理要用 `winmm.dll`（或改导入表），不能照搬
`version.dll` 的名字。

未验证项：Ada SASS 在 Ampere 上是否全部指令可用、`-1999` 那条门禁是否只比对 minor、
跑起来是否出画质/崩溃问题。证据等级：作者自证（README 有前后截图），全网仅此一例，
1 star / 1 fork / 0 issue，无第三方复现。

## 可用的替代（本机已验证）

同一台机器上 DLSS-G 解锁链路是好的，`engine\dlssg\dlssg_sm86\logs\dlssg_sm86.log`
今天（12:44–12:52）那次运行：

- `backend_loaded` → `dlssg-310.9.16-d3d12-sm86`，`max_generated_frames: 5`
- `install` → `actual_sm: 86`、`executed_on_sm86_hardware: true`、`gpu: NVIDIA GeForce RTX 3080`
- `kernel_pack` → `kernels_matched: 71`、`kernels_unused: 0`
- `fg_gate_exit` → `create_failures: 0`、`evaluates: 6751`、`clean: true`

所以：把插帧后端换成 **DLSS 4.5（DLSSG）** 即可（6× 已验证），或用
NVIDIA Smooth Motion（`fruc`，NvOFFRUC 走硬件光流）、RIFE、GMFSS。

## 复现/取证脚本

- `work\smv\diag_vfg_strings.py` —— 扫 DLL 的 ASCII/UTF16 字符串、ELF/PTX 记号
- `work\smv\diag_vfg_sm.py` —— 定位门禁字符串邻域、统计 `sm_XX`、列 cubin 偏移与 e_flags
- `work\smv\diag_asar_vfg.py` —— 解 app.asar，导出 renderer/app.js 等并按 `nvvfg` 打上下文
- `work\smv\diag_dlssg_log.py` / `diag_tree.py` —— 读 sm86 日志、列 engine 目录


---

## 后续：已经解开了

`docs/smv-vfg-sm86-unlock.md` 记录了把这条门推开的过程与实测：用 `winmm.dll` 代理把 8.6 报成 8.9、
把 cubin 的 `e_flags` 89 改成 86，RTX 3080 上 1080p 4x 跑到 `27 tweens / 60.94 ms per pair`。
本文保留为"为什么原版会 -1999"的定性分析。
