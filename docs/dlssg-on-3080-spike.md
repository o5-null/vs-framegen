# DLSS-G 在 RTX 3080 上脱离游戏运行（spike 结果）

日期 2026-11-16。目标：验证 rtx-encore / vsdlssg 那条路能不能在没有交换链、没有 Streamline、
没有游戏的进程里，把官方 DLSS-G 当成一个「两帧进、一帧出」的纯合成器，且跑在 sm_86 上。

结论：**能。** 已在 RTX 3080（sm_86, 驱动 610.x）上跑通 CreateFeature 与 EvaluateFeature，
生成帧质量是 50/50 混合的 22 倍好（720p 刚性平移），单次合成 ~1 ms。

## 1. 可行性的三个前提（缺一不可）

1. **NGX 核心必须是真的 `_nvngx.dll`。** dlssg_for_sm86 的代理只钩 NGX 核心的公开导出
   （`GetFeatureRequirements` / `GetCapabilityParameters` / `GetParameters` / `CreateFeature`），
   加载前先挂 LoadLibrary 监视。所以必须链动态库 `nvsdk_ngx_d.lib`，不能用 vsdlssg 的静态
   `nvsdk_ngx_s.lib`（那样核被编进 EXE，代理永远装不上钩子，`FrameGeneration.Available` 恒 0）。
   核心不用自己放：`nvsdk_ngx_loader.h` 的 `NGXLoadCoreLibrary` 会依次试
   `<exe目录>\_nvngx.dll` → `<exe目录>\nvngx.dll` → `NGXGetPathUsingQAI(adapterLUID)`
   → 注册表 `Services\nvlddmkm\Parameters\NGXCore\NGXPath` → `Global\NGXCore\FullPath`。
   本机落在 `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\_nvngx.dll`。
   **SDK 和 EncoreHub 里都没有 `_nvngx.dll`**，只能来自 DriverStore。

2. **snippet 路径必须通过 `PathListInfo` 指到 sm86 版的 `nvngx_dlssg.dll`。** 只给 exe 目录时
   NGX 会退回 DriverStore 的原厂 snippet，被架构检查挡掉：
   `NGXValidateSnippetMetaData:1499 error: snippet is using newer GPU arch 190 > 170`，随后
   `CreateFeature` 返回 `0xbad0000b`。给两个路径（exe 目录 + `dlssg_sm86\runtime`）后即通过。

3. **代理必须被真正加载。** EXE 不导入 `version.dll` 时它不会进进程。实测显式
   `LoadLibraryW(L"version.dll")` 即可（app 目录优先），等价于游戏自己导入它的隐式行为。
   代理日志目录 `dlssg_sm86\logs\` 不存在时它一行都不写，必须预先创建。

## 2. 可用的调用序列（已验证）

```cpp
// 1. init：两个搜索路径 + 日志
const wchar_t* searchPaths[] = { exeDir, (exeDir + L"\\dlssg_sm86\\runtime").c_str() };
NVSDK_NGX_FeatureCommonInfo common{};
common.PathListInfo = { searchPaths, 2 };
common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
common.LoggingInfo.DisableOtherLoggingSinks = true;
NVSDK_NGX_D3D12_Init(102100511, tempPath, device, &common, NVSDK_NGX_Version_API);

// 2. 能力
NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);   // 3080 上 = 1
caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfgMax);       // = 3

// 3. feature
NVSDK_NGX_DLSSG_Create_Params create{};
create.Width = create.RenderWidth = w; create.Height = create.RenderHeight = h;
create.NativeBackbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
params->Set(NVSDK_NGX_DLSSG_Parameter_Width, w);   // 不设会报 deprecated 警告
params->Set(NVSDK_NGX_DLSSG_Parameter_Height, h);
NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &feature, params, &create);

