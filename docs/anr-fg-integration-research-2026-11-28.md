# 在 vs-framegen 中接入 ANR FG 的可行性研究

日期：2026-11-28
状态：研究结论（未改代码）
相关：`docs/vdlssg-engines.md`（现有 NGX 链路与 XeFG/FSR 评估）、`vs-dlssg/src/ngx_session.cpp`、`vs-dlssg/src/d3d12_ctx.h`

## 1. 结论摘要

**技术上可行，且比 XeSS-FG 当初的接入代价小**：ANR FG 的 `nvngx_dlssg.dll` 是一个标准导出面的 NGX provider（D3D12 + Vulkan 双 API），只依赖 `USER32.dll` / `KERNEL32.dll`，可以由宿主直接 `LoadLibrary` 后逐函数调用，不需要 `_nvngx.dll` 核心、不需要 `version.dll` 代理、不需要 OptiScaler。

三点关键证据：

1. 导出面（dumpbin，见 §3.1）与 NGX 的 `NVSDK_NGX_D3D12_*` provider 面一致：`Init / Init_Ext / CreateFeature / EvaluateFeature / ReleaseFeature / PopulateParameters_Impl / GetFeatureRequirements / GetScratchBufferSize / Shutdown / Shutdown1`，外加 Apex 私有导出 `NRFG_PresentationControl`。
2. OptiScaler 的 `Nvngx_Custom`（`OptiScaler/framegen/nvngx/Nvngx_Custom.cpp`）就是这套用法：`CustomProviders::Load()` 取导出表 → `ngx.LoadExports()` → `QueryReportedParameters()` → 之后所有调用薄转发。**它不经过 NGX 核心的 provider 路由**，参数表用的是 OptiScaler 自己的 `NVNGX_Parameters`。
3. provider 内部字符串：`no OptiScaler presentation bridge after {} real frames (stock OptiScaler or another host): our frames run; ... need the bridge`。即**没有 OptiScaler 的 presentation bridge 时，帧生成仍然工作**，只有 "generated-only 呈现" 和面板绘制需要它。离线渲染不呈现，因此可以忽略。

真正的风险不在"能不能调起来"，而在：

- **许可**：仓库与安装包都没有 LICENSE/NOTICE，`README.txt` 写明"给装了 OptiScaler 的游戏用作 custom provider"。把 ANR 当视频插帧器、并随插件分发，**没有授权依据**。可行做法是只支持"用户自带 ANR 安装目录"，不打包、不镜像、不内嵌权重。
- **画质与性能未知**：没有任何公开的 VMAF/耗时对比；`fg-lite` 是轻量模型，且 ANR 的默认快路径是 **Vulkan**（`path_table.json` 里 NVIDIA 各架构首选 `vulkan-coopmat`，D3D12 只作为后备），我们在纯 D3D12 宿主里只能走 `d3d12-linalg / d3d12-packed / d3d12-dp4a`。
- **无公开 ABI 契约**：provider 版本会变（当前 `FG V.B20`，provider sha256 `138abe36…`），必须锁版本 + 版本探测 + 失败回退。

工作量估计：一个可验证的 spike 约 400–600 行新代码（D3D12 资源层可复用 `d3d12_ctx.h` 的 `Gpu`），产出结论后再决定是否产品化；产品化再加 `Engine::Anr` 枚举、新 profile、`anr_session.{h,cpp}`、`build.bat` 目标与文档，约 1–1.5 天。

## 2. ANR 是什么（分发形态）

- `apex-compute/ANR` = Apex Neural Rendering，产品线 ANR FG（beta）/ SR / RR。**源码不公开**，仓库只有 `nr-record` 对比工具。
- ANR FG 以安装包发布：`anr-fg-full`（游戏没装 OptiScaler）与 `anr-fg-addon`（已装）。安装脚本把它拷进 OptiScaler 的 `custom/Apex Neural Rendering FG/`，并改 `OptiScaler.ini`：`FGInput=nvngxfg`、`FGNvngxReplacement=Apex Neural Rendering FG`。
- 包内容（addon，`FG V.B20`，2026-11-19）：
  - `dlssg/nvngx_dlssg.dll`（Apex 签名，4.1 MB 级）
  - `dlssg/nr_fg.ini`、`path_table.json`、`size_table.json`
  - `dlssg/kernels/pack.nrx`、`kernels-portable/pack.nrx`、`adapters/pack.nrx`、`programs/<23 个变体>/pack.nrx`
  - 模型 `fn-pc8s1-s0`，`model_size=large`
