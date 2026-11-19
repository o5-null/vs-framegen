# vdlssg — 用 DLSS 帧生成给视频插帧

把 NVIDIA DLSS Frame Generation（游戏里的"帧生成"）当作视频插帧器，做成一个
VapourSynth 插件。在 RTX 3080（Ampere sm_86，官方不支持帧生成）上用
`dlssg_for_sm86` 代理跑通，1080p 单张中间帧 GPU 侧约 1.36 ms。

## 结论摘要

| 问题 | 结论 |
|---|---|
| rtx-encore 能不能移植 | 不能（无源码）；但它验证过的思路可以用官方 DLSS-G 运行时直接实现 |
| 3080 能不能跑 | 能，靠 SM86 代理绕过架构 gate，见 `docs\dlssg-on-3080-spike.md` |
| 要不要自己算光流/深度 | **不用**，但纹理必须给。`nullptr` → `0xbad0000a`；内容可常量：深度 0.05/0.5/0.95 扫描逐位相同，块匹配矢量与零矢量最大只差 8/255。喂 DIS 顶配稠密光流后 VMAF 只动 +0.003（`docs\vdlssg-vs-rife422-flow.md`） |
| 切镜谁管 | 插件自己管。运行库实测不做切镜检测（`suppressed` 恒 0） |
| 画质对比 RIFE | 略输：VMAF 77.11 vs rife47 79.74（差 2.6 分），但远好于 minterpolate/blend |
| 有没有"预设" | **没有**。FG 无任何 quality preset，见 `docs\vdlssg-presets.md` |
| 多帧生成能不能用 | 能，2×–6× 均已实现；2× 画质最优，6× 需要随包 `dist\engine45`（`MaxGeneratedFrames=5`），见 `docs\vdlssg-multiframe.md` 与 `docs\vdlssg45-engine.md` |
| 换成 XeFG / FSR 4.1 Lite | XeFG 协议上可接（NGX 即接口），但要格式降位 + 真光流 + 深度；FSR 4.1 Lite 在三份参考实现里不存在。见 `docs\vdlssg-engines.md` |
| 吞吐 | 2160p 55.15 fps 输出（2×），1080p 139.98 fps；24 fps 素材实时 2× 有约 15% 余量 |
| 第三方 app 的 DLSS 4.5 能不能搬到 20/30 | 能。SmoothMyVideo 的 `dlssg2f.exe` 补三行（先载代理 + 扩 PathListInfo）+ 放宽 asar 里一处 GPU 正则即可，3080 上实测单趟 6x。见 `docs\smv-dlssg-unlock.md` |

## 插件

```python
core.std.LoadPlugin(path=r"E:\code\dlssg-vfi\plugin\vdlssg.dll")
clip = core.ffms2.Source(r"E:\code\dlssg-vfi\media\sintel_trailer_1080p.mp4")
clip = core.resize.Bicubic(clip, format=vs.RGBS, matrix_in_s="709")   # 必须 RGBS
out  = core.vdlssg.FrameGen(clip, multi=2, proxy_dir=r"E:\code\dlssg-vfi\dist\engine45",
                            gpu=0, scene_threshold=0.15, log=1)

# 可选：喂真实运动矢量（RGBS，平面0=水平/平面1=垂直，单位像素，指向上一帧）
out  = core.vdlssg.FrameGen(clip, multi=2, mv=mvclip,
                            proxy_dir=r"E:\code\dlssg-vfi\dist\engine45")

# 可选：相机描述（默认 0 即可；2/3 会如实声明帧间相机运动，实测画质更差）
out  = core.vdlssg.FrameGen(clip, multi=2, proxy_dir=r"E:\code\dlssg-vfi\dist\engine45", camera=0)

# 6 倍：必须在 engine45 引擎包上跑（stock 运行库只认到 4×）
out  = core.vdlssg.FrameGen(clip, multi=6, proxy_dir=r"E:\code\dlssg-vfi\dist\engine45")
```

