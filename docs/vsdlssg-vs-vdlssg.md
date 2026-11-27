# 参考插件 vsdlssg 与本仓 vs-dlssg 的差异

> 研究日期：2026-11-21　对象：`参考插件\vsdlssg`（只读参考，PolyForm Noncommercial）与
> `vs-dlssg\src`（本仓实现）。两边都是「RGBS 帧当 CPU 内存、D3D12 上传 → NGX 求值 →
> 回读 → 写回 VS frame」的进程内 DLSS-G 封装，**没有任何 GPU 共享纹理**（参考插件全仓
> `SharedHandle/KeyedMutex/WrapResource` 零命中；本仓只有 FSR4 那条路走共享句柄）。

## 0. 一句话结论

参考插件把 FG 当成「**视频滤镜链的最后一环**」：它自己造 guide（D3D12 Video 块匹配
运动估计 + Depth Anything V2 深度），只做 2×，只支持 DLSS-G，用户自备原厂运行库。
本仓把 FG 当成「**纯合成器**」：guide 内容按实测认定无关（深度恒 0.5、MV 可选且生产
脚本从不传），把工程量投在**多帧生成 ×4 引擎 × sm_86 解锁的运行时打包**上。

## 1. 对外契约

| | `vsdlssg`（参考） | `vs-dlssg`（本仓） |
|---|---|---|
| 插件 id / 函数 | `com.vapourkit.dlssg` / `Interpolate`（`fg\plugin.cpp:188-190`） | `com.vapoursynth.<vdlssg\|vxessfg\|vvfg\|vfsr4>` / `FrameGen`（`frame_gen.cpp:465-471`） |
| 参数 | `clip, depth, motion:opt, runtime_dir, depth_inverted:opt, auto_motion:opt, scene_threshold:opt` | `clip, multi:opt, mv:opt, proxy_dir:opt, gpu:opt, scene_threshold:opt, duplicate_threshold:opt, camera:opt, log:opt` |
| 深度 | **原生 API 必需**（`GRAYS`，`fg\plugin.cpp:139-145`），只能由 `dlssg.py` 自动生成 | **没有入口**，内部常量 0.5f 填满整张 R32F 纹理（`ngx_session.cpp:662-684`） |
| 运动 | 可选 clip；缺省走内置 D3D12 Video 估计器（`auto_motion=1`） | 可选 `mv` clip；缺省零矢量常量纹理，**不做自动估计** |
| 分辨率/帧率 | 三个 clip 必须同尺寸/同帧数/同 CFR（`fg\plugin.cpp:143-145`） | 只有 `mv` 需匹配（`frame_gen.cpp:386-396`） |
| 有效分辨率 | D3D12 侧支持 `renderWidth/Height`，但 FG 固定 1:1 | 固定 1:1（`profiles.h` 无 DRS，`DynamicResolutionScaling=0`） |
| 输出帧率/时间戳 | 自己把 `fpsNum/Den` 约分后翻倍，并逐帧写 `_DurationNum/_DurationDen`、给插入帧推 `_AbsoluteTime`（`fg\plugin.cpp:162-169,107-119`） | 已按此改写：`multi` 倍后 gcd 约分写回 `vi.fpsNum/fpsDen`，逐帧写 `_Duration*`、插入帧推 `_AbsoluteTime`（`frame_gen.cpp` `applyTimelineProps`）；不再需要调用方 `AssumeFPS` |
| 输出 props | `_DLSSGGenerated/_DLSSGFallback/_DLSSGSceneCut` + `_SceneChangePrev/Next` 重映射到双倍时间轴 | `_DLSSGGenerated/_DLSSGFallback/_DLSSGSceneCut/_DLSSGMultiFrameIndex`，并已把源 `_SceneChangePrev/Next` 重映射到输出时间轴（组首/组尾） |
| 像素校验 | 逐像素校验有限且 0..1，否则抛错（`scene_detection.h:12-22`；HDR 直接不支持） | 完全不校验，越界值原样进运行库（`frame_gen.cpp:357-362` 只查格式） |

## 2. 引擎与倍数

- 参考：**一次 Evaluate 只出一个中间帧，固定 2×**。每对输入固定 `reset` pass + 产出 pass
  两次 Evaluate（`fg\fg_context.cpp:229-239`），README 明说 multi-frame 是 future work
  （`fg\README.md:142`）。
