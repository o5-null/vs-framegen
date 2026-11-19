# vs-framegen — 用游戏帧生成给视频插帧

把 NVIDIA DLSS Frame Generation、Intel XeSS-FG、NVIDIA VFG、AMD FSR 4 做成 **VapourSynth 补帧插件**，
在 RTX 3080（Ampere sm_86，官方不支持帧生成）上通过 `dlssg_for_sm86` / IAT 代理跑通；
同一份宿主源码编译出四个 DLL，一个引擎一个插件。仓库里还有一套**八引擎横向基准**（含 RIFE 四个变体）。

```
src (vs-dlssg\src) ──┬── vdlssg.dll   DLSS-G       D3D12 + NGX（引擎跑在进程内）
                     ├── vxessfg.dll  XeSS-FG      D3D12 + NGX
                     ├── vvfg.dll     NVIDIA VFG   CUDA（每中间帧一条 context）
                     └── vfsr4.dll    AMD FSR 4    Vulkan 导入桥共享纹理
```

## 快速开始

```python
import vapoursynth as vs
core = vs.core

src = core.ffms2.Source(r"input.mkv")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")   # 必须 RGBS

out = core.vdlssg.FrameGen(src, multi=2, scene_threshold=0.25,
                           proxy_dir=r"E:\code\dlssg-vfi\vs-dlssg\bin\dlss45")
out = core.std.AssumeFPS(out, fpsnum=src.fps_num * 2, fpsden=src.fps_den)
out.set_output()
```

其它引擎只是换命名空间与 `multi` 上限（参数完全相同）：

```python
out = core.vxessfg.FrameGen(src, multi=4, proxy_dir=r"...\xessfg")  # 2×–4×
out = core.vvfg.FrameGen(src, multi=6, proxy_dir=r"...\vfg")      # 2×–6×
out = core.vfsr4.FrameGen(src, multi=2, proxy_dir=r"...\fsr4")     # 固定 2×
```

参数、输出帧属性、部署（VapourSynth 插件目录 / mpv-lazy）与已知限制见
[`vs-dlssg\README.md`](vs-dlssg/README.md)；引擎间的差异与取舍见 `vs-dlssg\docs\engines.md`。

**先了解三条硬约束**：单进程只支持一种帧尺寸；只接受 `RGBS`；插件保持输入 fps 不变而把帧数乘
`multi`，非 mpv 场景必须自己 `AssumeFPS`，否则播放成慢动作。

## 基准：八个引擎，同一把尺

`bench\` 在同一机器、同一段素材、同一套计时口径下比较吞吐（11 档分辨率，480×270 → 4K）与
1080p 生成质量（19 个真实中间帧作 GT）。完整报告 `bench\results\report.md`，口径与坑
`bench\docs\method.md`，图表在 `bench\results\charts\`、对比图在 `bench\results\compare\`。

**图表、完整数据表与 4K 生成帧 2×2 对比：[`docs\benchmark.md`](docs/benchmark.md)**

| | 1080p | 4K | 质量（VMAF） |
|---|---|---|---|
| DLSS-G | **8.00 ms/pair** | 29.38 | 58.11（锐度 0.61，最软） |
| NVIDIA VFG | 19.77 | **22.63**（几乎不随分辨率涨） | 61.43 |
| XeSS-FG | 37.63 | 142.13 | 59.61 |
| FSR 4 | 38.19 | 143.98 | 56.15 |
| RIFE 4.6 | 16.20 | 69.97 | 62.37 |
| RIFE 4.25 lite | 31.42 | 75.58 | 61.86 |
| RIFE 4.26 | 37.78 | 83.63 | 62.79 |
| RIFE 4.26 heavy | 37.34 | 105.92 | **63.19**（19 帧赢 13） |

一句话：DLSS-G 最快但最软，VFG 的耗时几乎与分辨率无关（4K 反超 DLSS-G），XeSS-FG/FSR 4 在 sm_86
上又慢又差，质量由 RIFE 系列领跑。噪声与异常（同档跨轮 5–40% 漂移）在报告里有清单，别按小数点后
两位下结论。

## 目录

| 目录 | 内容 | 进 git |
|---|---|---|
| `vs-dlssg\src\` | **插件源码**：`frame_gen.cpp`（滤镜 + 切镜检测）、`d3d12_ctx.cpp`、每引擎一个 `*_session.cpp`，`profiles.h` 决定一个 DLL 用哪套；`build.bat` 一次编四个 DLL | ✓ |
| `vs-dlssg\` | 成品包布局：`bin\` 放四个 DLL 和四个引擎包（`dlss45` / `xessfg` / `vfg` / `fsr4`），另有 `mpv\` 脚本、`test\`、`docs\` | 源码 ✓，`bin`/`obj` ✗（第三方运行时，见 `THIRD_PARTY_NOTICES.md`） |
| `bench\` | 八引擎基准：`src\` 脚本、`results\` 报告与图表快照、`docs\method.md` | ✓ |
| `docs\` | 20 篇实测与设计记录（sm86 解锁、吞吐优化、画质对比、mpv 集成），图表在 `docs\benchmark\` | ✓ |
| `spike\` | 不依赖 VapourSynth 的验证程序（NGX 调用序列、多帧、运动估计） | ✓ |
| `test\` | 端到端 / 吞吐 / 一致性 / 日志解析脚本 | ✓ |
| `media\` | 素材取样与各模型跑分脚本（`sintel_trailer_1080p.mp4` 自行准备） | 脚本 ✓ |
| `ref\` | RTX Encore 运动估计器的参考源码片段与笔记 | ✓ |
| `archive\` | 第一代单引擎 `vdlssg`（已被 `vs-dlssg\src` 取代，仅留对照） | ✓ |
| `测试数据\` | 源帧、各引擎生成帧、指标（7.5 GB） | ✗ |
| `参考插件\` `测试环境\` | SmoothMyVideo 源码/便携版、mpv-lazy 测试环境（第三方，只读） | ✗ |
| `work\` `dist\` `rife\` `rife4k\` `ngxsdk\` | 代理运行目录、引擎解锁包、模型、NVIDIA SDK | ✗ |

仓库只有几 MB 源码：生成帧（303 MB）、TRT engine 缓存、引擎包与第三方装包都能重建，不进 git。

## 构建

```bat
cmd /c E:\code\dlssg-vfi\vs-dlssg\src\build.bat
```

依赖（可用环境变量覆盖：`NGX_SDK` / `VS_INCLUDE` / `VCVARS` / `VULKAN_SDK`）：

- MSVC x64（`C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat`）
- NGX SDK：`git clone https://github.com/NVIDIA/DLSS ngxsdk`，构建链 `nvsdk_ngx_d.lib`
- VapourSynth R74+ 头文件（`E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include`）
- Vulkan SDK（只有 `vfsr4` 用）

