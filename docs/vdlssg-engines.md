# 插帧引擎的接入面：DLSS-G 是怎么接的，XeFG 与 FSR 能不能换进来

本文回答两个问题：本插件当前与 DLSS-G 运行库之间的接法到底是什么，以及把它换成
XeSS-FG（XeFG）或 FSR 4.1 Lite 需要满足什么条件。结论来自三份源码的实证：
`参考插件\dlssg_for_sm86\docs\ARCHITECTURE.md`、`参考插件\MFG-Unlocker\source\native\xess\`、
`参考插件\rtx-encore\mod\docs\`，以及本机实测（RTX 3080 / 驱动 617.42 / 运行库 310.9.16）。

## 1. 当前链路（本插件 → 代理 → 运行库）

分三层，每层都只有一个连接点：

```
vdlssg.dll (VS 插件)
  │  d3d12_ctx.cpp：自建 ID3D12Device/Queue/CommandList + 三张输入纹理
  │  ngx_session.cpp：静态链 nvsdk_ngx_d.lib（动态加载器，非 _s 静态库）
  ▼
NVSDK_NGX_D3D12_Init / PopulateParameters / GetFeatureRequirements / CreateFeature / EvaluateFeature
  │  ↑ 这些调用最终落进 _nvngx.dll（DriverStore，nv_dispi.inf_amd64_feedb8c0271ca811）
  ▼
version.dll (代理) → dlssg_sm86\backend\sm86_backend.dll
  │  在 _nvngx.dll 的四个导出上挂钩，改写 PathListInfo 指向自带运行库、伪装 GPU 架构
  ▼
