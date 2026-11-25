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
| `preset` | 无（沿用引擎包） | DLSS 专有（`a`/`c`），其它引擎传了会报错；见 `docs\vdlssg-plugin.md` |
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
  - 并发上不去：`nvidia-smi` 采样稳态 GPU 占用 70–78%（含装载期 21–44%），把 `VFG_EIGHT_BIT=1`（拷贝字节减半）后耗时 20.36 ms 与 10-bit 的 20.17 ms 持平 —— 说明稳态是模型 GPU 计算受限，不是宿主或带宽受限，多条 lane 并发提交最多填掉那 22% 空档。真正还能省的是「一次算完整对」的批量提交，但它要把 filter 层改成缓存整对的 multi−1 帧，收益预计 <20%（后来实测为负，见下文《为什么并发和多帧流水都拿不到收益》）。
- 质量校验：`test\vfg_multi_check.py`（`VFG_MULTI=6`，Trim 90..93）逐帧算相邻差，正文 18 帧 delta 0.0010–0.0021 全部不同，无重复帧；末尾 `multi-1` 个 slot hold 最后一帧是框架侧行为，脚本已跳过。

### 用户侧表现：刚挂上滤镜时会卡一下（引擎初始化进显存）

VFG 的每条 lane 都是一个独立 CUDA context，`NvVFX_Load` 要在里面装载模型权重、建好三张设备面，然后才能出第一帧。这部分时间没有任何办法与播放重叠（引擎 API 就是同步装载），所以：

- 第一次挂上滤镜、换 `multi`（lane 数变了）、换分辨率、或者换文件重开进程，都会先卡一段：**medium 每 lane 1.5–2.5 s、high 0.5–0.7 s**（low 1.4–1.9 s），`multi=N` 是 N−1 条 lane 串起来建，1080p 6× medium 首次约 10 s 才出第一张生成帧；之后的帧对不再建 lane，进入稳态。
- 播放器里看到的就是「开始播放/切滤镜后画面停一两秒（或更久）然后才流畅」，不是掉帧、不是死锁，也不是显存不足；`VFG_TIMING=1` 会把每段建 lane 时间打到 stderr（context / pinned / create / model load / surfaces 五段，见上文档位一节）。
- 建 lane 的时间几乎全在引擎装载权重（`NvVFX_Load` 一步），与分辨率关系不大，所以缩分辨率救不了这段等待；想让首次卡顿短一些就降档（high 最快建 lane，medium/low 的权重装载慢约 4 倍）。

### 模型档位 `mode=`（2026-11-25）

DLL 的公开参数里 `Mode (uint32)` 是三档：0 low、1 medium、2 high，包内也确实带两套权重（`model_medium_*`、`model_small_*`，后者另有一套 `_fp8` 变体）。插件原先写死 high。现在：

- 滤镜参数 `mode=0|1|2`，默认 1（medium，2026-11-25 夜从 2 改过来，理由见下文《默认值改为 medium》）；`VFG_MODE` 环境变量仍可覆盖，供一次跑多档的基准脚本用。其它引擎不认这个参数，给了会直接报错而不是静默忽略。
- 实测（1080p sintel Trim 90..113，`test\engine_bench.py` 同源，每输出帧 wall，best-of-3，机器上另有 mpv/MuMu/wallpaper engine 占 GPU，±15% 噪声）：

  | multi | high | medium | low |
  | --- | --- | --- | --- |
  | 2 | 14.8 ms | 6.3 ms | 5.7 ms |
  | 4 | 18.8 ms | 5.9 ms | 4.4 ms |
  | 6 | 20.7 ms | 5.6 ms | 4.5 ms |

  同口径 DLSS-G 是 multi=6 5.7 ms、multi=2 12.4 ms，即 medium 档在 2× 时反而快近一倍。4K（3840×1632 阿哈 PV，trim 300..323，multi=2）每输出帧：high 51.9 / medium 15.5 / low 12.9 ms。
