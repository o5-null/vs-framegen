# vs-dlssg

VapourSynth 的补帧插件组：默认是 DLSS-G（`vdlssg.FrameGen`），带一个自包含的 **DLSS 4.5（310.9.16）引擎包**，在 RTX 30 系列上通过 sm86 解锁跑 2×–6× 整数倍补帧。同一份源码还编译出三个独立插件，一个引擎一个 DLL：`vxessfg`（Intel XeSS-FG）、`vvfg`（NVIDIA Video Frame Generation）、`vfsr4`（AMD FSR 4 帧生成）。引擎差异、部署与限制见 `docs\engines.md`。

- 直接吃平面 YUV（8/10/16 bit，4:2:0 / 4:2:2 / 4:4:4）或 `RGBS`，输出同格式、`multi` 倍帧数、`multi` 倍帧率
- YUV 进出：不再需要 `resize` 到 RGBS 再转回来，省掉两趟全帧转换（1080p `multi=3` 实测每源帧省 1–3 ms，见下文）
- 引擎跑在进程内（D3D12 + NGX / CUDA / Vulkan），不经 `dlssg2f.exe` 进程外转发
- 只做补帧，不做色调映射：HDR 片源仍要自己处理传递函数与元数据

## 目录

| 路径 | 内容 |
|---|---|
| `bin\vdlssg.dll` `bin\vxessfg.dll` `bin\vvfg.dll` `bin\vfsr4.dll` | 编译产物（x64），一个引擎一个插件 |
| `src\*.cpp` `src\*.h` | 共用 `frame_gen.cpp`（滤镜、切镜检测）+ `d3d12_ctx.cpp`（D3D12 上下文）+ 各引擎的 `*_session.cpp`；`profiles.h` 决定一个 DLL 用哪套 |
| `src\build.bat` | MSVC 构建脚本，一次编四个 DLL，`NGX_SDK` / `VS_INCLUDE` / `VCVARS` 可用环境变量覆盖 |
| `bin\dlss45\` | DLSS 4.5 引擎包：`version.dll` 解锁代理 + `dlssg_sm86\`（backend/kernels/runtime/xefg）+ `dlssg_sm86.ini`（`MaxGeneratedFrames=5`，即单趟 6×） |
| `bin\xessfg\` | 同一引擎包，`Engine=XeSS`、`MaxGeneratedFrames=3`（XeSS-FG，4×） |
| `bin\vfg\` | NVIDIA Video Effects SDK 四文件（VFG，进程内 IAT 解锁 sm_89 内核） |
| `bin\fsr4\` | FSR 4 桥：`smv_fsrfg_bridge.dll` + AMD 签名 DLL + vkd3d-proton |
| `test\` | `mf6.vpy`（DLSS 6×）、`xess4.vpy`、`vfg4.vpy`、`fsr4.vpy`、`all_engines.py`（四引擎各起子进程跑一遍）、`smoke.py` |
| `mpv\MEMC_DLSSG_NV.vpy` | mpv-lazy 用的一键补帧脚本 |
| `docs\` | 引擎对比（`engines.md`）、mpv 接入、插件参数、多帧语义、4K 实测 |


## 部署

**VapourSynth**：把 `bin\vdlssg.dll` 和整个 `bin\dlss45\` **平铺**到同一个插件目录，例如

```
<vapoursynth>\vapoursynth64\plugins\vdlssg.dll
<vapoursynth>\vapoursynth64\plugins\dlss45\...
```

插件默认优先加载 `<DLL 所在目录>\dlss45\version.dll`，找不到才回落到 DLL 目录本身（那就只有原版 4× 上限）。`dlss45\dlssg_sm86\logs\` 必须存在。

**mpv-lazy**：`vs-plugins\vdlssg.dll` + `vs-plugins\dlss45\`，再把 `mpv\MEMC_DLSSG_NV.vpy` 放到 `portable_config\vs\`，菜单里执行 `vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"`。详见 `docs\mpv-dlssg.md`。

## 构建

```
cmd /c vs-dlssg\src\build.bat
```

