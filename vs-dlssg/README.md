# vs-dlssg

VapourSynth 的补帧插件组：默认是 DLSS-G（`vdlssg.FrameGen`），带一个自包含的 **DLSS 4.5（310.9.16）引擎包**，在 RTX 30 系列上通过 sm86 解锁跑 2×–6× 整数倍补帧。同一份源码还编译出三个独立插件，一个引擎一个 DLL：`vxessfg`（Intel XeSS-FG）、`vvfg`（NVIDIA Video Frame Generation）、`vfsr4`（AMD FSR 4 帧生成）。引擎差异、部署与限制见 `docs\engines.md`。

- 输入 RGB 浮点（`RGBS`），输出同样帧率、`multi` 倍帧数
- 引擎跑在进程内（D3D12 + NGX / CUDA），不经 `dlssg2f.exe` 进程外转发
- 4K 2× 可实时，1080p 6× 约 3 ms/次
- 只做补帧，不做色调映射：HDR 片源需要自己解码到 RGBS 并写回传递函数

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

## 用法

```python
import vapoursynth as vs
core = vs.core

src = core.ffms2.Source(r"input.mkv")
src = core.resize.Bicubic(src, format=vs.RGBS, matrix_in_s="709")

out = core.vdlssg.FrameGen(
    src,
    multi=6,                 # 2..6，输出 multi 倍帧数
    scene_threshold=0.25,    # 0 关闭切镜检测
    proxy_dir=r"E:\...\vs-dlssg\bin\dlss45",   # 可省略：默认 <DLL 目录>\bin\dlss45
)
out = core.std.AssumeFPS(out, fpsnum=src.fps_num * 6, fpsden=src.fps_den)
out.set_output()
```

| 参数 | 缺省 | 说明 |
|---|---|---|
| `multi` | 2 | 2..6；每对相邻帧插 `multi-1` 张，帧率正好乘 `multi` |
| `mv` | 无 | 可选光流剪辑（RGB，2 或 3 平面，全尺寸或半尺寸）。**实测这颗运行库基本不使用 mvec**（±30 px 常数场只让输出变化 1.5e-4），可以不给 |
| `scene_threshold` | 0.15 | 相邻帧三平面 8 采样平均绝对差超过它就判切镜、该对直接 hold。快节奏素材建议 0.25–0.35：实测该片 10 s 内 0.15 会 hold 掉 11% 的帧对 |
| `proxy_dir` | `<DLL 目录>\dlss45` | 引擎包目录，必须含 `version.dll` 与 `dlssg_sm86\` |
| `gpu` | 无（自动） | DXGI 适配器序号。不写时跟随 sm86 代理：载入代理后读它 install 记录里的 `selected_luid`，选同一张卡；没有记录就取接显示器的 NVIDIA 卡。多显卡机器上旧的 `gpu=0` 可能正是代理没覆写的那张，会报 `0xbad00002` |
| `camera` | 0 | 相机类型（0 默认 / 1 第一人称 / 2 第三人称 / 3 工具） |
| `log` | 0 | 每帧向 stderr 打一行毫秒数 |

输出帧属性：`_DLSSGGenerated`（1 = 生成帧）、`_DLSSGFallback`（0 正常 / 1 切镜 hold / 2 抑制 hold / 3 末帧 hold）、`_DLSSGSceneCut`、`_DLSSGMultiFrameIndex`。

其它引擎只是换命名空间与 `multi` 上限（参数完全相同）：

```python
out = core.vxessfg.FrameGen(src, multi=4, proxy_dir=r"...\xessfg")  # Intel XeSS-FG，2×–4×
out = core.vvfg.FrameGen(src, multi=6, proxy_dir=r"...\vfg")      # NVIDIA VFG，2×–6×
out = core.vfsr4.FrameGen(src, multi=2, proxy_dir=r"...\fsr4")     # AMD FSR 4
```

VFG 每个中间帧要一条独立 CUDA context（模型把「上一帧」状态存在 context 里），1080p 约 1.4 GB 显存 + 0.5–0.7 s 建 lane，6× 在 10 GB 卡上可行、4K 不可行，细节见 `docs\engines.md`。

## 性能（RTX 3080，星铁 PV 4K 源，每配置 30 对 × 2 轮取最好）

| 分辨率 | multi=2 | multi=4 | multi=6 |
|---|---|---|---|
| 1080p 1920×816 | 108.2 fps 输出 / 4.42 ms | 98.8 fps / 3.29 ms | 101.0 fps / 3.11 ms |
| 4K 3840×1632 | 45.9 fps / 7.89 ms | 43.0 fps / 5.53 ms | 16.0 fps / 22.62 ms |

（fps 是含解码与 RGBS 转换的端到端输出帧率；ms 是 NGX 单次 `evaluate`。4K 6× 第二次重复会掉到 8.3 fps，显存与带宽吃紧，实时播放不可行。）

## 已知限制

1. **单进程只支持一种帧尺寸**：换分辨率要重启进程，否则报 `vdlssg: only one frame size per process is supported`。
2. 只接受 `RGBS`（`cfRGB` + `stFloat` + 32 bit + 3 平面），YUV 片源请先 `resize`。
3. 插件保持输入 fps 不变却把帧数乘 `multi`，非 mpv 场景要自己 `AssumeFPS`，否则播放成慢动作。
4. 不做 HDR 色调映射，输出 8/10 bit SDR 观感；HDR 片源需自行处理传递函数与元数据。
5. 真实 6× 没有真值可比：用「两帧间隔的中点」当对照时，这颗引擎只比线性 blend 好 3%（MAE 0.156 / VMAF 28.3）；真实用法（相邻帧 + 1/6 步长）观感可用，见 `docs\hsr6x-measure.md`。