- 23 个 program 变体：`fg-lite-{any|3840x2160}-{coopmat|coopmat-nvidia-blackwell|packed|dp4a|portable|portable-linalg|portable-xmx}-{srgb8|scrgb16|pq10}`。只有 Blackwell 有 2160p 专用程序，其余 `any` 通用。
- 三个颜色编码族正好对应三种片源：`srgb8`（SDR 8bit UNORM）、`scrgb16`（线性 FP16 scRGB，HDR）、`pq10`（HDR10 PQ）。
- `nr_fg.ini` 可调：`path=auto|coopmat|packed|dp4a|portable|portable-linalg|portable-xmx`、`model_size`、`nits_per_unit=80`、`max_generated=5`（→ 最多 6X）、`scene_cut=1`。
- `size_table.json` 给了按 GPU 架构 × 分辨率档的单帧代价估计（large：1080p/1440p `[2,6]`，Ampere 2160p `[2,6]` 为预测值）。这是目前唯一可得的性能量级线索：**单帧约 2–6 ms 量级**，需实测。
- `path_table.json`：NVIDIA 各架构首选 `vulkan-coopmat`，其次 `d3d12-linalg`；AMD 首选 Vulkan portable 系列，D3D12 为后备。**D3D12 侧只有 linalg / packed / dp4a**（字符串：`D3D12 runs the packed, dp4a and linalg paths, not ...`）。

## 3. 技术取证

### 3.1 provider 导出与依赖

`dumpbin /exports`（MSVC 14.44.35207）结果，23 个导出：

```
NRFG_PresentationControl
D3D12:  Init  Init_Ext  CreateFeature  EvaluateFeature  ReleaseFeature
        GetFeatureRequirements  GetScratchBufferSize  PopulateParameters_Impl
        Shutdown  Shutdown1
VULKAN: Init  Init_Ext  Init_Ext2  CreateFeature  CreateFeature1  EvaluateFeature
        GetFeatureRequirements  GetScratchBufferSize  PopulateParameters_Impl
        ReleaseFeature  Shutdown  Shutdown1
```

依赖表只有 `USER32.dll`、`KERNEL32.dll` —— 没有静态链 d3d12/vulkan，运行期动态解析，因此可以放进我们的 D3D12 进程里跑。

### 3.2 支持的参数名（字符串取证）

完整 `DLSSG.*` 参数集：`Backbuffer / HUDLess / Depth / MVecs / UI / UIAlpha / OutputReal / OutputInterpolated / OutputDisableInterpolation / MultiFrameCount / MultiFrameIndex / MultiFrameCountMax / MvecScale / MvecJittered / MvecDilated / MvecInvalidValue / DepthInverted / Reset / Camera* / ClipToPrevClip / PrevClipToClip / OrthoProjection / ColorBuffersHDR / JitterOffset / NotRenderingGameFrames / TargetFrameRate / ResourceAlwaysProvidedFlags / ResourceNeverProvidedFlags / MenuDetectionEnabled / UserInterfaceRecompositionEnabled`。

资源形态字符串：`float16x4 colors (UNORM: code / steps), float32 depth, float16x2 two-channel textures` —— 与 `d3d12_ctx.h` 的现有布局**完全一致**：颜色 `R16G16B16A16_FLOAT`、深度 `R32_FLOAT`、motion `R16G16_FLOAT`。

其它有用字符串：
- 三态 generator：`ANR FG, NVIDIA DLSS-G, OFF`；`NVIDIA DLSS-G unavailable: nvidia_dlssg = off in nr_fg.ini`。
- `NGX core started by this provider for NVIDIA's DLSS-G` / `NGX core {}: not started by the game (GetCapabilityParameters {}); this provider starts it when NVIDIA's DLSS-G is first made` —— NGX 核心只服务于"回退到 NVIDIA DLSS-G"那条路，ANR 自己的模型不需要。
- `requires the matching OptiScaler presentation bridge`、`generated-only requires the Streamline presentation bridge` —— 只约束"呈现策略"。
- `host binds constant and storage buffers` —— ANR 自己录命令到宿主给的 command list，符合 EvaluateFeature 契约。