- 质量（PV 三个真实中间帧，PSNR / SSIM / 锐度比 GT）：high 33.91 / 0.9765 / 0.512，medium 33.52 / 0.9740 / 0.407，low 32.33 / 0.9692 / 0.427。medium 的代价主要是更软——VFG 本来已是几家里最软的（锐度/GT 0.652），选档时要按这个话题权衡，而不是只看 PSNR。
- 阶段分解（1080p multi=2，每个 tween）：high 打包 2.2 + `run` 3.1 + 回读 25.0 ≈ 30 ms；medium 2.1 / 0.4 / 7.1 ≈ 9.6 ms；low 2.3 / 0.5 / 4.2 ≈ 7.0 ms。注意 `NvVFX_Run` 只是入队，真正等模型的是 `download` 里的 `streamSynchronize`，所以那 25 ms 是模型本身。
- 建 lane 变慢：high 0.42–0.68 s，medium 1.6–2.3 s，low 1.4–1.9 s。`VFG_TIMING=1` 现在把建 lane 拆成 context / pinned / create / model load / surfaces 五段打印，实测中低档多出来的时间**全部在 `NvVFX_Load` 一步**（high +0.41 s，medium +1.95 s，low +1.53 s），其余四段三档一致（65–124 / 104–129 / 271–342 ms）。把 `CUDA_CACHE_PATH` 指到空目录再跑，目录里仍是 0 个文件，所以不是 PTX JIT 缓存在重建，是引擎自己在装载中低档权重时慢约 4 倍。6× 的首帧尖峰因此从约 2.5 s 涨到约 10 s。
- 宿主侧同时改了两处：`packRow`/`unpackRow` 换成 AVX2 八像素并行（`/arch:AVX2` 本来就是构建底线），`upload` 去掉多余的 `streamSynchronize`（H2D 与 `NvVFX_Run` 在同一个 stream 上，顺序天然保证，join 放在 `download`）。打包耗时 1080p 2.7→2.1 ms、4K 6.5→4.4 ms（4K 剩下的主要是 `yuvRowToRgb` 的标量转换）；端到端在 high 档看不出差别（GPU 占 80%），medium/low 档仍被噪声盖住。
- 验证：PV 三个时刻 × 三档共 9 帧与旧标量构建**逐字节相同**（max|diff| 0），`mode=` 与 `VFG_MODE` 产出同一帧。另记一条坑：同一进程里重复跑同一个帧对会走 pass-through（模型的新鲜度规则），做档位对比必须每例单独进程。
- 还能再要的：见下一节——三条结构性路线都试过，全部没有收益。
- mpv / k7sfunc 侧三条入口都已接上（2026-11-26）：`MEMC_VFG_NV.vpy` 的 `Mode = 1` 字面量、`user-data="mode=2"`、环境变量 `VVFG_MODE`，优先级 `字面量 < VVFG_MODE < user_data`；`k7f_vpypar.lua` 的 OSD 只在 VFG 脚本上多出一行「档位 低/中/高」（按脚本 basename 判断），模块算子 `VFG_NV(mode=0|1|2)` 同理。注意 `mode=0` 是合法值，转发时不能用 `if values[key]` 这种真假判断。

### 为什么并发和多帧流水都拿不到收益（2026-11-25）

围绕「把 CPU 的打包/解包藏到别的 lane 的 GPU 时间后面」试了三条路线，都实测否掉，代码已回退，只保留 `VFG_TIMING` 的分段诊断：

1. **lane 并发（去全局锁，per-lane 暂存 + 池化分带线程）**：功能与画质都对（逐帧 md5 与串行构建全同：1080p 2×/6×、YUV420P10、4K 各一例），但收益为零，因为**本插件永远收不到并发请求**。在 `generate` 里数在途调用，python 多线程与 `vspipe -r 6` 下峰值恒为 1：`frame_gen.cpp` 用 `fmUnordered` 注册（同一实例一次只调一帧）。
2. **改成 `fmParallelRequests` 强开并发**：core 会同时持有多个在途输出帧，10 GB 卡上 5 条 lane ×1.4 GB 直接进 WDDM 分页，1080p 6× medium 从 9 ms 变成 ~700 ms（130 帧 91.75 s → 274 s），用户侧表现为显存爆掉、卡顿。已改回。
3. **整对下发 / 两帧流水（一次把所有 in-between 排进各自 lane 再逐个回读，或回读完一帧再补发一帧）**：high 档 multi=6 从 20.5 ms 变成 22.7 ms（稳定复现 5 次，慢 10%），medium 档 6.09 → 6.03（噪声内）。原因是 high 档 GPU 已饱和，多 context 同时跑反而切碎 SM 与缓存局部性；medium 档的 `wait`（3.4 ms）主要是每条 lane 各自上传同一个源帧（每对 10 次 8 MB H2D 走同一条 PCIe），重叠不解带宽。

