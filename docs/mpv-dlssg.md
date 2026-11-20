# 把 vdlssg 装进 mpv-lazy（实时补帧）

目标：在 `测试环境\mpv-lazy` 里像 `MEMC_RIFE_NV.vpy` 一样，用 `vf set vapoursynth=…` 一键把片源补成 2×–6×。
实测可行：1080p 源、`Multi=2` 时 mpv 显示 `29.97 → 59.94 fps`，滤镜链为 `vapoursynth [file=~~/vs/MEMC_DLSSG_NV.vpy]`。

## 1. 部署清单

| 目标 | 内容 |
|---|---|
| `mpv-lazy\vs-plugins\vdlssg.dll` | `vs-dlssg\bin\vdlssg.dll`（118,784 B） |
| `mpv-lazy\vs-plugins\dlss45\` | `vs-dlssg\bin\dlss45` 整棵（`version.dll` + `dlssg_sm86\{backend,kernels,runtime,xefg,logs}`） |
| `mpv-lazy\portable_config\vs\MEMC_DLSSG_NV.vpy` | 本文配套脚本 |
| `portable_config\menu.conf` / `input_uosc.conf` | 各加一行，绑到 `vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"` |

`dlss45` 必须与 `vdlssg.dll` **同目录**：插件默认优先加载 `<DLL 所在目录>\dlss45\version.dll`，找不到才回落到 DLL 目录本身（那就只有原版 4× 上限）。`dlss45\dlssg_sm86\logs\` 必须存在，代理自己不建。

脚本里还有两级兜底：`core.vdlssg` 不存在时依次试 `..\vs-plugins\vdlssg.dll`、`<脚本目录>\vdlssg.dll`、`..\..\..\..\vs-dlssg\bin\vdlssg.dll`（开发树），所以即使 mpv-lazy 的自动加载路径变了也不会直接失败。

## 2. 脚本选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `Multi` | 2 | `<2..6>` 帧率倍数，每对相邻帧插 `Multi-1` 张。1080p 可 6（单次约 3 ms），1440p 用 3–4，**4K 只用 2** |
| `Scene` | 0.25 | 切镜阈值（三平面 8 采样平均绝对差）。0 关闭；快节奏素材 0.25 起，稳妥 0.35。默认的 0.15 会把 11% 的帧对 hold 成重复帧 |
| `Gpu` | `None` | 显卡序号；`None` 跟随 sm86 代理安装时选中的那张卡（多显卡机器别写死 0，见 `docs\vdlssg-adapter.md`） |
| `Pre_H` | 0 | `>0` 时先把源预缩到该高度再补帧，4K 6× 显存吃紧时可填 1080 |
| `Log` | False | 每帧向 stderr 打一行 `evaluate` 毫秒数 |

调试覆盖：`VDLSSG_MULTI` / `VDLSSG_SCENE` / `VDLSSG_PRE_H` / `VDLSSG_LOG` 环境变量，优先级高于脚本里的字面量（方便一条命令试不同倍数，不必改文件）。

## 3. 与 `MEMC_RIFE_NV.vpy` 的差异

- 不依赖 `k7sfunc`：没有现成的 DLSSG 封装，直接 `core.vdlssg.FrameGen`，所以也不需要 `vsmlrt` / TensorRT。
- 像素格式：**YUV 直通**——脚本把 mpv 送来的平面 YUV 直接喂给插件（插件已支持 8/10/16 bit 平面 YUV，按帧属性里的 `_Matrix`/`_Range`/`_ChromaLocation` 自行转 RGB 给引擎，结果再转回原格式）。只在源是 RGBS（浮点 32bit）或用户显式要 RGB 时才保留 `Bilinear` 转换。省掉的两趟全帧转换实测每源帧 1–3 ms（1080p），细节见 `docs\interp-fps-budget.md`。
- mpv 的 `vf_vapoursynth` 只接受 8/10bit 平面 YUV（`RGBS` 不在其中），这是 YUV 直通能成立的前提；反过来它也会把 NV12 之类的半平面先转成平面 YUV 再进 VS。
- 帧率：插件自己把 `fpsNum/fpsDen` 乘上 `Multi`（已约分）并逐帧写 `_Duration`/`_AbsoluteTime`，脚本不再 `AssumeFPS`；只有源没有帧率时才兜底一次。
- 没用 `FPS_CTRL`：那是给「源帧率过高时先降帧」的，补帧倍数是整数倍，直接 `container_fps * Multi` 即可。

## 4. 踩过的坑

1. `resize.Bilinear(range_out_s=…)` **不存在**：VapourSynth 的 resize 只有 `range` / `range_s`（输出侧）与 `range_in` / `range_in_s`（输入侧）。报错是 `Function does not take argument(s) named range_out_s`。
2. 从 RGB 转回 YUV **必须显式给矩阵**，否则 `Matrix must be specified when converting to YUV or GRAY from RGB`。脚本里 `matrix_s="709" if yuv_in else None`。
3. 插件按「进程内单实例、单帧尺寸」管理 NGX 会话：mpv 连续播放不同分辨率的文件时，前一个节点必须已释放，否则 `vdlssg: only one frame size per process is supported`。换片源尺寸报这个错就重启 mpv。
4. 4K 6× 在 3080 上是 22 ms/次、重复跑会掉到 8 fps，实时播放不可能；1080p 6× 的端到端输出约 100 fps，能跟上 120/144 Hz 屏。
5. 颜色：脚本固定 `709`，HDR/`bt.1886` 片源会被当 SDR 处理（DLSS-G 只出 8/10bit SDR，本来就不做色调映射）。

## 5. 实测

| 场景 | 结果 |
|---|---|
| 1080p（源 29.97 fps）+ `Multi=2` | mpv 显示 `59.9401 fps`，滤镜生效，无报错 |
| 4K 3840×1632 + `Multi=2` | 端到端约 46 fps 输出，可实时；`Multi=6` 掉到 16 fps 并会随显存压力继续跌 |

画质口径见 `docs\hsr6x-measure.md`：真实 6× 没有真值可比，只能看观感；拿「两帧间隔的中点」当对照时，这颗引擎只比线性 blend 好 3%。