### 3.3 OptiScaler 如何驱动它（可直接照抄的最小集）

`Nvngx_DllProxy`（`OptiScaler/proxies/Nvngx_DllProxy.{h,cpp}`）：

1. `Init/Init_Ext` 薄转发；
2. `QueryReportedParameters()` 用 provider 的 `D3D12_PopulateParameters_Impl` 填一个**宿主自己的** `NVNGX_Parameters`，读 `DLSSG.MultiFrameCountMax`、`FrameGeneration.Available`；
3. `CreateFeature` / `EvaluateFeature` 薄转发，`EvaluateFeature` 前会补 `DLSSG.ShowDebug`、`DLSSG.DispatchFlags`；
4. 可选：`DLSSG.Depth` 复制一份再设回去（修 AMD 深度损坏）。

`NVSDK_NGX_Parameter` 在 `ngxsdk/include/nvsdk_ngx_params.h:53-74` 是**纯虚接口**（Set/Get 各 8 个重载 + `Reset`），因此宿主可以自己实现参数表，不依赖 `_nvngx.dll`：约 120 行 `std::unordered_map<std::string, variant>` + vtable 即可，`nvsdk_ngx_params.h` 的 40 个 `NVSDK_NGX_Parameter_Set*/Get*` 自由函数也能在自己的实现上直接工作。

### 3.4 与现有架构的契合度

| 需求 | 现状 | 结论 |
| --- | --- | --- |
| D3D12 device / queue / 资源 | `d3d12_ctx.h` 的 `Gpu`，颜色 `R16G16B16A16_FLOAT`、深度 `R32_FLOAT`、motion `R16G16_FLOAT`，含上传/回读/YUV 打包/共享纹理 | 直接复用，无需改格式 |
| 会话接口 | `acquire/release/generate/newStream/width/height`（`vfg_session.h` 为最简样式） | ANR 会话照此实现 |
| motion 约定 | `ngx_session.cpp:1267` 注释：`R16G16_FLOAT` per texel，单位=帧像素，从当前帧指回上一帧，`mvecScale=1/w,1/h` | 与 DLSS-G 同约定，ANR 接受同一套 |
| 引擎选择 | `fg_types.h:17` `Engine{Dlss,Xess,Vfg,Fsr4}` + `profiles.h` 编译期 profile（`FG_ENGINE/FG_SESSION_*`/`FG_PROXY_DIR`） | 加 `Anr` 即可 |
| 多帧 | `engineMaxMulti`，`frame_gen.cpp:666` 与 per-pass `multiFrameIndex` | ANR `max_generated=5` → `6` |

也就是说，**唯一缺的是一段"直接驱动 provider"的会话代码**，其余全部现成。

## 4. 接入设计

### 4.1 路线选择

| 路线 | 做法 | 评估 |
| --- | --- | --- |
| A. 直接宿主 provider（推荐） | 自己的会话类里 `LoadLibrary` ANR 的 `nvngx_dlssg.dll`，自己实现参数表，`Init → PopulateParameters → CreateFeature → EvaluateFeature` | 无 OptiScaler、无 NGX 核心、无 GPL 牵连；依赖未公开 ABI，需 spike |
| B. 内嵌 OptiScaler 做呈现桥 | 把 OptiScaler 的 dxgi/swapchain 钩子搬进离线宿主 | 荒谬：需要真实 swapchain 与 Present，且 GPL-3.0 污染 |
| C. 走现有 `version.dll` 代理 | 让 sm86 代理转发到 ANR | 不行：该代理只做 NVIDIA DLSS-G 的 `PathListInfo` 改写 |
| D. 等 Apex 提供 headless SDK / 授权 | 商务沟通 | 最干净，时间不可控 |

### 4.2 文件级改动清单（P1）

1. `fg_types.h`：`Engine` 增 `Anr`；`engineName` 增 `"anr"`；`engineMaxMulti` 增 `6`；`colorFormatFor()`（`ngx_session.cpp:59`）对 `Anr` 返回 `R16G16B16A16_FLOAT`；`ngx_session.cpp:467` 的日志 tag 分支不要落到 `vxessfg`。
   - 更干净的做法是让 `AnrSession` 自带 `colorFormatFor`，`NgxSession` 只保留 Dlss/Xess 两支。
