# 三种额外补帧引擎：XeSS-FG、NVIDIA VFG、AMD FSR4

`vs-dlssg` 现在是四个独立插件，一个 DLL 一个引擎，互不影响：装哪个用哪个，`multi` 上限与引擎包目录各自不同。

| 插件 | 引擎 | 产物 | 引擎包目录 | `multi` | 输入纹理 |
|---|---|---|---|---|---|
| `vdlssg.FrameGen` | NVIDIA DLSS-G（sm86 解锁） | `bin\vdlssg.dll` | `dlss45\` | 2–6 | RGBA16F |
| `vxessfg.FrameGen` | Intel XeSS-FG model 15/18 | `bin\vxessfg.dll` | `xessfg\` | 2–4 | R10G10B10A2 |
| `vvfg.FrameGen` | NVIDIA Video Frame Generation | `bin\vvfg.dll` | `vfg\` | 2–6 | R10G10B10A2 |
| `vfsr4.FrameGen` | AMD FSR 4 帧生成（SmoothMyVideo 桥 + FSR runtime） | `bin\vfsr4.dll` | `fsr4\` | 2（固定） | RGBA16F（可退 RGBA8） |

四个插件共用同一份 `frame_gen.cpp`（帧映射、切镜检测、属性输出）与同一套参数，只有 `profiles.h` 的六个宏与 `*_session.cpp` 不同。想加第五个引擎，只需再写一个 session 类并加一个 profile。

同一段源（1080p sintel `Trim 90..113` = 24 源帧，RTX 3080，`test\engine_bench.py`，已排除引擎装载的预热帧）：

| 插件 | multi | 输出帧 | 耗时 | 每输出帧 |
|---|---|---|---|---|
| `vdlssg` | 6 | 144 | 0.97 s | 6.76 ms |
| `vxessfg` | 4 | 96 | 0.75 s | 7.78 ms |
| `vvfg` | 4 | 96 | 1.94 s | 20.17 ms |
| `vvfg` | 6 | 144 | 3.21 s | 22.32 ms |
| `vfsr4` | 2 | 48 | 0.44 s | 9.08 ms |

VFG 比 DLSS-G 慢约 3.5 倍（模型本身重，且每个中间帧要一条独立 CUDA context），FSR4 受桥的语义限制只能 2×。

参数（四个插件完全一致）：

| 参数 | 缺省 | 说明 |
|---|---|---|
| `multi` | 2 | 每对相邻帧插 `multi-1` 张，上限见上表 |
| `mv` | 无 | 可选光流剪辑（RGB(S)，2 或 3 平面，全尺寸或半尺寸），像素单位、第 k 帧指向 k−1 |
| `scene_threshold` | 0.15 | 相邻帧三平面 8 采样平均绝对差超阈值判切镜、整对 hold |
| `proxy_dir` | `<DLL 目录>\<引擎包目录>` | 引擎包目录 |
| `gpu` | 0 | 适配器序号 |
| `camera` | 0 | DLSS 专有（相机类型），其它引擎忽略 |
| `log` | 0 | 每帧向 stderr 打一行毫秒数 |

输出属性：`_DLSSGGenerated`、`_DLSSGFallback`、`_DLSSGSceneCut`、`_DLSSGMultiFrameIndex`（名字沿用，四个插件一致）。

---

## XeSS-FG

不需要 Intel 的 `libxess_fg.dll`：`dlssg_for_sm86` 0.4.2 的代理里已经把 XeSS-FG 的两个模型（model 15 = 2X、model 18 = 4X 图）编译成自己的 CUDA kernel，放在 `dlssg_sm86\xefg\xefg_kernels.dll`。所以只要把 `dlss45\` 整目录复制成 `xessfg\`，再把 `dlssg_sm86.ini` 改成：

```ini
[FrameGeneration]
Engine=XeSS
MaxGeneratedFrames=3