「让 5 个 in-between 共享一个 CUDA context 以省掉源帧重复上传」不成立：模型的「上一帧」状态记在 context 上而不是 effect 上（这正是每 lane 一个 context 的原因），同一 context 里第二个 effect 仍会走 pass-through。

于是现状即平台期：high 档受限于模型计算（GPU 20.5 ms/输出帧），medium/low 档受限于每帧 16 MB 的 PCIe 往返；剩下的旋钮只有 `mode=`、`multi`，以及把测量本身挪到空闲 GPU 上。理论可动的量级：medium 档若能把 8 次多余的源帧上传合并起来，还能再省 1–2 ms/输出帧（约 20%），但那需要引擎支持跨 context 的源帧共享或 D2D 拷贝，插件侧做不到。
（2026-11-25 夜更正：源帧共享已经做成了托管内存方案，「整对批量」在静机器上还没重测；这一节的
`wait` 里也还混着每个 lane 各自的上传，请看下面三节的最新账本。）

### 显存账本：每条 lane 一份模型（2026-11-25 夜）

显存不是「一张帧缓存」的量级，结构上等于**模型权重 × lane 数**：每条 lane 自己 `cuCtxCreate` 一个 context，
`NvVFX_Load` 在该 context 里把整套权重加载一遍（这也是模型「上一帧」状态能在 context 里重置的原因）。
实测（插件 verbose 打印的相邻 `free MiB` 之差，1080p，+1.4 GB 进程基线）：

| 分辨率 | high | medium | low |
| --- | --- | --- | --- |
| 480×270 | 622 | 238 | 216 MiB |
| 720p | 1310 | — | — |
| 1080p | 1406 | 714 | 354 MiB |
| 1440p | 1526 | — | — |
| 2160p | 1828 | — | — |
| 3840×1632 | 1990 | — | — |

三点结论：

1. 三档在 1080p 约为 4:2:1；480×270 时 high 仍有 622 MiB，说明其中约 0.6 GB（medium/low 约 0.2 GB）
   是**与分辨率无关的权重 + context 固定开销**，其余才是随分辨率的激活/工作区。
2. high 档随分辨率强烈次线性（622 → 1990 MiB，面积涨 20 倍、显存只涨 3.2 倍），像是模型内部
   downscale/分块后的固定网格。
3. 于是 multi=6 的 5 条 lane：1080p high ≈7 GB（10 GB 卡只剩 ~0.3 GB）、4K ≈10 GB（必爆）；
   medium ≈3.5 GB。这就是默认档从 high 改成 medium 最硬的理由，和 `fmParallelRequests` 一开就
   进 WDDM 分页（见上一节）的根因也是同一个。

官方文档（`docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html`，1.3.0）另外两条硬约束：
「Only a batch size of 1 is supported」（每次 `Run` 只写 `DstImage0`，调用方必须先取走）、
「Video Frame Generation is unsupported on RTX 2060 and RTX 3080」——所以 3080 上我们整条路都是
解锁后的非支持路径，文档也不给任何显存数字。

### 默认值改为 medium（2026-11-25 夜）

`fg_types.h` 的 `Config::quality` 默认 `1`，`test\vfg4.vpy` 用 `VFG_MODE` 默认 1 传 `mode=`，与引擎
自己的默认档一致。依据（1080p sintel，multi=6，含其它进程 ~1.4 GB 基线显存，`nvidia-smi` 200 ms 采样）：

| 档 | ms/输出帧 | SM util | VRAM used | 功耗 |
| --- | --- | --- | --- | --- |
| high | 18.3–19.1 | 83–84% | 7.7 GB | 302 W |
| medium | 5.5–5.7 | 44–49% | 4.3 GB | 131–177 W |
| low | 4.3–4.4 | 32% | 2.4 GB | 97–100 W |