**必须链动态 `nvsdk_ngx_d.lib`（`/MD`）**：代理只钩真 `_nvngx.dll` 的导出，静态链接永远装不上钩子。

## 环境要求

- 真实 `_nvngx.dll` 在 DriverStore（`nv_dispi.inf_amd64_*`），NGX loader 自己会找到，不要拷进项目。
- `bin\dlss45\dlssg_sm86\logs\` 目录必须预先存在，否则零日志静默失败。
- 运行目录必须是含 `version.dll` 的目录（即 `bin\dlss45` 或 `bin\xessfg`，代理要被显式拉起）；
  VFG 不需要额外代理，`vvfg.dll` 在进程内改 `nvVFXVideoFrameGeneration.dll` 的导入表。
- 每个进程只能初始化一次 NGX：同进程加载两个 NGX 系插件（`vdlssg` / `vxessfg`）会让第二个失败，
  `vs-dlssg\test\all_engines.py` 因此每个引擎起一个子进程。

## 第三方运行时与许可

引擎包不是本仓库的内容：`.gitignore` 忽略 `/vs-dlssg/bin/`，那里放构建产物与四个包
（`dlss45` / `xessfg` / `vfg` / `fsr4`）。NVIDIA 运行时以"并入应用的目标码"为准，代理与
解锁属于本机自用补丁，都不适合进 git。

| 想做的事 | 做法 |
|---|---|
| 自己跑 | 按 `docs\licensing.md` 把包放成 `bin\<引擎>\`，插件在 DLL 旁边自己找 |
| 发插件（DLL + 源码） | 可以，随分发物附 `runtime\nvngx_dlss.license.txt` |
| 转发引擎包 / 单独发 SDK 组件 | 不要，见 NVIDIA RTX SDKs License §3(a)(b) |

逐组件清单在 `THIRD_PARTY_NOTICES.md`，部署细节与获取步骤在 `docs\licensing.md`。

## 文档索引

| 文件 | 内容 |
|---|---|
| `vs-dlssg\README.md` | 插件组总览：部署、构建、参数表、性能表、已知限制 |
| `docs\vdlssg-plugin.md` | 插件设计、切镜/抑制/hold 语义、三个已修 bug 与回归入口 |
| `docs\dlssg-on-3080-spike.md` | 三条硬前提、NGX 调用序列、spike 探针、原始 VMAF/吞吐 |
| `docs\vdlssg-engines.md` | 接入面实证：NGX 即接口、XeFG 的输入/格式契约、FSR 4.1 Lite 不存在 |
| `docs\vdlssg-multiframe.md` · `docs\vdlssg45-engine.md` | 2×–6× 语义与 DLSS 4.5 引擎包（`MaxGeneratedFrames=5`） |
| `docs\vdlssg-throughput.md` | 四处吞吐优化详解（18.9 → 55.15 fps）、踩过的坑 |
| `docs\vdlssg-vs-rife422-flow.md` · `docs\vdlssg-vs-models.md` | 与 RIFE/FILM/IFRNet/minterpolate 的逐组对比；喂真实光流实测无效 |
| `docs\engines-benchmark.md` | 八引擎基准摘要（完整报告在 `bench\`） |
| `docs\benchmark.md` | **基准主页**：五张图表、11 档吞吐表、1080p 质量表、4K 生成帧 2×2 对比与原始数据链接 |
| `docs\smv-dlssg-unlock.md` · `docs\smv-vfg-sm86-unlock.md` | 把 SmoothMyVideo 的 DLSS 4.5 / VFG 解锁到 RTX 20/30 |
| `docs\hsr6x-measure.md` | 星铁 PV 4K 实测：6× 速度、GT 代理画质、切镜阈值曲线 |
| `docs\mpv-dlssg.md` | 装进 mpv-lazy：部署清单、脚本选项、五个坑 |
| `docs\licensing.md` · `THIRD_PARTY_NOTICES.md` | 第三方 DLL 的许可边界、实际放哪、从哪获取 |
| `docs\vdlssg-adapter.md` | 多显卡：代理只覆写一张卡，插件如何跟随它选卡（`0xbad00002` 的成因） |

## 致谢与商标

DLSS、NGX、VFG 是 NVIDIA 的技术；XeSS 是 Intel；FSR 是 AMD。引擎包与解锁思路来自
[SmoothMyVideo](https://github.com/flowreen/SmoothMyVideo)（MIT），RIFE 变体模型来自
[Practical-RIFE](https://github.com/hzwer/Practical-RIFE)，TensorRT 包装参考
[vs-mlrt](https://github.com/AmusementClub/vs-mlrt)。本仓库只做宿主编排与测量，不含上游模型的训练产物。