- 本仓：`multi` 2..6，靠 DLSS-G 的 `multiFrameCount/multiFrameIndex` 逐槽各调一次
  Evaluate（`ngx_session.cpp:866-900`），引擎上限 `Dlss=6 / Xess=4 / Vfg=6 / Fsr4=2`
  （`fg_types.h:38-50`）。每对帧仍然先跑一次 `reset=true` 的 pass，但**跨对跨槽缓存**
  （`lastPairIndex/seededPairIndex`，`ngx_session.cpp:700-707`）：上一对的右帧就是这一对的
  左帧时省掉上传，同一对再取别的槽时省掉 reset pass。
- 参考每对都 reset、不缓存，理由是「乱序请求也不能依赖历史」（`fg\README.md:96-99`）。
  本仓靠 `streamId` 代次 + 缓存键等价保证同一语义（`ngx_session.cpp:966-979`）。

## 3. Guide 从哪来（核心分歧）

### 参考插件：完整的 guide 生产链

- 运动：`vsmotion` / 内建路径都用 **D3D12 Video 固定功能 ME**，块 16×16 + 1/4 像素精度
  （`src\d3d12_ctx.cpp:356-359`），块向量 int16 在 shader 里 `*0.25` 转像素
  （`src\d3d12_ctx.cpp:590`），再跑一遍 `cs_5_0` 全屏精化（3×3 邻块候选 + 亮度匹配 +
  正则 + 零向量兜底）。FG 内建路径全分辨率、结果只在 GPU（`m_motionTexture`），
  不回读 CPU；独立 `vsmotion` filter 则每帧阻塞回读一次。
- 深度：`vk_depth.py` 的 **Depth Anything V2 Small**，TensorRT 优先、ORT 兜底
  （`depth\vk_depth.py:60-92`），短边 252/518，固定反深度映射 `raw/(raw+20)`
  （`:122-123`）以避免逐帧归一化导致的 pumping。开销：720p guide 生成 20.6 ms（fast）/
  42.7 ms（high），单帧 FG ≈46 ms（RTX 5080 Laptop，`fg\README.md:83-86`）。

### 本仓：guide 内容当作常量

- 深度整张 0.5f；MV 缺省全零。注释给出了当时的实测依据（`ngx_session.cpp:662-667`）：
  深度 0.05/0.5/0.95 扫描与 LCG 噪声**逐位相同**，即内容不进结果。
- 这与本仓 `docs\dlssg-on-3080-spike.md` §7 的十组 Sintel triple 结论一致：纹理对象是硬
  校验（缺则 `0xbad0000a`），内容几乎不影响（±几十 px 的 MV 只改第四位小数；10000 px 的
  荒谬 MV 才拉坏画质）。**所以参考插件那 46 ms/帧不是 DLSS-G 的成本**，是它块匹配 + 单目
  深度链的成本（`docs\dlssg-on-3080-spike.md:74`）。
- 代价：本仓放弃了「真实 MV 进入遮挡判断」的潜在收益（spike §7 第 3 条：真实运动进来后
  深度才有可见影响）；参考插件把这条能力留着，但同样没有用 VMAF 证明它赚回了 20–40 ms。

## 4. 场景切换

| | 参考 | 本仓 |
|---|---|---|
| 判据 | 64×36 网格采样（每格取中心像素）的 \|Δ\| 均值 **且** 每通道 16 bin 直方图距离，两者都 ≥ 阈值（`scene_detection.h:24-26,57`） | 两帧亮度梯度方向直方图（9 bin 无符号，4 像素采样）的 L1/2 距离 `hog ≥ threshold`，与亮度无关；有效梯度覆盖不足 5% 的帧（黑场/渐隐）判非切镜 |
| 默认值 | 0.15（`fg\plugin.cpp:22`） | 0.12（`frame_gen.cpp`）；另有 `duplicate_threshold=0.005` 判重复帧 |
| 显式标志 | 优先看 `_SceneChangeNext/_SceneChangePrev`（`fg\plugin.cpp:86-87`） | 不看 |
| 顺序 | 先 validate 像素再判切（故意，防止坏像素被 fallback 掩盖，`fg\plugin.cpp:82-85`） | 先判切，判中就不分配输出帧（`frame_gen.cpp:175-187`） |
| 行为 | hold 左帧，`fallback=1` | 同（`fallback=1`）；另外 `fallback=2`（运行库抑制）、`3`（末对无右帧）、`4`（重复帧复制前一帧） |
| 运行库抑制 | 读回 `pOutputDisableInterpolation`，非 0 → hold | 同，但只在 `multiFrameCount<=1` 时信任（`ngx_session.cpp:1031`），多帧时只当提示 |