medium 只用 2.2 倍速度换 0.4 dB PSNR-Y 与一半显存；而 medium/low 的 SM 占用只有 44%/32%，
说明**它们已经不是 GPU 受限**，剩下的时间在宿主侧——这正是下一节的内容。

### 现在的瓶颈：GPU 与解包完全串行（2026-11-25 夜）

阶段拆分（`VFG_TIMING=1`，medium，python 串行请求，机器较静，每 tween）：

| 场景 | up（打包+H2D） | `NvVFX_Run` | wait（模型+D2H） | unpack（解包） | 端到端 |
| --- | --- | --- | --- | --- | --- |
| 1080p multi=6 | 0.48（p50 0.00，命中缓存即跳过） | 0.36 | 2.90 | 2.13 | 5.3 ms/输出帧 |
| 1080p multi=4 | 0.78 | 0.35 | 2.96 | 2.09 | 5.3 ms |
| 1080p multi=2 | 2.22（每帧都要打包） | 0.35 | 3.10 | 2.13 | 5.5 ms |
| 4K multi=4 | 1.77 | 0.39 | 5.06 | 5.45 | 11.6 ms |
| 4K multi=2 | 5.06 | 0.35 | 5.00 | 5.34 | 12.6 ms |

- 1080p 一个 in-between 的账：GPU 约 3.0 ms（模型 ~2.3 + D2H 8.3 MB ~0.7），CPU 解包 1.9 ms，
  两者**完全串行**，所以 5.8 ms 里只有一半在用 GPU（SM 44%）。
- 4K 更极端：解包 5.4 ms 已经和 wait 持平，宿主侧占比过半；4K 的打包也涨到 5.1 ms（`yuvRowToRgb`
  的标量转换 + 786 万像素的量化）。
- 解包内核本身已接近内存带宽上限：独立微基准（1920×1080，10-bit→float×3，60 次/点）用现有
  AVX2 store 内核是 1 线程 0.90 / 4 线程 0.72 / 8 线程 0.82 / 16 线程 1.01 ms；换成
  `_mm256_stream_ps` + `_mm256_stream_load_si256` 反而更慢（1.19/1.06/1.01/1.21），所以
  「非临时存储」不是优化方向。
- `VFG_DIAG=1` 让 `download` 对同一帧缓冲跑第二遍（已热页驻留）：1080p medium unpack 1.94 /
  repeat 1.65，high 2.03 / 1.70 ⇒ 首次触碰新页只占 ~0.3 ms（每轮 `VirtualAlloc` 的新提交内存
  微基准里是 3.2 ms，帧缓冲被复用所以躲过了），其余 1.65 ms 是内核 + 内存流量本身。
- 由此得到唯一还开着的大收益：把 CPU 解包与下一次 GPU 提交重叠，理论上限
  `max(3.0, 1.9)`，即 5.3 → ~3.3 ms/输出帧（约 1.6×），4K 约 1.9×。按帧返回的 API
  （`fmUnordered`，一次一帧）拿不到跨调用的重叠，所以只能在一对内部做——下一节记了落地后的实测。
  - 另外：源帧上传的冗余已经消掉了（见下一条），所以「整对批量」现在能省的只剩解包重叠。

### 流水式批量：把解包藏到下一帧的 GPU 时间后面（2026-11-26）

上一节量出的 1.6× 重叠已经落地：`Impl::batch`（`VFG_BATCH=0` 可关，默认开，只对一对多于 1 个
in-between 时生效，`vfg_session.cpp` 的 `reserveHost`/`issue`/`tween`）。

- 形态：一对的第一次调用先把这一对的所有 in-between 各发到自己的 lane（`issue(i)` 只做
  `NvVFX_Run` + D2H 到该帧的 pinned 槽），之后每次 serve i 是 **sync i → issue i+1 → unpack i**，
  于是 unpack(i)（2.1 ms 纯 CPU）与 GPU(i+1)（~3 ms）真正重叠。早先试过的「整对全发」不行，
  因为 5 条 lane 的 run 同时入队、几乎同时完成，GPU 算完才轮到 5×2.1 ms 的串行解包。