2. `profiles.h`：新增
   ```cpp
   #elif defined(FG_PLUGIN_VANR)
   #define FG_NS "vanr"
   #define FG_DESC "Apex Neural Rendering frame interpolation"
   #define FG_ENGINE fg::Engine::Anr
   #define FG_SESSION_HEADER "anr_session.h"
   #define FG_SESSION_NS anr
   #define FG_SESSION_CLASS AnrSession
   #define FG_PROXY_DIR L"anr"   // 用户放置 ANR 安装目录的位置
   ```
3. `anr_provider.h`（新，约 120 行）：`NVSDK_NGX_Parameter` 的自实现 + `ProviderExports` 结构（`Init/CreateFeature/EvaluateFeature/ReleaseFeature/PopulateParameters_Impl/Shutdown1` 函数指针）+ `LoadLibraryW`/`GetProcAddress` 封装 + 版本与 sha256 日志。
4. `anr_session.{h,cpp}`（新，约 500–700 行）：单例 `acquire/release`、`generate`、`newStream`、`width/height`；内部持 `vdlssg::Gpu`、provider 模块、feature handle、参数表。评估时序：
   ```
   Gpu::begin()
   Gpu::recordPreEvaluate(copyColor, copyMotion)
   设 DLSSG.* 参数（Backbuffer/HudLess/Depth/MVecs/OutputInterpolated/OutputDisableInterpolation/
                 BackbufferFrameID/MultiFrameCount/MultiFrameIndex/Reset/MvecScale/Camera*…）
   provider.EvaluateFeature(gpu->list(), handle, params, nullptr)
   Gpu::recordEvaluated(); Gpu::recordPostEvaluate(); Gpu::endAndWait()
   ```
5. `build.bat`：新增目标 `vanr`（`FG_PLUGIN_VANR`），源文件集合同 `vdlssg` 但用 `anr_session.cpp` 换掉 `ngx_session.cpp`。
6. `docs/`：本文件 + 后续实测报告；`README`/`vdlssg-plugin.md` 增 `proxy_dir` 指向 ANR 安装目录的说明。

### 4.3 载入与配置策略

- 载入路径：`<proxy_dir>\anr\dlssg\nvngx_dlssg.dll`；`proxy_dir` 也允许直接指向 `Apex Neural Rendering FG` 目录，做两级探测。
- 启动时读同目录 `nr_fg.ini`，**强行覆盖**两处以适配离线宿主：
  - `path = packed|linalg|dp4a`：禁止 `auto` 选到 Vulkan（我们的宿主没有 swapchain，ANR 的 Vulkan 路径依赖 `native bridge`：镜像 DXGI 纹理、hook 队列、等待 present，字符串里有 `our passes on fence {} did not finish in 1.5 s` 的超时逻辑）。
  - `scene_cut` 保持 `1` 或由本插件的 cut 检测接管（`frame_gen.cpp` 已有基于梯度方向的切镜检测，`sceneThreshold=0.12`）。
- 颜色编码映射：`srgb8` ← SDR 8bit 片源（需确认 VS 侧 RGBS 的传函约定：若已是线性，需要先编码到 sRGB code value，否则整体 gamma 偏差）；`scrgb16` ← HDR 线性 FP16；`pq10` ← HDR10。`nits_per_unit` 决定线性 1.0 对应多少 nit，HDR 片源要按实际峰值亮度改（默认 80 是给游戏 tonemap 后的 SDR 量级）。
- 版本锁：记录 provider sha256 与 `nr_fg.ini` 的 `build`；不匹配只告警不拒绝（Beta 迭代快），但日志里必须能看出用的是哪一版。

### 4.4 多帧与禁用语义

- `max_generated=5` 对应 `engineMaxMulti=6`；每个生成帧一次 `EvaluateFeature`，`DLSSG.MultiFrameIndex/Count` 逐次设置，与现有 `runPass()`（`ngx_session.cpp:1584-1597`）一致。
- `OutputDisableInterpolation`（`Gpu::disableTexture`）在 ANR 里是否同样被写，属待验证项；若不写，切镜时只能靠本插件自己的 copy-through 兜底（`frame_gen.cpp` 已有该逻辑）。