本仓的单指标 + 更密采样（1080p 约 9.7 万样本 vs 参考 6912 样本）对整体淡化/曝光跳变更
容易误判成切镜；参考的双条件正是为这个场合写的。

## 5. 运行库与设备

| | 参考 | 本仓 |
|---|---|---|
| 运行库来源 | 用户自备 `nvngx_dlssg.dll`（验证环境 310.9.1），用 `PathListInfo` 指 `runtime_dir`，缺失直接报错；**链接静态 `nvsdk_ngx_s.lib`**（`fg\CMakeLists.txt:1`） | 随包带 `dlssg_sm86` 代理（`version.dll` + 310.9.16 `nvngx_dlssg.dll` + `fg_kernels.dll` + `sm86_backend.dll`），**必须动态链 `nvsdk_ngx_d.lib`**，否则代理 hook 不上（`build.bat:53`、`ngx_session.cpp:1-5`） |
| 加载方式 | 无代理 | `LoadLibraryW("<proxy>\version.dll")` 必须在 `NVSDK_NGX_D3D12_Init` 之前，且要先建 `dlssg_sm86\logs`（否则代理静默失败，`ngx_session.cpp:81-100`） |
| 适配器 | 自己枚举 DXGI，跳过软件适配器，**只收 VendorId 0x10DE**，`FEATURE_LEVEL_12_0`（`src\d3d12_ctx.cpp:98-135`） | `gpu` 参数指定或自动；自动时从代理日志尾部解析 `install` 事件的 `selected_luid`（代理安装时钉死一张卡），必要时借兄弟包的日志（`ngx_session.cpp:128-215`） |
| 同进程共用 | 全局 mutex + 引用计数，同一 `runtime_dir` 与同一 device，最后一个实例才 Shutdown（`fg\fg_context.cpp:19-24,89-112`） | 单例 + `std::mutex`；命名互斥 `Local\vs-dlssg-ngx-<engine>-<pid>` 拦住 vdlssg+vxessfg 同进程混用（`ngx_session.cpp:733-747`） |
| 失败处理 | 抛错并毒化 context，后续帧统一报错（`fg\plugin.cpp:121-124`） | 每帧 `setFilterError`，session 不死；错误的 `gpu=` 会附带「代理只 patch 一张卡」的诊断（`ngx_session.cpp:837-841`） |

## 6. GPU 资源与同步

两边结构几乎一致：单 device、单 DIRECT queue、单 allocator/list/fence、固定纹理集
（RGBA16F 颜色/输出、R32F 深度、RG16F 运动、4B 抑制标志）、upload 堆 + readback 堆、
Evaluate 前全局 UAV barrier（`参考 src\d3d12_ctx.cpp:918-921` / `vs-dlssg\src\d3d12_ctx.cpp:377-402`）。

差异只在细节：
- 等待：参考 30 s 超时（「宁可失败也不要无限挂死」，`src\d3d12_ctx.cpp:1244`）；本仓
  `WaitForSingleObject(INFINITE)`（`d3d12_ctx.cpp:420-450`）。
- 计时：参考有可选 GPU timestamp query（`GpuTimings`，默认关）；本仓每帧 std 计时 + `log=1`
  打到 stderr。
- 参考 D3D12 侧还多一条视频编码队列（给 ME 用，带跨队列 fence 互等与死锁规避，
  `src\d3d12_ctx.cpp:1070-1174`）；本仓没有这一套，因为不算 MV。
- 状态跟踪：参考软件记录 speculative/committed 双份资源状态、异常时回滚；本仓直接按
  固定顺序 barrier，无回滚。

## 7. 多引擎（本仓独有）

参考插件只有 DLSS-G。本仓把 VS 滤镜层（`frame_gen.cpp`，四个 DLL 同源）和引擎层拆开，
`profiles.h:12-56` 决定命名空间/引擎/引擎包目录：