[XeSSFG]
FlowResolution=Middle
UseHudless=Auto
```

- `MaxGeneratedFrames` 最大 3（4× 输出）；超过 3 代理直接拒绝（`max_generated_frames_unsupported`）。
- XeSS 只收 8/10-bit backbuffer，插件因此把颜色纹理切成 `R10G10B10A2_UNORM`（`d3d12_ctx` 的 `colorFormat` 参数）：RGBA16F 会被判 `backbuffer_format_<n>` 并 `xefg_feature_refused`。
- 深度与运动矢量是硬要求（XeSS 用 Warp 做遮挡）。插件沿用 DLSS 的做法：深度填常量 0.5、没给 `mv` 时填零场。XeSS 比 DLSS-G 慢，但在 1080p 上仍是 1–3 ms/张量级。
- 日志证据（`xessfg\dlssg_sm86\logs\dlssg_sm86.log`）：`"engine":"XeSS-FG model 15 ..."`、`"colour_format":"R10G10B10A2_UNORM"`、`"event":"xefg_reset" ... "graph":"2x"`、`"cuda_image":"cubin_sm86"`。
- 实测：1080p 四倍帧 `Output 96 frames in 1.09 seconds (87.87 fps)`（24 源帧，`test\xess4.vpy`）。

## NVIDIA VFG

`vvfg` 直接调 NVIDIA Video Effects SDK 的 `nvVFXVideoFrameGeneration.dll`（`vfg\` 四个文件：`NVCVImage.dll`、`NVVideoEffects.dll`、`nvVFXVideoFrameGeneration.dll`、`cudart64_12.dll`）。它只带 sm_89 / sm_120 两种 cubin，RTX 30（sm_86）要解锁，否则模型加载报 `the model's load failed: unknown error (status -1999)`。

解锁在插件进程内完成（不需要 `winmm.dll` 代理，也不需要改参考插件）：`vfg_session.cpp` 在 `LoadLibrary` 之后、`NvVFX_Load` 之前改 `nvVFXVideoFrameGeneration.dll` 自己的导入表，把 `nvcuda.dll` 的两个槽换成 hook：

1. `cuDeviceGetAttribute`：`attrib == 76`（COMPUTE_CAPABILITY_MINOR）且值为 6 时改报 9 —— 8.6 冒充 8.9。
2. `cuModuleLoadData`：先扫一遍 fatbin，把 ELF 头 `e_flags` 低字节 89 改成 `0x00560556`、fatbin 条目里 sm 字段 89 改成 86，再交给原函数。

两条 hook 只影响这个 DLL 自己的 IAT，不影响进程里其它 CUDA 使用者。

**`multi` 2–6：每个中间帧一条 lane（一个 CUDA context）。** 模型的「上一帧」状态存在 CUDA context 里，同一个帧对重复 `NvVFX_Run` 时只有第一次真插值，之后一律原样输出当前帧 —— 换 `DstImage0` 缓冲、换 effect 实例、换 CUDA stream、`FrameMultiplier`/`FrameIndex` 模式、反向 prime 都无效，但**换一个新的 CUDA context 就重置**。所以插件给每个中间帧索引建一条独立 lane（自己的 context + stream + effect + 三张 device 面 + 两槽帧缓存），每对帧每条 lane 只跑一次：`t = index/(multi)`，lane 的上一帧天然就是上一对的 `cur`，不需要任何显式重置。probe 验证（`test\probe\vfgprobe.cpp`，320x180，`count=4`）：ratio = 0.299 / 0.448 / 0.587 / 0.680，四个中间帧互不相同且单调。