// 4. 每个中点：reset 一次 + 两趟 Evaluate（前一帧、当前帧）
//    两趟都必须给 pOutputInterpFrame，否则 ValidateApplicationResources 报
//    "required output 'output_generated' missing or nullptr" → 0xbad0000a
```

输入资源：color = RGBA16F，depth = R32F（单通道），motion = RG16F（两 half）。
color/output 要带 `ALLOW_UNORDERED_ACCESS`；Evaluate 前 output 转 UAV，之后转 COPY_SOURCE 读回。
相机参数全给假值即可：identity 矩阵、near 0.1 / far 100 / FOV 1.04719755 / aspect w/h、
`cameraMotionIncluded = true`、`orthoProjection = true`、`motionVectorsInvalidValue = 65504`、
`mvecScale = {1/w, 1/h}`、各 subrect = {w, h}。

## 3. 实测数据（RTX 3080，屏幕上还有 95% 占用的桌面负载，取最好值）

| 分辨率 | 位移 | 生成帧 vs 真实中点 | 50/50 混合基线 | 单次两趟 Evaluate |
|---|---|---|---|---|
| 1280×720 | 6 px | **0.00154** | 0.03388 | 1.6–2.5 ms |
| 1920×1080 | 8 px | **0.01094** | — | 1.6–2.0 ms |
| 3840×2160 | 12 px | **0.00595** | 0.06663 | 31 ms（受污染，非稳定值）|

即 1080p 一个生成帧 ~0.8–1.0 ms。dlssg 官方 0.4.2 的离线重放数据（3080 Ti）是 1440p 1.03–1.65 ms、
4K 1.47–2.17 ms/生成帧，量级一致。**vsdlssg 报的 46 ms/帧不是 DLSS-G 的成本**，是它自己那条
块匹配 + 单目深度链的成本。

## 4. 两个反直觉的结果

1. **我们自己喂的运动矢量几乎不影响输出。** 同一对帧分别喂 正确矢量 / 全零 / 反号，
   生成结果逐位相同（MAE 都是 0.00154）。这一路径上运动补偿是由运行库内部完成的，
   至少对这种刚性平移的合成内容如此。→ 真素材上复测的结论见 §7：MV 被读但正常量级
   只改动第四位小数，所以插件确实可以省掉稠密光流。
2. **运行库不做场景切换检测。** 喂两张完全无关的帧，`outputDisableInterpolation` 仍返回 0，
   生成帧与 50/50 混合一样糟（0.20186 vs 0.20693）。→ 切镜检测必须插件自己做
   （vsdlssg 用 scene_threshold = 0.15），切镜处直接吐真帧。

## 5. 要变成 VS 插件还缺什么

- 真视频素材的质量验证（合成图案只证明了管线通，不证明质量）。
- 运动矢量与深度（§7 已定论）：纹理对象必须给（缺则 `0xbad0000a`），内容可以常量。
  插件因此省掉稠密光流、Depth Anything 与 D3D12 Video 块匹配。
- 场景切换检测、HDR（DLSS-G 这条路当前只吃 SDR float）、常量分辨率约束。
- 帧的进出：D3D12 侧零拷贝最好（VS 的 frame 上传到 RGBA16F 纹理），否则光上传就比合成本身贵。

## 6. 真实素材质量（Sintel 预告片 1080p24，10 组三帧）

方法：取连续三帧 i / i+1 / i+2，把 **i 与 i+2** 喂进去，让 DLSS-G 生成中间帧，和真的 i+1 比。
这样 GT 是真帧（不是插值出来的），但对插值器是 **位移加倍** 的苛刻条件。10 组基准帧
120 / 200 / 300 / 420 / 540 / 660 / 780 / 900 / 1020 / 1140，脚本 `media/extract.bat`。

四个对照：`blend` = 50/50 混合；`minterp` = ffmpeg `minterpolate` MCI+OBMC；`rife` =
rife-ncnn-vulkan rife-v4.6；`dlssg` = 本 spike。

| case | VMAF rife | VMAF dlssg | VMAF minterp | VMAF blend |
|---|---|---|---|---|
| t00 | **90.67** | 81.83 | 78.78 | 83.74 |
| t01 | **85.02** | 80.62 | 74.45 | 61.57 |
| t02 | **81.05** | 76.92 | 77.01 | 8.40 |
| t03 | 97.43 | 97.43 | 97.43 | 97.43 |
| t04 | **51.78** | 41.53 | 35.82 | 13.18 |
| t05 | **88.52** | 85.77 | 74.21 | 43.78 |
| t06 | 24.23 | **24.61** | 23.30 | 23.11 |
| t07 | **90.99** | 89.69 | 88.90 | 52.23 |
| t08 | **95.73** | 95.66 | 96.23 | 96.21 |
| t09 | 96.69 | **97.00** | 97.32 | 97.25 |
| 均值 | **80.21** | 77.11 | 74.35 | 57.69 |

MAE 均值：rife 0.00377 / dlssg 0.00411 / minterp 0.00502 / blend 0.00707。

- DLSS-G 的 MAE 只比混合好 **1.70×**，但 VMAF 领先混合 **19.4 分** —— MAE 奖励糊（颗粒被平均掉），
  所以真实素材上必须看 VMAF，别看 MAE。
- DLSS-G 赢 minterp 6/10，RIFE 赢 DLSS-G 7/10。**三者的排序是稳定的：RIFE > DLSS-G > 古典 > 混合。**
- 失败模式集中：快速摇镜（t04，41.5 分；t06，24.6 分）与细结构（t00 鼻环重影）。
  目视对照在 `media/review/`（`t02_face.png`、`t04_compare.png`，左=生成、中=真帧、右=混合）。

## 7. 运动矢量与深度：对象必需，内容不必

先分清两件事：**纹理对象**是运行库的硬校验项，**纹理内容**不是。

不带纹理（指针传 `nullptr`）直接失败，运行库按名字报错：

```
[EndpointCore::ValidateApplicationResources:968] Error: required input 'depth_curr' missing or nullptr
[EndpointCore::ValidateApplicationResources:968] Error: required input 'mvec_curr2prev' missing or nullptr
   -> FAILED: EvaluateFeature (previous frame) failed (NGX 0xbad0000a)