dlssg_sm86\runtime\nvngx_dlssg.dll  (310.9.16) + kernels\fg_kernels.dll（SM86 cubin）
```

四条不可省的约束（都是踩出来的，见 `docs\dlssg-on-3080-spike.md`）：

1. 必须链**动态** `nvsdk_ngx_d.lib` 并 `/MD`。静态 `nvsdk_ngx_s.lib` 会把 NGX 实现整个编进
   插件，进程里就不会有 `_nvngx.dll`，代理没有可挂钩的对象，特性创建永远失败。
2. `PathListInfo` 必须含 `<proxy>\dlssg_sm86\runtime`。否则运行库回落到 DriverStore 的原厂
   `nvngx_dlssg.dll`，撞架构检查 `snippet is using newer GPU arch 190 > 170` → `0xbad0000b`。
3. 代理要被显式 `LoadLibraryW(L"version.dll")` 拉起，且 `<proxy>\dlssg_sm86\logs\` 必须预先存在；
   缺日志目录时代理静默不写日志，看上去像"什么都没发生"。
4. 两趟 Evaluate 都要给 `pOutputInterpFrame`。第一趟喂上一帧做 history reset，其输出丢弃
   （`ARCHITECTURE.md:417`：特性刚创建后的第一次 Evaluate 历史是空的）；漏了就给 `0xbad0000a`。

代理内部把这件事说得很清楚（`ARCHITECTURE.md:531-534`）：它**只换 Evaluate**。CreateFeature、
能力应答、多帧上限上报、Streamline 的帧节奏与 Present 全都还是 NVIDIA 的东西；替换点是
`fg_gate` 在 NGX 核心 `EvaluateFeature` 上唯一一个跳板（特性 11 = 帧生成）。这就是为什么插件
不需要关心 Present，也不需要 Streamline。

## 2. 关键认识：NGX 本身就是那个"通用接口"

要找的"通用 FG 接口"并不存在，也不需要：**`NVSDK_NGX_D3D12_*` 那 10 个导出就是接口**，
`NVSDK_NGX_Parameter` 就是参数面。任何引擎只要把这张表实现一遍，就能被同一个调用方驱动。

MFG-Unlocker 把这一点做成了可复制的范式：

- `source\native\xess\vulkan\xemfg_provider_inprocess.cpp` 导出了完整的
  `NVSDK_NGX_D3D12_Init / Init_Ext / Shutdown / Shutdown1 / GetFeatureRequirements /
  CreateFeature / ReleaseFeature / EvaluateFeature / GetScratchBufferSize /
  PopulateParameters_Impl`，内部全部转发给 XeFG。
- 参数面完全沿用：`DLSSG.Backbuffer`、`DLSSG.Depth`、`DLSSG.Hudless`、`DLSSG.MultiFrameCount`、
  `DLSSG.MultiFrameIndex`、`DLSSG.OutputInterpolated`、`DLSSG.DepthInverted`，以及两个回调
  `DLSSG.GetCurrentSettingsCallback` / `DLSSG.EstimateVRAMCallback`。
- `NVSDK_NGX_Handle` 的真实布局是 `{ unsigned int Id; ... }`——Id 在最前，provider 可以在后面
  挂自己的指针（`struct Handle { unsigned int Id; void* feature; bool d3d12; };`）。
- 更高一层的 `MfgXeMFGProviderApi`（`mfg_xemfg_provider.h`，version 1，append-only）只是
  进程内 shim 之间传递这张表用的，不是对外 ABI。

所以"能不能接 XeFG"不是接口问题，是**输入契约与格式**问题。

## 3. XeFG 能接，但要换参数面

XeFG 的输入比 DLSS-G 硬得多（`d3d12_provider.cpp:584-593`）：

| 项目 | DLSS-G（本项目实测） | XeFG |
|---|---|---|
| backbuffer | 必须 | 必须 |
| depth | **纹理必须**（`nullptr` → `0xbad0000a`），**内容可常量**：0.05/0.5/0.95 扫描与噪声均逐位相同 | **必须**，缺则 `inputs_missing`；内容同样被真实使用（Warp 遮挡判断） |
| motion vector | **纹理必须**（`nullptr` → `0xbad0000a`），**内容几乎无所谓**：真实块匹配 vs 零矢量 max 8/255、px>8 = 0.000%，仅 10000 px 极端值才破坏 | **必须**，且像素单位 + `motionVectorScaleX/Y`，影响直接 |
| hudless | 不需要 | 可选，有则优先于后缓冲 |
| 多帧 | 2×/3×/4×（一次 Evaluate 一张） | 每个 index 一次 `Evaluate`，同一套 `MultiFrameCount/Index` |

三个硬阻碍，按严重程度排：

1. **后缓冲格式**。`ngx_provider::ColorFormat(bb.Format)` 对不接受的格式返回
   `DXGI_FORMAT_UNKNOWN`，随后 `f.fault = "backbuffer_format_<n>"` 并拒绝该特性
   （`d3d12_provider.cpp:438-439`）。XeFG 只吃 R8G8B8A8 / B8G8R8A8 / R10G10B10A2 这一族；
   本插件现在用的是 `DXGI_FORMAT_R16G16B16A16_FLOAT`（VS 的 RGBS 平面直接上传），
   正好落在被拒的格式里。改成 8-bit 或 10-bit 打包意味着每帧多一次精度转换与一次拷贝。
2. **必须提供真实光流与深度**。原插件"不需要光流也不需要深度"这一条对它不成立。视频侧没有
   深度，只能填常量；填常量等于让 XeFG 在错误的遮挡关系上做 warp，效果不会比 DLSS-G 更好。
   motion vector 可以自算（块匹配），但那就回到了 RIFE/传统光流的路子，XeFG 唯一的优势只剩速度。
3. **输入必须是资源 + subrect**，不能像现在这样"平面上传→纹理"。XeFG 走 `Resource{resource,
   baseX, baseY, width, height}`，视频侧可以把整帧当一个 subrect，成本不高。

另外两个值得注意的点：

- `XeFGSceneChangeThreshold` **默认关闭**，原因与视频侧一模一样：它把快摇当成切镜，画面先冻住
  再跳。真切镜仍靠游戏上报复位。视频侧没有"游戏上报"，所以切镜判断还是得插件自己做——这条
  经验对两个引擎通用。
- XeFG 的 CUDA 实现（`xess\cuda\xefg_cuda_layers.cu`）在 XeFG 自己的 command list 里就地替换
  Intel 的 `Dispatch`，用 `mma.sync` 跑 int8 卷积，输出与 Intel 逐位相同。它绑定的是
  XeFG 1.3.1.78 / 1.3.4.95 两个精确 PE 指纹（`xefg_internal.h` 的 `Resolve`，不匹配则
  `build_not_profiled`，且绝不往模块里写东西）。本机 `参考插件\rtx-encore\runtimes\intel-xess\3.0.2\libxess_fg.dll`
  就是 1.3.1.78，**不需要联网下载**。

结论：XeFG 接得进来（协议层面完全可行，本机运行库也在），但对视频插帧不是一个划算的交换——
要付出格式降位、多算光流与假深度，换来的只是"另一个 2×/4× 等分插帧器"。

## 4. FSR 4.1 Lite：三份参考实现里根本不存在

对 `MFG-Unlocker`、`dlssg_for_sm86`、`rtx-encore\mod` 三个仓库全量检索 `4.1`、`Lite`、`ffx`、
`fmf`、`Fluid Motion`、`FidelityFX`：**零命中**。`fsr` 只有三处，全是"检测到游戏已开
FSR 帧生成就不要叠加"的标志位：

- `source\native\optiscaler_host.h:141`——全屏模式判定沿用 FSR 的规则（注释）。
- `source\native\xess\d3d12_host.h:143`——`bool fsrFrameGeneration = false;`
- `source\native\xess\standalone_policy.h:294`——`// the game's FSR frame generation is on (never stacked)`

代理的 INI 只有两个引擎：`[FrameGeneration] Engine=DLSS|XeSS`。`foreign_fg_mod` 的 kind 列表里
虽然登记了 `dlssg_to_fsr3`，但那是"识别到别的 mod 就退让"，不是实现。

因此 FSR 4.1 Lite 无法通过"代理换引擎"接入。真要评估它，只能走 AMD 的 FidelityFX SDK
（本机没有副本，本轮也联不上网取证），而且它同样需要深度 + 光流，遇到的问题与 XeFG 同构。

## 5. 对我们的实际选择

| 方案 | 能否接入 | 视频侧代价 | 结论 |
|---|---|---|---|
| DLSS-G（现状） | 已接入 | 无（纹理必需但内容可常量，不需要真光流/深度） | 保持 |
| XeFG | 协议可行 | 格式降位 + 真光流 + 假深度 | 只在"想要 4× 以上"时再评估 |
| FSR 4.1 Lite | 否（三方实现均无） | — | 不使用 |
| 自研（RIFE/IFRNet 等） | 已对比 | 已有模型 | 画质优先时的选择 |

真要提升画质，瓶颈不在引擎选择：`docs\vdlssg-vs-models.md` 显示 DLSS-G 落后 RIFE 约 2.6 VMAF，
差距集中在快摇与高频细节，这是"等分时刻 + 单帧融合"的固有短板，换引擎不解决。
