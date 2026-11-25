# mpv 补帧滤镜跑分：VS 滤镜 vs 原生滤镜

2026-11-25。给分发包加了一个双击可用的入口，用同一段素材、同一倍数、同一分辨率，
把「VS 滤镜（`vapoursynth` + `MEMC_DLSSG_NV.vpy`）」和「原生滤镜（`vf_playkit`）」各跑一遍，
比较**每秒能喂进多少输入帧**。本文记录口径、实测数字与踩过的坑。

## 用法

包里的 `installer\mpv-补帧滤镜跑分.bat`：

1. 双击；素材可以拖进窗口、粘贴路径，或事先改名成 `installer\bench-clip.mkv`（有它就不再问）。
2. 问分辨率：`1080p` / `2k` / `4k`，可以写多个（空格或逗号分隔）；**直接回车 = 三个全测**。
3. 问倍数 `multi`（2–6）；**直接回车 = 2**。
4. 每个分辨率先跑 VS 滤镜、再跑原生滤镜，最后打印汇总表并写入
   `installer\mpv-FilterBench-result.tsv`（`tag / WxH / multi / frames / elapsed_s / out_fps / src_fps / note`）。

免交互（批处理/自己脚本里调用）：

```bat
installer\mpv-补帧滤镜跑分.bat "E:\视频\clip.mkv" "1080p 4k" 2
```

## 口径

| 项 | 取值 | 为什么 |
| --- | --- | --- |
| 素材 | 真实视频 + `hwdec=d3d11va` | `vf_playkit` 只吃 D3D11 硬件帧；合成源 `lavfi` 和 `--vo=null` 都会让滤镜静默不执行 |
| 分辨率 | `--vf=d3d11vpp=scale=W/SRCW,<补帧滤镜>` | 换分辨率不必换素材，D3D11 视频处理器在硬件域内缩放，按**宽度**算倍率 |
| 播放 | `--untimed --frames=600` | 不算解码/呈现的时钟限制，只测吞吐 |
| 呈现 | `d3d11-flip=no` + `d3d11-sync-interval=0` + `autofit=960x540` | 默认 flip/vsync 会把结果压到 66fps；小窗口降低呈现开销 |
| 计时 | Lua 在 `playback-restart` 记 `mp.get_time()`，`end-file` 收尾 | NGX 初始化不进分母 |
| 指标 | `out_fps = frames / elapsed`；`src_fps = out_fps / multi` | 前者是呈现吞吐，后者是"等效能吃多少源帧" |

两条路径的命令（倍数靠 `VDLSSG_MULTI` 传给 VS 脚本，避免 `user-data` 里的 `;` 引号地狱）：

```
VS      --vf=d3d11vpp=scale=X,vapoursynth=portable_config/vs/MEMC_DLSSG_NV.vpy
原生    --vf=d3d11vpp=scale=X,playkit=multi=N:log=no:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45
```

## 实测（RTX 3080 10GB，源 1920x816 30fps，frames=600）

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
- Lua 的 `print()` 走 stdout，mpv 日志与 `[vdlssg] convertOutput ...` 噪音走 stderr（脚本里把 stderr 重定向到
  `%TEMP%\mpv-filter-bench\*.log`）。
- `--input=no` 会连 `--term-playing-msg` 一起关掉（探片源宽度用后者），所以探宽只加 `<nul` 防吞标准输入。
- 探宽那次 mpv 会占住重定向的 stdin，交互输入只在真实控制台可靠——批处理请用上面三参数形式。
- 编辑这套脚本：`.bat` 必须存成 **ANSI（GB18030）+ CRLF**（UTF-8 会让 cmd 错行、中文乱码）。

## 落地位置

| 文件 | 作用 |
| --- | --- |
| `mpv-playkit\installer\mpv-补帧滤镜跑分.bat` | 交互入口（取素材、问分辨率/倍数、逐档跑两条路径） |
| `mpv-playkit\installer\mpv-FilterBench.conf` | 公共播放参数（hwdec / flip / untimed / frames / 窗口） |
| `mpv-playkit\installer\mpv_filter_bench.lua` | 计时探针（记时、算 fps、追加 TSV） |

打包：`mpv-playkit\manifests\components.yml` 的 `bench-scripts` 组件（kind=repo-files）把这
三个文件落到包的 `installer/` 下——installer/ 其余上游脚本仍然冻结、不进包，见
`mpv-playkit\docs\DECISIONS.md` 第 32 条。