| DLL | 引擎 | 路径要点 |
|---|---|---|
| `vdlssg` / `vxessfg` | NGX DLSS-G / XeSS-FG（代理把 XeSS 模型编成自己的 CUDA kernel，不需要 `libxess_fg.dll`） | 同 `ngx_session.cpp`，颜色 RGBA16F / R10G10B10A2 |
| `vvfg` | NVIDIA Video Effects `VideoFrameGeneration`，纯 CUDA（`nvcuda.dll` 动态取符号） | 多帧 = 每个中间帧一条 lane（独立 CUDA context + 3 张 device 面）；模型只带 sm_89/sm_120，运行时把 `cuDeviceGetAttribute(attrib 76)` 报成 9、把 fatbin `e_flags`/条目 89→86 做 IAT 级解锁（`vfg_session.cpp:143-278`） |
| `vfsr4` | AMD FidelityFX FG，经 SMV 桥 + 打过补丁的 vkd3d-proton | 唯一共享 GPU 资源的一条：句柄实际是 Vulkan `OPAQUE_WIN32`，只能同 LUID 的 Vulkan 设备导入（D3D12 `OpenSharedHandle` 与 CUDA `cuImportExternalMemory` 都失败），timeline semaphore 抬值同步；回读 staging 必须 `HOST_CACHED`，否则 1080p 掉到 ~180 MB/s（`fsr4_vk_device.cpp:165-349`） |

## 8. 线程与帧生命周期

- 参考：`createVideoFilter(..., fmFrameState, ...)`（`fg\plugin.cpp:183`），`arInitial` 里
  `requestFrameFilter`，NGX 整对求值持进程级 mutex。
- 本仓：`createVideoFilter2(..., fmUnordered, nullptr, 0, ...)`（`frame_gen.cpp:452-453`），
  自己 request 源帧、声明无依赖；引擎会话全局单例 + mutex，CPU 打包/解包用
  `parallelRows`（NGX `min(hw,16)` / VFG 16 / FSR4 8）——**GPU 侧仍是一帧一同步，没有在飞队列**。
- 两边都限制「一个进程一个分辨率 + 一个引擎」。

## 9. 构建与分发

- 参考：CMake + VS，静态 CRT `/MT`，NGX SDK 与运行库都不入包，模型 `download_model.py`
  单独装；带 Vapourkit `.vkfilter`、`dlssg.py`、`vk_depth*.py`。
- 本仓：`build.bat` 一次编四个 DLL，引擎包（代理/内核/运行库/FSR4 全套 vkd3d+license）
  一起分发；没有 Vapourkit filter，面向 mpv（`mpv/*.vpy`）。
- 代码规模：参考 C++ ≈2491 行，其中 `src\d3d12_ctx.cpp` 1476 行（59%，因为它兼了
  ME + 精化 shader + 双队列 + 计时）；本仓 ≈3000 行但拆成 4 个引擎各自 ~300–1100 行，
  VS 层只有 472 行。

## 10. 若要对齐，值得抄 / 不值得抄

**值得抄（低成本、有实际后果）**
1. **时间戳**：输出 fps 翻倍 + 每帧 `_DurationNum/_DurationDen` + 插入帧 `_AbsoluteTime`
   递推。~~本仓现在依赖调用方 `AssumeFPS`~~ → 已实现（`applyTimelineProps`，`multi` 倍后
   gcd 约分写回 `vi.fpsNum/fpsDen`），下游读 `_Duration`/`_AbsoluteTime` 不再错。
2. **切镜双条件**：空间差 + 直方图同时超阈值，能消掉整体曝光/淡化造成的误判；本仓现在
   一条件即可，阈值 0.15 且采样更密，误判概率更高（`mpv-dlssg.md` 已记录 0.15 会 hold
   11% 的帧对）。
3. **像素合法性校验**：参考对越界/NaN 直接抛错；本仓把任意 float 送进运行库，出问题时
   现象发生在运行库深处。
4. **显式 `_SceneChangePrev/Next` 优先**：静态检测再准也不如源片自带的切点。—— 已实现，
   且做成「粘性」：一旦见过标记就全程听源，无标记=无切，只有从没见过标记才回退内置检测并
   warning 一次（避免给只标切点的检测器（`std.SCDetect`、K7sfunc `SCENE_DETECT`）叠加第二个意见）。

**不值得抄**
1. D3D12 Video 块匹配 + 全屏精化：spike §7 证明正常量级 MV 对输出影响在第四位小数，
   而这套东西吃掉了参考插件 20–40 ms/帧的主要开销。
2. Depth Anything V2 单目深度：深度内容在无真实 MV 时逐位不影响输出，在有 MV 时只影响
   0.007% 像素超过 8/255。
3. 每对无条件 reset 而不用跨对缓存：本仓的 `lastPairIndex/seededPairIndex` 在保持
   「乱序安全」语义下少一次上传/pass。