需要 MSVC（x64）、[NGX SDK](https://github.com/NVIDIA/DLSS) 的头与 `nvsdk_ngx_d.lib`、VapourSynth R74+ 头。默认按下面的布局找依赖，任一不存在会直接报错退出：

```
NGX_SDK      = <包>\..\..\ngxsdk
VS_INCLUDE   = E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include
VCVARS       = C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat
```

插件按 `VS_USE_LATEST_API` 编译（当前头是 API 4.2），注册时用 `min(运行时 API, 头文件 API)` 声明版本，所以同一个 DLL 在 API 4.0 / 4.1 / 4.2 的核上都能加载；`clip` 与 `mv` 以 `rpGeneral` 声明为依赖（4.1 起 `getNodeDependency` 能看到这两条边），`log=1` 会打印核的 API 版本。

## 用法

```python
import vapoursynth as vs
core = vs.core

src = core.ffms2.Source(r"input.mkv")          # 420P10 等平面 YUV 直接喂

out = core.vdlssg.FrameGen(
    src,                     # RGBS 也接受（float32 RGB）
    multi=6,                 # 2..6，输出 multi 倍帧数、multi 倍帧率
    scene_threshold=0.25,    # 仅当上游没有 _SceneChange* 属性时才用；0 关闭内置检测
    proxy_dir=r"E:\...\vs-dlssg\bin\dlss45",   # 可省略：默认 <DLL 目录>\dlss45
)
out.set_output()
```

YUV 的颜色编码不做成参数，而是每对从源帧属性里取（mpv 的 `vf vapoursynth` 会自动写全）：

| 属性 | 缺省 | 说明 |
|---|---|---|
| `_Matrix` / `_ColorSpace` | 1（BT.709） | 先读 `_Matrix`，没有再读 `_ColorSpace`（mpv 只写后者，两者数值同为 H.273 编码） |
| `_Range` | 0（limited） | 1 = full |
| `_ChromaLocation` | 0（left） | 0 left / 1 center / 2 top-left / 3 top；色度用双线性取样 |

| 参数 | 缺省 | 说明 |
|---|---|---|
| `multi` | 2 | 2..6；每对相邻帧插 `multi-1` 张，帧率正好乘 `multi` |
| `mv` | 无 | 可选光流剪辑（RGB，2 或 3 平面，全尺寸或半尺寸）。**实测这颗运行库基本不使用 mvec**（±30 px 常数场只让输出变化 1.5e-4），可以不给 |
| `scene_threshold` | 0.15 | **只在源帧完全没有 `_SceneChangePrev`/`_SceneChangeNext` 时才用**（会 warning 一次）：相邻帧按 8 像素步长采样的平均绝对亮度差超过它就判切镜、该对直接 hold（RGBS 平均三个通道，YUV 只看亮度平面并按 `_Range` 归一化，两边同为 [0,1] 亮度，阈值含义一致）。快节奏素材建议 0.25–0.35：实测该片 10 s 内 0.15 会 hold 掉 11% 的帧对。上游带切镜属性时（K7sfunc `SCENE_DETECT`、`std.SCDetect`）本参数完全不起作用 |
| `proxy_dir` | `<DLL 目录>\dlss45` | 引擎包目录，必须含 `version.dll` 与 `dlssg_sm86\` |
| `preset` | 无（沿用引擎包；出厂为 A） | **仅 vdlssg**：`a` = 历史模型，`c` = FG Lite。装载代理前改写 `<proxy_dir>\dlssg_sm86.ini` 的 `[DLSSG] Preset`，进程内固定（同一进程换值会报错，换预设要另起进程）。静态画面与 `a` 逐像素相同；运动段 PSNR 45.6–48.0 dB，1080p 每生成帧 p50 GPU 时间 −13%、整体 wall −4.5%。`b` 直接报错：它要 HUD-less 画面 + UI 颜色/遮罩，本插件给不出，运行库也只会按 `a` 跑 |
| `gpu` | 无（自动） | DXGI 适配器序号。不写时跟随 sm86 代理：载入代理后读它 install 记录里的 `selected_luid`，选同一张卡；没有记录就取接显示器的 NVIDIA 卡。多显卡机器上旧的 `gpu=0` 可能正是代理没覆写的那张，会报 `0xbad00002` |
| `camera` | 0 | 相机类型（0 默认 / 1 第一人称 / 2 第三人称 / 3 工具） |
| `log` | 0 | 每帧向 stderr 打一行毫秒数 |

输出帧属性：`_DLSSGGenerated`（1 = 生成帧）、`_DLSSGFallback`（0 正常 / 1 切镜 hold / 2 抑制 hold / 3 末帧 hold）、`_DLSSGSceneCut`、`_DLSSGMultiFrameIndex`。帧率按 `multi` 乘好（`fpsNum` 已约分），每帧另写 `_DurationNum/_DurationDen`，插入帧按一个输出帧间隔推 `_AbsoluteTime`；源的 `_SceneChangePrev/_SceneChangeNext` 会搬到输出时间轴上（组首 / 组尾），所以下游按帧计时或按属性判切都正确。

其它引擎只是换命名空间与 `multi` 上限（参数完全相同）：

```python
out = core.vxessfg.FrameGen(src, multi=4, proxy_dir=r"...\xessfg")  # Intel XeSS-FG，2×–4×
out = core.vvfg.FrameGen(src, multi=6, proxy_dir=r"...\vfg")      # NVIDIA VFG，2×–6×
out = core.vfsr4.FrameGen(src, multi=2, proxy_dir=r"...\fsr4")     # AMD FSR 4
```

VFG 每个中间帧要一条独立 CUDA context（模型把「上一帧」状态存在 context 里），1080p 默认 medium 档约 0.7 GB 显存（high 档 1.4 GB）+ 1.5–2.5 s 建 lane（high 0.4–0.75 s），6× 在 10 GB 卡上可行、4K 不可行，细节见 `docs\engines.md`。

**仅 vvfg** 有模型档位 `mode`：`0` low / `1` medium（默认）/ `2` high。medium 与 high 的 PSNR-Y 只差 0.4 dB，1080p 每张内插约 3 ms（high 约 30 ms）、每条 lane 约 0.7 GB（high 1.4 GB），所以默认取 medium；high 留给画质优先的静态画面。环境变量 `VFG_MODE` 可临时换档。

## 性能（RTX 3080，星铁 PV 4K 源，每配置 30 对 × 2 轮取最好）

| 分辨率 | multi=2 | multi=4 | multi=6 |
|---|---|---|---|
| 1080p 1920×816 | 108.2 fps 输出 / 4.42 ms | 98.8 fps / 3.29 ms | 101.0 fps / 3.11 ms |
| 4K 3840×1632 | 45.9 fps / 7.89 ms | 43.0 fps / 5.53 ms | 16.0 fps / 22.62 ms |

（fps 是含解码与格式转换的端到端输出帧率；ms 是 NGX 单次 `evaluate`。4K 6× 第二次重复会掉到 8.3 fps，显存与带宽吃紧，实时播放不可行。）

YUV 直通相对「先 `resize` 到 RGBS、再转回 YUV」的收益（1080p sintel，240 输出帧，每源帧毫秒，越低越好）：

| 引擎 | YUV 直通 | RGBS 中转 | 差 |
|---|---|---|---|
| `vdlssg`（multi=3） | 30.86 | 32.59 | −1.73 |
| `vxessfg`（multi=3） | 27.98 | 29.18 | −1.20 |
| `vvfg`（multi=3） | 98.40 | 99.70 | −1.30 |
| `vfsr4`（multi=2） | 22.40 | 25.25 | −2.85 |

省的是 VS 侧两趟全帧转换；引擎本身的开销不变，所以帧率越高、引擎越慢，相对收益越小。

## 已知限制

1. **单进程只支持一种帧尺寸**：换分辨率要重启进程，否则报 `vdlssg: only one frame size per process is supported`。
2. 接受两种输入：`RGBS`（`cfRGB` + `stFloat` + 32 bit + 3 平面）或平面 YUV（`cfYUV`、`stInteger`、8/10/16 bit、3 平面、`subSamplingW/H <= 1`，即 4:4:4 / 4:2:2 / 4:2:0）。YUV 片源直接给，不必再 `resize` 到 RGBS；同一进程只允许一种像素布局，换布局要重启进程。
3. 插件把输出帧率乘上 `multi` 并逐帧写 `_Duration`/`_AbsoluteTime`，**不要**再自己 `AssumeFPS`（会乘两次）。
4. 不做 HDR 色调映射，输出 8/10 bit SDR 观感；HDR 片源需自行处理传递函数与元数据。
5. 真实 6× 没有真值可比：用「两帧间隔的中点」当对照时，这颗引擎只比线性 blend 好 3%（MAE 0.156 / VMAF 28.3）；真实用法（相邻帧 + 1/6 步长）观感可用，见 `docs\hsr6x-measure.md`。