- 1080p medium：每个 in-between 从 5.8 ms 降到 2.6 ms；端到端 multi=6 5.64 → 4.18（−26%）、
  multi=4 6.05 → 4.49（−26%）；high multi=6 17.81 → 16.64（−6.6%，GPU 本来饱和）；
  4K medium multi=4 12.45 → 10.74（−14%）。GPU 被别的进程压着时绝对值都上浮，配对 A/B 仍是
  7.16 → 5.32（−26%），所以比例稳定。
- 4K 的账对不上（账内每对 ~27.9 ms，端到端 42.8）是**宿主**的开销：`core.std.BlankClip(RGBS)`
  空帧的纯 VS 成本就是 3840×1632 3.525 ms/帧、1920×1080 1.146 ms/帧（≈0.55 ms/Mpx），与缺口
  逐毫秒吻合。真实的插件占用是 1080p medium ~2.6 ms/帧（端到端 4.18 里 1.15 是宿主地板）、
  4K ~6.9 ms/帧（端到端 10.7 里 3.5 是宿主）。
- 结论修正：medium/low 已经贴着「GPU 算一帧」的下限（每个 in-between 的端到端 4.18 ms 里
  VS 宿主地板 1.15、插件 2.6，而 GPU 部分本身就是 ~3），剩下的大头是 VS 的宿主地板，插件侧没有下一步了；
  high 的 16.6 ms 里 ~14 是模型本身，解包与宿主都已藏住。旧「已到平台期」的判断对 high 成立，对 medium/low 不成立。
- 代价与约束：pinned 槽每对最多 7 × frameBytes（1080p 5 × 16 MB、4K 5 × 75 MB）；一次请求
  一对里的全部 in-between，取单帧（如质量对比脚本 `get_frame(1)`）时会多做几次 GPU 计算。
- 验证：默认流水路径与共享打包缓存时代的基线逐帧 md5 一致（1080p multi=2/6、YUV420P10、4K
  四组，`verify_seq1.bat` + `cmp_dirs.py`），`VFG_BATCH=0` 与默认产出亦完全相同。


### 源帧跨 lane 共享：CUDA IPC 不行，托管内存可以（2026-11-25 夜）

multi=6 时 5 条 lane 原本各持有一份相同的两帧（1080p 每 lane 80 MB 显存、每对 5×8 MB H2D，其中 4 次冗余）。

- CUDA IPC 在 Windows/WDDM 上不可用：探针里 `cuIpcGetMemHandle` 成功，但 `cuIpcOpenMemHandle`
  flags=0 返回 `1 (CUDA_ERROR_INVALID_VALUE)`、flags=1 返回 `201 (CUDA_ERROR_INVALID_CONTEXT)`，
  连在导出方自己的 context 里打开也是 1；`cuCtxEnablePeerAccess` 返回 217。
- `cuMemAllocManaged`（`CU_MEM_ATTACH_GLOBAL`）分配的指针没有 context 归属，UVA 下换 context
  直接 `cuMemsetD8` 可用（普通 `cuMemAlloc` 的指针跨 context 会返回 1）。
- 于是改成：lane 0 的 context 里分配两槽托管内存，所有 lane 的 `SrcImage0/1` 都指向它；`upload`
  只在上传后立刻 `streamSynchronize`（别的 context 读它，本流的顺序没有意义），`source` 用
  `sharedIndex[2]` 记哪个槽是哪一帧。1080p 显存 80 → 16 MB，每对 5 次上传变 1 次。
- 收益：medium multi=6 每对少 32 MB H2D，约 0.6 ms/输出帧（8%）；high 档被 20 ms 模型计算掩盖，
  看不出差别；multi=2 只有一条 lane，本来就没有冗余。
- 验证：`VFG_TIMING` 里除 lane 0 外其余 lane 的 `up` 都是 0.00，且 1080p 2×/6×、YUV420P10、4K
  四组逐帧 md5 与共享前完全一致（顺带证明引擎只读 `SrcImage0/1`，不把它当草稿区）。



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