- 代价一：显存。1080p 下每条 lane 约 1.4 GB（模型 + 三张 4 字节/像素的面），6× 的 5 条 lane 共约 5.6 GB；10 GB 卡上 1080p 6× 还剩约 3.2 GiB，4K 不可行。
- 代价二：建 lane。每条 lane 要 `NvVFX_Load` 一次模型，1080p 约 0.5–0.7 s（320x180 约 0.55 s，与面积关系不大，主要是权重上传），所以 multi=N 的首 N−1 个中间帧会出现秒级尖峰，之后稳定。
- 像素格式固定 RGBA / RGB10A2（BGRA 会红蓝互换），插件走 `R10G10B10A2` + `P32`；`pitch = width*4`。`VFG_EIGHT_BIT=1` 环境变量可切到 RGBA8/U8（供排查，语义与 10-bit 完全同构）。
- 实测（`test\vfg4.vpy`，1080p，Trim 90..113 = 24 源帧；vspipe 口径，时间含引擎装载与 lane 建造，所以比开头那张表低）：
  - `multi=4`：`Output 96 frames in 3.78 seconds (25.41 fps)`，lane 0/1/2 建造 691/508/491 ms，显存余量 8885 → 7479 → 6073 MiB。
  - `multi=6`：`Output 144 frames in 6.81 seconds (21.14 fps)`，5 条 lane 596/579/552/539/571 ms，余量 8885 → 7479 → 6073 → 4667 → 3261 MiB。
  - 稳态每个中间帧约 25–40 ms（RTX 3080，10-bit；比 DLSS-G 的 1.4 ms 重一个数量级，这是 VFG 模型本身的成本）。
  - 源帧打包在 lane 之间共享（`vfg_session.cpp` 的 `packed` 两槽缓存 + `packedIndex[2]`）：同一帧只跑一次 RGBS→RGB10A2 循环，省掉 4/5 的打包 CPU 时间，4× 由 4.47 s 降到 3.78 s、6× 由 7.81 s 降到 6.81 s；H2D 次数不变（每个 context 一份自己的显存面，跨 context 不能 D2D）。
  - 打包与回读解包都按行分带并行（`parallelRows`，最多 16 线程，同 `ngx_session.cpp` 的做法）：4× 再降到 1.94 s / 20.17 ms 每输出帧，6× 6.08 s（含 2.9 s 建 lane，稳态约 3.2 s）。`vfg_multi_check.py` 结果不变（`multi=4` 正文 12 帧 delta 0.0020–0.0025，`multi=6` 18 帧 0.0010–0.0021，末尾 hold 帧数正确）。
  - 并发上不去：`nvidia-smi` 采样稳态 GPU 占用 70–78%（含装载期 21–44%），把 `VFG_EIGHT_BIT=1`（拷贝字节减半）后耗时 20.36 ms 与 10-bit 的 20.17 ms 持平 —— 说明稳态是模型 GPU 计算受限，不是宿主或带宽受限，多条 lane 并发提交最多填掉那 22% 空档。真正还能省的是「一次算完整对」的批量提交，但它要把 filter 层改成缓存整对的 multi−1 帧，收益预计 <20%。
- 质量校验：`test\vfg_multi_check.py`（`VFG_MULTI=6`，Trim 90..93）逐帧算相邻差，正文 18 帧 delta 0.0010–0.0021 全部不同，无重复帧；末尾 `multi-1` 个 slot hold 最后一帧是框架侧行为，脚本已跳过。


## AMD FSR 4

`vfsr4` 不碰 AMD 的 D3D12 API，也不走 NGX：它加载 `fsr4\smv_fsrfg_bridge.dll`（SmoothMyVideo 的桥），桥自己用 `smv_vkd3d_d3d12.dll`（打过补丁的 vkd3d-proton）建一个 D3D12 设备，创出 `in`（颜色）、`mv`（R16G16F 运动矢量）、`out`（结果）三张共享纹理和一条共享 fence，把 NT 句柄交给宿主导入。`amd_fidelityfx_framegeneration_dx12.dll` + `amd_fidelityfx_loader_dx12.dll` 是 AMD 的 FSR 运行时，`amdxc64.dll` 是桥的 stand-in。