```

`--nodepth`、`--nomv`、两者同时（三个目录都 RC=1）报的是同一族错误，说明这条校验不是可选的。
SDK 契约也这么写（`ngxsdk/include/nvsdk_ngx_defs_dlssg.h:106`）："Resources required by the
algorithm (backbuffer, mvecs, depth, and output)"。所以插件里那两个常量纹理**必须存在**。

内容则可以随意填。十组 triple 素材、全部同一构建、每项都跑两遍确认逐位可复现：

| 探针 | 做法 | vs 基线（`out_base`） | gen mean |
|---|---|---|---|
| `--depthval 0.05` / `0.95` | 深度常量扫描（与默认 0.5 相距 0.45） | **10/10 逐位相同**，max=0 | 0.00416 |
| `--depthrand` | 深度填 LCG 噪声（均值恰为 0.5） | **10/10 逐位相同**，max=0 | 0.00416 |
| `--mvzero` / `--mvflip` | 矢量填零 / 反向 | **10/10 逐位相同** | 0.00416 |
| `--blockmv` | 16×16 块匹配（±32 px，coarse step 4）逐像素矢量 | 10/10 有差异，max 1–8/255，px>8 = 0.000% | 0.00416（t02 0.00524→0.00525） |
| `--mvhuge` | 矢量常量 10000 px | 10/10 有差异，t02 max 123、px>8 = 3.19% | **0.00451** |
| `--blockmv --depthrand` | 真实矢量 + 噪声深度 | 对比 `out_blockmv` 有差异，max 1–21/255，t02 px>8 = 0.007% | 0.00416 |

三条读数需要放在一起才成立：

1. **深度内容在无真实运动时是纯噪声**。0.05 / 0.5 / 0.95 三点扫描、噪声、以及什么都不改，
   输出逐位相同——这不是"深度被归一化后抵消"，因为常量扫描没有任何可以抵消的空间平均过程。
   深度只可能进入"按深度判断遮挡"的路径，而该路径要先有可信的运动才有意义。
2. **运动矢量被读，但量级要足够大才可见**。`--blockmv` 十组都有逐像素差异，可最大差只有
   8/255、超过 1% 的像素不超过 0.058%，mean 一位小数都不动。只有 10000 px 这种物理上不可能
   的矢量才把输出拉坏（1.70× → 1.57×）。正常视频里 ±几十 px 的矢量，贡献落在第四位小数。
3. **真实运动一旦进来，深度才开始有可见影响**（`--blockmv` vs `--blockmv --depthrand` 最大
   21/255，t02 有 0.007% 像素超过 8/255）。这符合遮挡判断的机制：运动可信了，深度才有东西
   可判。

对插件的意义：**纹理对象必须给，内容可以常量**。不需要自己算稠密光流、不需要 Depth
Anything、不需要 D3D12 Video 块匹配；运动估计与遮挡判断都在 `nvngx_dlssg` 内部完成
（`Kernel_EstimatePrev2CurrScatter` / `EstimateIntermMvecsScatter` / `WarpBlendingWeights`）。
整条链是 **RGB 进 → RGB 出 + 自己判切镜**。这也解释了为什么 `--blockmv` 那种 16×16 块匹配
得不到收益：它不是"更好的矢量能提升画质"，而是内部估计器对低质量矢量本身不敏感。


## 8. 吞吐

4K（3840×2160）25 轮、取最好值：**两趟 Evaluate 2.664 ms → 一个生成帧 ~1.3 ms**。
1080p 同样 ~0.8–1.0 ms。与 dlssg 官方 0.4.2 的离线重放数据一致（3080 Ti 4K 1.47–2.17 ms）。

同机对照 rife-ncnn-vulkan（Vulkan/ncnn，比 vs-mlrt 的 TensorRT 慢）在 4K 上墙钟
2.2–3.3 s/插值帧，但其中 PNG 解码编码与进程启动占大头，且当时 GPU 有 95% 桌面负载，
**这个数只能当数量级参考，不能当 RIFE 的真实成本**。可靠的口径是 vs-mlrt 的 TRT 实测。

真正的瓶颈是**数据搬运不是合成**：本 spike 每轮把 4K 三张输入（约 200 MB）从 CPU 拷上去，
upload 单次 40–180 ms，比 Evaluate 贵 20 倍以上。插件必须做零拷贝（VS frame → 共享纹理
或 GPU 侧直接生成），否则跑不过拷贝。

