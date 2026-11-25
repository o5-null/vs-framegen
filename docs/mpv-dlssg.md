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
发行包与手工部署都是这个位置（`vs-plugins\dlss45`）；K7 的 `MEMC_DLSSG_NV.vpy` 还会先试 `vs-plugins\bin\dlss45`，所以 0.1.0 那种多一层 `bin` 的旧包也能用。

脚本里还有两级兜底：`core.vdlssg` 不存在时依次试 `..\vs-plugins\vdlssg.dll`、`<脚本目录>\vdlssg.dll`、`..\..\..\..\vs-dlssg\bin\vdlssg.dll`（开发树），所以即使 mpv-lazy 的自动加载路径变了也不会直接失败。

## 2. 脚本选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `Multi` | 2 | `<2..6>` 帧率倍数，每对相邻帧插 `Multi-1` 张。1080p 可 6（单次约 3 ms），1440p 用 3–4，**4K 只用 2** |
| `Scene` | 0.25 | 切镜阈值（三平面 8 采样平均绝对差）。0 关闭；快节奏素材 0.25 起，稳妥 0.35。默认的 0.15 会把 11% 的帧对 hold 成重复帧 |
| `Gpu` | `None` | 显卡序号；`None` 跟随 sm86 代理安装时选中的那张卡（多显卡机器别写死 0，见 `docs\vdlssg-adapter.md`） |
| `Pre_H` | 0 | `>0` 时先把源预缩到该高度再补帧，4K 6× 显存吃紧时可填 1080 |
| `Preset` | `"a"` | `<a|c>` DLSS-G 模型预设：`a` 默认模型（历史行为）；`c` = FG Lite，每生成帧 GPU 时间约少 13%，静态画面与 `a` 逐像素相同、HUD/遮挡边缘略软 |
| `Log` | False | 每帧向 stderr 打一行 `evaluate` 毫秒数 |

`Preset` 的机制在插件侧：DLSS-G 的预设不是 NGX 参数，而是引擎包 `<proxy_dir>\dlssg_sm86.ini` 的 `[DLSSG] Preset`，只有装载 `version.dll` 时读一次、钉死在进程上。插件因此在装载前改写这一行，**跑过一次 `c` 之后包里的 INI 就停在 `C`**（下次不写 `preset=` 的脚本也会跑 C），恢复默认用 `preset="a"` 跑一次即可。同一进程内换档只能重建滤镜。细节与实测见 `vs-dlssg\docs\vdlssg-plugin.md` 的「模型预设」一节。

调试覆盖：`VDLSSG_MULTI` / `VDLSSG_SCENE` / `VDLSSG_PRE_H` / `VDLSSG_PRESET` / `VDLSSG_LOG` 环境变量，优先级高于脚本里的字面量（方便一条命令试不同倍数，不必改文件）。

`vf` 的 `user-data` 优先级最高，可以从 mpv 侧传参：`--vf=vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy":user-data="multi=4;scene=0.35;preset=c"`（值里有 `=`/`;` 必须整体加引号）。OSD 菜单调参见 `docs\mpv-osd-vpy-params.md`。

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

## 6. 播放中改参数

`Multi/Scene/Pre_H/Preset/Log` 可以通过 `vf=vapoursynth` 的 `user-data` 子参数从 mpv 侧传进脚本，配合 `k7f_vpypar.lua` 做 OSD 菜单调参（改完重建该条滤镜，引擎重新初始化；`preset` 换档同样只能靠重建）。
机制、代价与其它四条通路的可行性判定见 `docs\mpv-osd-vpy-params.md`。

## 7. 算力不足时自动降级（`k7f_memc_guard.lua`）

mpv 本身**没有**「插帧来不及就输出源帧」的开关，纯配置只能选「丢哪种帧」：

| 配置 | 实际行为 | 观感 |
|---|---|---|
| `video-sync=audio` + 默认 `framedrop=vo` | 视频按音频时钟，迟到的输出帧在 VO 丢弃（仍要等滤镜算完） | 不漂移，但会周期性顿 |
| `video-sync=display-resample` | 重采样音频去追视频，音频跟着变慢 | 不丢帧，但声音变慢/变调 |
| `video-sync=display-resample-vdrop` | 保持音频速度，丢视频帧补漂移 | 比上一条好，仍会顿 |
| `framedrop=decoder` | 在滤镜**之前**丢帧 | **不要用**：源帧不连续，插值闪烁/鬼影 |

真正有效的是把插帧滤镜整个禁用，让 mpv 直接显示源帧。`portable_config\scripts\k7f_memc_guard.lua`
就是做这件事：每 0.5 s 采样一次，用两个信号判断「跟不上」——

- `lag = 1 - (Δtime-pos / Δ墙钟) / speed`：播放被拖慢的比例（`display-resample` 下有效）
- `Δframe-drop-count / Δ墙钟` 与 `Δmistimed-frame-count / Δ墙钟`：每秒丢弃/错过的帧（`audio` 下有效）

连续 3 s 超阈值（默认速度 < 92% 或丢帧 > 5 fps）就 `vf toggle @VDLSSG` 禁用滤镜；
之后等待 30 s 试探性启用，若再跟不上则退避翻倍（上限 300 s）。用标签切换 `enable`
标志而不是删除滤镜，参数仍留在 `vf` 列表里，`k7f_vpypar` 的状态不会被清空。

代价：启用/禁用都要重建滤镜链，候选引擎（DLSSG/TensorRT）会重新初始化，切换瞬间
会卡一下（试探期默认 5 s 内不作判定）。所以阈值不要调得太敏感，否则会在两种状态间反复跳。
每次切换后脚本还会重置采样基线并静默 `source_grace`（默认 2 s），避免 time-pos 追赶
旧进度时误报「补帧已关闭但仍不流畅」。

```conf
# script-opts.conf（可选，默认值已匹配 k7f_vpypar）
script-opts-append = k7f_memc_guard-enabled=yes
script-opts-append = k7f_memc_guard-label=VDLSSG
script-opts-append = k7f_memc_guard-script=~~/vs/MEMC_DLSSG_NV.vpy
```

```conf
# input_uosc.conf（可选）
Ctrl+v  script-binding k7f_memc_guard/toggle
v       script-binding k7f_memc_guard/status
```

脚本消息：`on` / `off` / `now`（立即降级，测试用）/ `status`。

> `mpv-playkit\portable_config\` 是上游每日覆盖目录，不能直接加这个脚本；要进发行包
> 只能走拓展包的 `portable_config/**` 覆盖层，或等上游 K7sfunc 提供同等能力。

包里四个 VS 补帧预设的实测（吞吐、XeSS-FG 3× 崩溃、新旧 vpy 差异）见
[`mpv-playkit-vs-engine-check.md`](mpv-playkit-vs-engine-check.md)。

想在自己机器上量「VS-DLSSG / VS-VFG / 原生滤镜」的吞吐，用包里的 `installer\mpv-FilterBench.bat`
（1080p / 2k / 4k 可选，回车全测）：口径与实测数字见
[`mpv-filter-bench.md`](mpv-filter-bench.md)。


