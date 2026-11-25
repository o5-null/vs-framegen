# mpv 补帧滤镜跑分：VS 滤镜 vs 原生滤镜

2026-11-25。给分发包加了一个双击可用的入口，用同一段素材、同一倍数、同一分辨率，
把「VS 滤镜（`vapoursynth` + `MEMC_DLSSG_NV.vpy`）」和「原生滤镜（`vf_playkit`）」各跑一遍，
比较**每秒能喂进多少输入帧**。本文记录口径、实测数字与踩过的坑。

## 用法

包里的 `installer\mpv-补帧滤镜跑分.bat`：

1. 双击；**不需要准备素材**，直接回车就会自动生成一段 10 秒 1920x1080 测试片段
   （缓存在 `%TEMP%\mpv-filter-bench\bench-src.mp4`，删掉即重新生成）。也可以把视频拖进窗口、
   粘贴路径，或事先改名成 `installer\bench-clip.mkv|mp4`（有它就不再问）。
2. 问分辨率：`1080p` / `2k` / `4k`，可以写多个（空格或逗号分隔）；**直接回车 = 三个全测**。
3. 问倍数 `multi`（2–6）；**直接回车 = 2**。
4. 预检：两条路径各用 40 帧试跑一次，确认滤镜真的初始化了（失败的路径整段跳过并警告）。
5. 每个分辨率先跑 VS 滤镜、再跑原生滤镜，最后打印汇总表并写入
   `installer\mpv-FilterBench-result.tsv`（`tag / WxH / multi / frames / elapsed_s / out_fps / src_fps / note`）。
6. 再把系统/显卡/驱动/播放器版本和这轮结果整理成 `installer\report.txt`，可以直接发给别人。

免交互（批处理/自己脚本里调用）：

```bat
installer\mpv-补帧滤镜跑分.bat "E:\视频\clip.mkv" "1080p 4k" 2
```

## 口径

| 项 | 取值 | 为什么 |
| --- | --- | --- |
| 素材 | 真实视频（或自动生成的 mp4）+ `hwdec=d3d11va` | `vf_playkit` 只吃 D3D11 硬件帧；合成源 `lavfi` 和 `--vo=null` 都会让滤镜静默不执行 |
| 分辨率 | `--vf=d3d11vpp=scale=W/SRCW,<补帧滤镜>` | 换分辨率不必换素材，D3D11 视频处理器在硬件域内缩放，按**宽度**算倍率 |
| 播放 | `--untimed --frames=600` | 不算解码/呈现的时钟限制，只测吞吐 |
| 呈现 | `d3d11-flip=no` + `d3d11-sync-interval=0` + `autofit=960x540` | 默认 flip/vsync 会把结果压到 66fps；小窗口降低呈现开销 |
| 计时 | Lua 在 `playback-restart` 记 `mp.get_time()`，`end-file` 收尾 | NGX 初始化不进分母 |
| 指标 | `out_fps = frames / elapsed`；`src_fps = out_fps / multi` | 前者是呈现吞吐，后者是"等效能吃多少源帧" |
| 有效性 | 预检时在 `--log-file` 里找 `[vapoursynth] initialized.` / `playkit: vdlssg bridge ready` | 滤镜初始化失败时 mpv 只是禁用滤镜继续播，会跑出虚高的假数字 |

两条路径的命令（倍数靠 `VDLSSG_MULTI` 传给 VS 脚本，避免 `user-data` 里的 `;` 引号地狱）：

```
VS      --vf=d3d11vpp=scale=X,vapoursynth=portable_config/vs/MEMC_DLSSG_NV.vpy
原生    --vf=d3d11vpp=scale=X,playkit=multi=N:log=no:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45
```

## 报告（report.txt）

`:report` 段把 `MPV_BENCH_*` 环境变量交给 `installer\mpv_filter_bench_report.ps1`，生成
`installer\report.txt`，四段：

| 段 | 内容 |
| --- | --- |
| 系统 | Windows 版本/内部版本、架构、物理内存、CPU 型号/核数/线程/标称频率（`Win32_OperatingSystem` / `Win32_Processor`） |
| 显卡 / 驱动 | 每块 `Win32_VideoController` 的名字、驱动版本与日期、当前分辨率/刷新率，外加 `nvidia-smi` 的卡名/驱动/显存（虚拟显示器也会列出来，属正常） |
| 播放器 | `mpv --version` 的第一行 + `FFmpeg version` 行 |
| 测试条件 + 结果 | 素材路径（自动生成会标注）、源宽度、目标分辨率、multi、播放参数、两条 vf 命令行，结果表按 8 列对齐 |

`Win32_VideoController.AdapterRAM` 对 4GB 以上的卡会报成 4.0 GB，显存以 `nvidia-smi` 那行为准。
报告用 UTF-8 带 BOM 写，记事本/浏览器/微信文件预览都能正常显示中文。



自动生成的 1920x1080 30fps 片段（双击后的默认路径），multi=2，GPU 空载实测：

| 分辨率 | VS 滤镜 out/src fps | 原生滤镜 out/src fps |
| --- | --- | --- |
| 1080p（1920x1080） | 149.7 / 74.8 | 51.2 / 25.6 |
| 2k（2560x1440） | 96.0 / 48.0 | 53.7 / 26.8 |
| 4k（3840x2160） | 52.2 / 26.1 | 40.3 / 20.1 |

早期用 1920x816 素材（2.35:1）测的表，含 6× 档，留作对比：