**关键：三张纹理只能用 Vulkan 导入。** SMV 的 vkd3d 补丁（`fsr4\source\vkd3d-proton-bfdb811-smv.patch`）在 `CreateSharedHandle` 里删掉了 `D3DKMTShareObjects`，直接返回 Vulkan 的 OPAQUE_WIN32 导出句柄，并且不再登记到 vkd3d 自己的 shared-handle 表。实测：`ID3D12Device::OpenSharedHandle` 在该资源上返回 `E_INVALIDARG`（vkd3d）或让进程直接退出（系统 d3d12）；CUDA 12 的 `cuImportExternalMemory`（OPAQUE_WIN32 / KMT / dedicated / size 0 全试过）一律 `CUDA_ERROR_INVALID_VALUE`；只有系统 D3D12 能打开 fence。所以 `fsr4_vk.cpp` 用 Vulkan 按 LUID 选同一块显卡，`vkGetMemoryWin32HandlePropertiesKHR` 判断句柄类型（实测颜色/运动/输出都是 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT` = 0x40，线性 tiling 可用，导出特性 `EXPORTABLE|IMPORTABLE`；fence 是 `D3D12_FENCE_BIT` = 0x8 的 timeline 信号量），dedicated import 后只在 TRANSFER_SRC/DST 里用，所有拷贝都在 `GENERAL` 布局完成。

**fence 协议**（每次 `generate` 一轮，严格 +1）：

1. 宿主把 `prev`（RGBS→RGBA16F，alpha 填 1.0）写进 `in`，把运动矢量写进 `mv`，用 `vkQueueSubmit` + `VkTimelineSemaphoreSubmitInfo` 把共享 timeline 抬到 `v`；
2. `fsrfg_frame_i(idx, waitValue = v, signalValue = v + 1, reset, 16.6667)`：桥在自己的队列上 `Wait(fence, v)`，跑 prepare V2 + `numGeneratedFrames = 1` 的生成 dispatch，再 `Signal(fence, v + 1)`；调用本身不阻塞；
3. 宿主 `vkWaitSemaphores(v + 1)`，然后立即 `vkCmdCopyImageToBuffer` 把 `out` 回读到 CPU。

桥的语义是「每次喂下一张真实帧，`out` 是上一张与这一张的中点」，所以一对帧要喂两次：`prev` 用 `reset=1`（序列第一帧，`out` 无 tween），随后 `cur` 用 `reset=0`，第二次之后 `out` 才是中点。`fsr4_session.cpp` 因此按 stream+`pairIndex` 记状态：相邻对（`pairIndex == lastPair + 1`）只喂 `cur`，同一对重复调用直接复用上一次的 `out`。

- **`multi` 只能是 2**：桥里 `numGeneratedFrames` 硬编码 1，只有一张 `out` 纹理；`engineMaxMulti(Fsr4)` 返回 2，`multi=3` 会直接报 `vfsr4: the fsr4 engine interpolates one frame per pair, so multi must be 2`。
- 颜色优先 RGBA16F（fmt 1），`fsrfg_create_i` 失败时退回 RGBA8（fmt 0，纯线性 0..1↔0..255），日志会写明。深度不需要：桥内部自己填常量 0.5。运动矢量是像素单位、current→previous；没给 `mv` 时填零场，半尺寸 `mv` 按最近邻放大。
- `camera` 只做平移近似：把该帧运动矢量的均值（clamp 到 ±width/4、±height/4）传给 `fsrfg_set_camera_i`，让 FSR 有一个大致正确的相机步进；旋转/缩放相机无法表达。
- **回读必须用 HOST_CACHED 的主机内存。** 第一版选了第一个 `HOST_VISIBLE|HOST_COHERENT` 内存类型（写合并），GPU 写进去很快但 CPU 读出来只有约 180 MB/s：1080p 一帧要 90 ms。改成优先 `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` 后回读降到 3–4 ms，这是整条链路唯一真正花的钱。
- 实测（RTX 3080，`test\fsr4.vpy` 与 `test\fsr4_720.vpy`，`Trim 90..95` = 6 源帧 → 12 帧；vspipe 口径，含装载）：
  - 1080p：`Output 12 frames in 0.15 seconds (78.85 fps)`，首帧 24.4 ms（建上下文），之后 11.5–12.0 ms/帧。
  - 1280x720：`Output 12 frames in 0.11 seconds (107.63 fps)`，首帧 16.3 ms，之后 6.0–6.6 ms/帧。
  - 每帧分解（`VFSR4_DEBUG=1`，1080p）：打包 2–3 ms、上传 1.3 ms、`fsrfg_frame_i` 调用 0.6 ms、等桥 3–4 ms（FSR4 本体 GPU 时间）、回读 3–4 ms。
  - 日志（`log=1`）：`[vfsr4] FSR 4.0.2 on vkd3d-proton over Vulkan, 1920x1080 RGBA16F`、`[vfsr4] frame 5 from 2/3 slot 1/1: 12.07 ms`。
- 已复核：整帧都写满 —— 纯色 1920x1080 的 2 帧测试里 `out` 第 1079 行均值 0.3000，与第 0 行一致。此前「最底 3 行 FSR 不写」的结论是 sintel 自带黑边造成的误判。真正能看到的只是黑边区的极淡溢出：`Trim 90..92` 那对的中间帧在 963–1016 行有 mean≈0.004 的内容（正常黑边是 0），属 FSR 在低对比边缘的正常行为。探针里用竖条纹验证过 tiling 与 fence 语义正确（中点相位准，连续行无条带）。

## 每帧开销分解（1080p，RTX 3080）

`test\engine_bench.py` 的 `LOG=1` 把插件自报的毫秒数（stderr）和墙钟一起抓出来，宿主时间 = 墙钟 − 自报：

| 插件 | 墙钟/输出帧 | 引擎自报 | 宿主 | 说明 |
|---|---|---|---|---|
| `vdlssg` 6× | 5.99 ms | 0.93 ms（p50 0.61） | 5.06 ms | NGX 评估极快，宿主是瓶颈 |
| `vxessfg` 4× | 6.00 ms | 1.25 ms（p50 1.03） | 4.75 ms | 同上 |
| `vvfg` 4× | 20.17 ms | 每 tween 约 27 ms（p50 26.7，含建 lane 尖峰） | 约 5 ms | 模型 GPU 受限，见 VFG 一节 |
| `vfsr4` 2× | 8.90 ms | 自报含两次喂帧，不可直接比 | 约 5 ms | 打包/回读/等桥各占一部分 |

- DLSS/XeSS 的 5 ms 宿主时间基本就是内存带宽下限：每输出帧要读回 1920×1080×8 B（RGBA16F，25 MB）再写成 3 个 float 平面（25 MB），合计 50 MB；本机内存带宽下 50 MB 约 5 ms，实测 5.06 ms 已经贴着这个地板。
- 打包/回读循环在 `ngx_session.cpp` 由 `parallelRows` 按行分带并行（最多 16 线程，单线程这类搬运只有约 1 GB/s）；`vfg_session.cpp` 的打包与解包现在同样分带并行。再往上加线程没有收益，因为瓶颈已经从单核速度移到内存总线。
- 引擎内部的 GPU 调用是串行的、有状态的（NGX feature、VFG 的 context 状态、FSR4 的 fence 序），不能把一个引擎的求值拆到多线程；唯一的并发维度是 VFG 的多条 lane（各自独立的 context/stream），而它的稳态 GPU 占用已到 70–78%，并发只能填掉空档。
- 想在 DLSS/XeSS 上再降宿主开销，只能减少字节数：例如把回读格式从 RGBA16F 换成 `R10G10B10A2`（10 MB 而不是 25 MB），代价是输出精度掉到 10-bit；1080p 收益约 1.7 ms/帧，4K 才是它的舞台（回读从 66 MB 变 33 MB）。
- `vfsr4` 的 8.9 ms 里 GPU 只占约 3.5 ms，其余是打包（2–3 ms）、上传（1.3 ms）与回读（3–4 ms）；回读已经是 `HOST_CACHED` 内存下的带宽下限。

## 已知限制

1. 四个插件各自单进程单尺寸：换分辨率要重启进程。
2. 都只接受 `RGBS`（`cfRGB` + `stFloat` + 32 bit + 3 平面）。
3. 插件把输出帧率乘上 `multi` 并逐帧写 `_Duration`/`_AbsoluteTime`，不要再自己 `AssumeFPS`。
4. `camera` 只对 DLSS 有效。
5. `vdlssg` 与 `vxessfg` 不能同进程共存：NGX 的核（`nvngx.dll`）和 dlssg snippet 都按名字加载、每进程只能 init 一次，先 init 的代理凭自己的 `dlssg_sm86.ini` 固定 `Engine=DLSS` 还是 `XeSS`，后者的 `EvaluateDlssg`/`CreateDlssgFeature` 会返回 `0xbad00004`/`0xbad00001`。插件现在用进程级命名互斥量提前拦截，报 `another NGX engine plugin (vdlssg) already initialized NGX in this process; ... load only one of vdlssg / vxessfg per script`。`vvfg`、`vfsr4` 不受影响（前者只用 CUDA driver API，后者只用 vkd3d + Vulkan）。对比四个引擎跑 `test\all_engines.py`，它按引擎各起一个子进程。