| 参数 | 缺省 | 说明 |
|---|---|---|
| `clip` | — | 必须 `RGBS`（float32、三平面）；NV12/YUV 会被拒绝 |
| `multi` | 2 | 每对源帧生成 `multi-1` 张中间帧（2–6 倍帧率）。上限同时受运行时 `DLSSG.MultiFrameCountMax` 约束：stock `dlssg_sm86`（`MaxGeneratedFrames=3`）到 4×，`dist\engine45`（`=5`）到 6× |
| `mv` | 无 | 可选运动矢量剪辑：`RGBS`，与 `clip` 同尺寸/帧数/帧率；平面 0 = 水平、平面 1 = 垂直，单位像素，第 k 帧的场定义在源帧 k 网格上、指向帧 k−1。不给则用常量零场（实测对输出影响极小） |
| `proxy_dir` | 插件旁的 `engine45\`，否则插件所在目录 | 含 `version.dll` 与 `dlssg_sm86\` 的目录；`engine45` 是提取出来的 DLSS 4.5 引擎包（6× 上限） |
| `gpu` | 0 | 显卡序号 |
| `scene_threshold` | 0.15 | 切镜判定阈值（8 像素采样 luma 平均绝对差） |
| `camera` | 0 | 传给运行库的相机描述：0 = 五个单位阵 + 正交投影（默认）；1 = 真实透视投影、无帧间相机运动；2 = 透视 + 按该 pair 的运动场拟合 `clipToPrevClip`；3 = 同 2 且 `cameraMotionIncluded=false`。实测 1 输出逐位不变，2/3 让输出变化但 VMAF 反降 0.29（`docs\vdlssg-vs-rife422-flow.md`），保持 0 |
| `log` | 0 | 1 = 每张生成帧打印耗时到 stderr |

输出属性：`_DLSSGGenerated`、`_DLSSGFallback`（0 正常 / 1 切镜 / 2 抑制 / 3 末帧保持）、
`_DLSSGSceneCut`、`_DLSSGMultiFrameIndex`。

## 文档

| 文件 | 内容 |
|---|---|
| `docs\dlssg-on-3080-spike.md` | 三条硬前提、NGX 调用序列、spike 探针、原始 VMAF/吞吐 |
| `docs\vdlssg-plugin.md` | 插件设计、切镜/抑制/hold 语义、三个已修 bug 与回归入口 |
| `docs\vdlssg-throughput.md` | 四处吞吐优化详解（18.9 → 55.15 fps）、踩过的坑 |
| `docs\vdlssg-vs-models.md` | 与 RIFE/FILM/IFRNet/minterpolate/blend 的十组三帧逐组校准 |
| `docs\vdlssg-multiframe.md` | 多帧生成：SDK 语义、抑制标志、位置验证、质量与吞吐表 |
| `docs\vdlssg-presets.md` | "预设"取证（FG 无 preset）、可迁移开关清单、两项实测均无效 |
| `docs\vdlssg-engines.md` | 接入面实证：NGX 即接口、XeFG 的输入/格式契约、FSR 4.1 Lite 不存在 |
| `docs\vdlssg-vs-rife422-flow.md` | 测试数据 1080p：帧质量 + 稠密光流 + 喂真实光流（dlssg_mv）+ 耗时 |
| `docs\rtx-encore-motion-estimator.md` | Encore 自研运动估计逆向：10 个 pass、深度加权分层块匹配、S10.5 编码、可用性评估 |
| `docs\smv-dlssg-unlock.md` | 把 SmoothMyVideo 的 DLSS 4.5 从 RTX 40/50 解锁到 20/30：宿主三处补丁、asar 门限、证据与坑（产物 `dist\smv-dlssg-unlock`） |
| `docs\vdlssg45-engine.md` | `dist\engine45` 引擎包：与 SMV / stock 代理的差异、6× 上限的来源、冒烟验证与用法 |
| `docs\hsr6x-measure.md` | 星铁 PV 4K 实测：6× 速度、GT 代理画质、mvec 实测无效、切镜阈值曲线与产物路径 |
| `docs\mpv-dlssg.md` | 把 vdlssg 装进 mpv-lazy：部署清单、`MEMC_DLSSG_NV.vpy` 选项、与 RIFE 脚本的差异、五个坑 |
| `测试数据\results\report_final.md` | 上述对比的合并报告（含 VMAF/PSNR/SSIM/锐度/耗时） |

## 目录

| 目录 | 内容 |
|---|---|
| `plugin\` | 插件源码（`vdlssg.cpp` / `ngx_session.cpp` / `d3d12_ctx.cpp`）+ `build.bat` |
| `spike\` | 独立验证程序（不走 VapourSynth，用来做对照实验） |
| `dist\` | `dlssg_for_sm86` 0.4.2 代理包原件；`smv-dlssg-unlock\` = 给 SmoothMyVideo 用的解锁安装包；`engine45\` = 从 SmoothMyVideo 提取的 DLSS 4.5 引擎包（6×） |
| `vs-dlssg\` | 可直接分发的成品包（38.6 MB / 25 文件）：`bin\vdlssg.dll`、`src\`（源码 + 独立 `build.bat`）、`engine45\`、`mpv\MEMC_DLSSG_NV.vpy`、`test\`、`docs\`、根 README |
| `work\` | 代理运行目录（`version.dll`、`dlssg_sm86\`、`dlssg_sm86.ini`）+ 中间产物 |
| `ngxsdk\` | NVIDIA NGX/Streamline 头文件与 `nvsdk_ngx_d.lib` |
| `media\` | Sintel 素材、三帧/五帧取样、各模型的跑分脚本 |
| `test\` | 端到端/吞吐/一致性/日志解析脚本 |
| `framegen-bench\` | 独立 git 仓库：八引擎（DLSSG/XeSS/VFG/FSR4 + RIFE 四变体）帧生成基准，`src\` 脚本 + `results\` 图表报告；原始数据在 `测试数据\engines\` |

## 构建

```bat
cmd /c E:\code\dlssg-vfi\plugin\build.bat
```

需要 MSVC（`C:\software\msvc`，`vcvars64.bat`）、`ngxsdk\include`、`nvsdk_ngx_d.lib`、
VapourSynth R74 头文件（`E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include`）。

**必须链动态 `nvsdk_ngx_d.lib`（`/MD`）**：代理只钩真 `_nvngx.dll` 的导出，静态链接方案
永远装不上钩子。

## 环境要求

- 真实 `_nvngx.dll` 在 DriverStore（`nv_dispi.inf_amd64_feedb8c0271ca811\`），NGX loader
  自己会找到，不要拷进项目。
- `work\dlssg_sm86\logs\` 目录必须预先存在，否则零日志静默失败。
- 运行目录必须是含 `version.dll` 的目录（代理要被显式拉起）。