## 5. 主要风险与未知（spike 必须回答）

1. **D3D12 路径能否在无 swapchain / 无 Present 的进程里跑起来**：`Init` 是否需要 `FeatureCommonInfo.PathListInfo`、是否强制要求 `NRFG_PresentationControl` 被调用；`EvaluateFeature` 是否依赖 `BackbufferFrameID` 单调递增之外的呈现事件。
2. **路径回退**：`path=packed` 时是否仍尝试建 Vulkan 设备（字符串显示它会枚举 Vulkan loader 并镜像 DXGI 纹理）。若强制 Vulkan，则离线宿主收益要大打折扣，需评估"自建隐藏窗口 + swapchain"是否值得（不建议）。
3. **画质**：`fg-lite` + 零 motion + 常量深度 0.5 的输出质量。DLSS-G 在该配置下可用，但 ANR 训练分布未知；需与 DLSS-G、RIFE 三方对比 VMAF。
4. **真实 motion 的收益**：本插件已有 `nvof_motion.cpp`（NVOF 稠密光流）与可选 motion clip，可喂给 `DLSSG.MVecs`。ANR 可能比 DLSS-G 更依赖 motion —— 这反而是潜在的加分项，但要单独测。
5. **性能**：`size_table.json` 的 2–6 ms 是"按档位的估计"，不是 D3D12 实测；1080p/4K 各测一遍。
6. **ABI 稳定性**：Beta 包随时改导出/参数名。需要"缺失导出即回退到 DLSS-G 并报错"的防御。
7. **许可**：见 §1，产品化前必须明确（只能"用户自带"，且要重新评估"非游戏用途"是否越界）。

## 6. 分阶段计划

**P0 —— spike（1 个新 exe，先不碰插件）**
仿 `spike_nvof.cpp` 写 `spike_anr.cpp`（`build_spike.bat` 同款编译方式）：
- 640×360 与 1920×1080 两个合成序列（平移方块 + 渐变背景 + 一段真实片源抽帧）；
- 复用 `Gpu` 或直接内联最小 D3D12 代码；自实现参数表；
- 依次验证：`LoadLibrary` → `Init` → `PopulateParameters`（打印 `MultiFrameCountMax`/`FrameGeneration.Available`）→ `CreateFeature` → `EvaluateFeature` → 回读；
- 输出中间帧 PNG + 耗时；再分别用 `path=packed|linalg|dp4a` 跑一遍对比。
- 成功判据：能产出非空、非复制的中间帧；D3D12 路径可用；单帧耗时 < 8 ms @1080p。

**P1 —— 集成**：按 §4.2 落地 `Engine::Anr` + `vanr` 目标，跑通 `test\suite`，格式覆盖 RGBS / YUV420 8bit/10bit。

**P2 —— 画质与性能**：三段素材（实拍、动画、游戏录屏）× {无 motion, NVOF motion, 外部 motion clip} × {srgb8, scrgb16}，对 RIFE / DLSS-G 出 VMAF/SSIM 与 ms/帧表。

**P3 —— 发布决策**：只有在 P2 明显优于 DLSS-G、且许可问题有说法时才发布。

## 7. 验证命令（P1 之后）

```bat
cd /d E:\code\dlssg-vfi\vs-dlssg\src
build.bat vanr
E:\code\VFI\.venv\Scripts\python.exe -m pytest test\suite -q
```

画质对比沿用仓库现有脚本（VMAF 工具链见 `docs/` 内的引擎对比记录），产物写入 `docs/anr-fg-bench-*.md`。

## 8. 待决问题

1. 许可边界：只做"用户自带 ANR 安装目录 + 本插件不打包"是否可以接受？是否需要先向 Apex 询问"离线/非游戏宿主"的授权？
2. 是否接受"只在 NVIDIA/AMD 上走 D3D12 后备路径"（而非 ANR 首选的 Vulkan）？如果 P0 证明 D3D12 路径明显更慢或不可用，是否放弃？
3. 是否把 ANR 做成默认引擎之一（与 DLSS-G 并列），还是仅作为可选实验 DLL？