| 分辨率 | multi | VS 滤镜 out/src fps | 原生滤镜 out/src fps |
| --- | --- | --- | --- |
| 1080p（1920x816） | 2 | 155.7 / 77.8 | 53.9 / 26.9 |
| 1080p | 6 | 166.6 / 27.8 | 86.4 / 14.4 |
| 2k（2560x1088） | 2 | 117.9 / 58.9 | 55.8 / 27.9 |
| 4k（3840x1632） | 2 | 61.0 / 30.5 | 47.1 / 23.5 |
| 4k | 6 | 65.9 / 11.0 | 78.6 / 13.1 |

同一台机器多次重跑波动约 ±15%（原生滤镜更明显），趋势稳定：**1080p/2k 上 VS 路径快 2–3 倍，
4K 且高倍数时原生滤镜反超**（VS 路径在 4K 6× 掉到 11 的等效源帧）。

对照项：同参数下无滤镜 607 out fps、只加 `d3d11vpp` 572 fps，所以瓶颈不在呈现端。
`PLAYKIT_TIMING=1` 显示 `vf_playkit` 内部 `process=5–8ms/对`、`copy_wait<0.5ms`，
而端到端是 34ms/对（1080p 2×）–68ms/对（6×），差额来自 D3D11↔D3D12 的 GPU 同步等待。

## 坑

- `--include` 必须写 `--include=文件`（空格分隔报 `option requires parameter`）。
- `--vf=` 的参数里 `~~/` **不会展开**，VS 脚本要写相对包根目录的 `portable_config/vs/...`。
- mpv 会把脚本名里的 `-` 归一化成 `_`：`--msg-level` 前缀、脚本内求值名都要用 `mpv_filter_bench`；
  `script-opts` 在这条路径上取不到值，参数改用环境变量传。
- Lua 的 `print()` 与 mpv 自身日志都走 stdout；`[vdlssg] convertOutput ...` 那类噪音才走 stderr。
  所以要留完整运行日志就用 `--log-file=`（自带 debug 级），别指望 `2>文件`。
- 自动生成素材必须用 **mp4**：matroska 输出在这里写不出帧率（`container-fps=(unavailable)`），
  vpy 拿到 0 会直接报 `AssumeFPS: invalid framerate specified`；`--ofps` 选项不存在，加了产出 0 字节。
- 滤镜初始化失败时 mpv 只打印 `Disabling filter ... because it has failed.` 然后继续播，
  fps 会虚高（VS 三档都能报 1300+），所以必须先预检并在日志里找**成功标记**。
- `--input=no`、`--terminal=no` 会连 `--term-playing-msg` 一起关掉（探片源宽度靠它），只加 `<nul` 防吞输入即可。
- 探片源宽度那次的 mpv 会占住重定向的 stdin，交互输入只在真实控制台可靠——批处理请用上面三参数形式。
- 编辑这套脚本：`.bat` 必须存成 **ANSI（GB18030）+ CRLF**（UTF-8 会让 cmd 错行、中文乱码）。
- **VS 滤镜定位 VSScript 用包内副本**：脚本 `set VSSCRIPT_PATH=<包根>\Lib\site-packages\vapoursynth\vsscript.dll`
  （`mpv-src\video\filter\vf_vapoursynth.c:819` 支持这个环境变量）。包里那份往上四层正好是包根，
  能自己找到 `python.exe`/`python3.dll`，不读 `%APPDATA%\VapourSynth\vapoursynth.toml`。
  这台机器上 PATH 里的旧 `vapoursynth.exe config` 会把那份 toml 写成 2 字段（R82 要 3 字段、第三个是
  `python.exe` 的 mtime），一被重写就整段 `Failed to initialize VapourSynth VSScript library`——
  别去修全局 toml，跑分只认包内副本。
- 报错 `Failed to initialize VapourSynth VSScript library: Python executable and library path
  couldn't be determined...` 就是上面那条（VS 预检会整段跳过，只剩原生滤镜那一行）。
- 报告要用 **UTF-8 带 BOM** 写（PS 5.1 读无 BOM 按 ANSI，中文乱码）；`& 'mpv.com'` 在 PowerShell 里
  不查当前目录，要传绝对路径（脚本已改用 `%CD%\mpv.com` 并在 ps1 里兜底 `Join-Path`)。
- 同一台机器上跑别的 GPU 负载（例如另一场并发基准）会让结果整体慢 3–4 倍，报告里的数字务必在空载时测。

## 落地位置

| 文件 | 作用 |
| --- | --- |
| `mpv-playkit\installer\mpv-补帧滤镜跑分.bat` | 交互入口（取素材、问分辨率/倍数、逐档跑两条路径） |
| `mpv-playkit\installer\mpv-FilterBench.conf` | 公共播放参数（hwdec / flip / untimed / frames / 窗口） |
| `mpv-playkit\installer\mpv_filter_bench.lua` | 计时探针（记时、算 fps、追加 TSV） |
| `mpv-playkit\installer\mpv_filter_bench_report.ps1` | 出 `installer\report.txt`（系统/显卡/驱动/播放器 + 测试条件 + 结果表） |

打包：`mpv-playkit\manifests\components.yml` 的 `bench-scripts` 组件（kind=repo-files）把这
四个文件落到包的 `installer/` 下——installer/ 其余上游脚本仍然冻结、不进包，见
`mpv-playkit\docs\DECISIONS.md` 第 32 条。
