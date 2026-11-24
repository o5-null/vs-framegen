# vs-dlssg 单卡（x16 槽 RTX 3080）测速 — 2026-11-24

## 环境

| 项 | 值 |
| --- | --- |
| GPU | 单卡 NVIDIA GeForce RTX 3080 10GB（x16 槽卡，display_active Disabled，DXGI 0，LUID 0x11a58） |
| 包 | `mpv-playkit/dist/stage/mpv-playkit-pre-2026.11.20-e84b3d4-windows-x64-vsdlssg`（2026.11.20-e84b3d4，vs-framegen 0.2.1，dlss45 4.5） |
| 引擎 | `vs-plugins/vdlssg.dll`（216064 B）+ `vs-plugins/dlss45`，preset a |
| 源 | `work/bench_analysis/ah_720p.mkv`(1280×544)、`ah_1080p.mkv`(1920×816)、`ah_2k.mkv`(2560×1088)、`ah_4k.mkv`(3840×1632，hardlink → `测试数据/阿哈角色PV 4k.mkv`)，均 HEVC 30fps |

## 一、VS 滤镜路径（无 VO，权威吞吐）

脚本 `work/bench_npk/vs_bench.py`：用 `.venv` 的 VapourSynth + ffms2 读源当 `video_in`，
exec 包内 `portable_config/vs/MEMC_DLSSG_NV.vpy`，跳过 3 帧预热后连续 `get_frame()` 计时。
多倍率走环境变量 `VDLSSG_MULTI`。**这条路径没有播放器/VO 干扰，是可比的纯滤镜吞吐。**

### 1920×816（30fps 源）

| multi | 输出 ms/帧 | 输出 fps | 源 fps | 相对无滤镜（1.090 ms/源帧，917 输出 fps） |
| --- | --- | --- | --- | --- |
| 2 | 3.683 | 271.5 | 135.8 | 每源帧 7.37 ms（含解码） |
| 3 | 3.246 | 308.1 | 102.7 | 每源帧 9.74 ms |
| 4 | 2.849 | 351.1 | 87.8 | 每源帧 11.40 ms |
| 6 | 2.367 | 422.5 | 70.4 | 每源帧 14.20 ms |

### 3840×1632（30fps 源）

| multi | 输出 ms/帧 | 输出 fps | 源 fps | 相对无滤镜（2.346 ms/源帧，426 输出 fps） |
| --- | --- | --- | --- | --- |
| 2 | 10.026 | 99.7 | 49.9 | 每源帧 20.05 ms |
| 4 | 7.372 | 135.7 | 33.9 | 每源帧 29.49 ms |
| 6 | 6.405 | 156.1 | 26.0 | 每源帧 38.43 ms |

结论：VS 路径被 CPU↔GPU 上传/回读主导。4K 每源帧成本是 1080p 的 2.7 倍，
且 multi 越高每源帧固定开销越摊薄（每生成帧边际成本 1080p ≈ 1.7-2.0 ms）。

## 二、原生滤镜路径（`vf=playkit`，mpv 端到端，含 VO 开销）

包已经是"零拷贝"版本，本次实测**不再出现** `OpenSharedHandle (input) failed (0x80070057)`
与 `duplicating frames`：

```
[playkit] playkit: D3D11 adapter LUID=0000000000011a58 name=NVIDIA GeForce RTX 3080
[playkit] playkit: vdlssg bridge ready 1920x816 nv12 bits=8 multi=2
```

测量方法（`work/bench_npk/matrix.ps1`）：固定播放时长而非帧数——`--frames=N` 计的是
**输出帧**（VO 交付帧），不是输入帧，早前按输入帧换算的表格作废。现在每个配置跑
`--length=30` 与 `--length=60` 两段，取时间差求稳态速率（消除 ~1s 启动开销）：

```
mpv.exe --no-config --vo=gpu-next --gpu-api=d3d11 --hwdec=d3d11va --no-audio --untimed \
  --length=N --vf=playkit:multi=M:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45 <src>
```

### 实际显示吞吐（含 gpu-next VO）

| 分辨率 | 无滤镜 | 2× | 4× | 6× |
| --- | --- | --- | --- | --- |
| 1280×544 | 180.8 fps / 5.53 ms | 84.4 fps / 11.85 ms | 98.4 fps / 10.16 ms | 94.0 fps / 10.64 ms |
| 1920×816 | 176.7 fps / 5.66 ms | 74.4 fps / 13.44 ms | 96.7 fps / 10.34 ms | 88.0 fps / 11.36 ms |
| 2560×1088 | 177.5 fps / 5.63 ms | 68.8 fps / 14.53 ms | 92.2 fps / 10.85 ms | 84.7 fps / 11.80 ms |
| 3840×1632 | 182.8 fps / 5.47 ms | 60.6 fps / 16.50 ms | 80.8 fps / 12.38 ms | 75.7 fps / 13.20 ms |

（单元格：输出 fps / 每输出帧毫秒）

换算成"能喂多少源帧"（每源帧 = 每输出帧 × multi）：

| 分辨率 | 2× | 4× | 6× |
| --- | --- | --- | --- |
| 1280×544 | 23.7 ms → 42.2 源 fps | 40.6 ms → 24.6 | 63.8 ms → 15.7 |
| 1920×816 | 26.9 ms → 37.2 源 fps | 41.4 ms → 24.1 | 68.2 ms → 14.7 |
| 2560×1088 | 29.1 ms → 34.4 源 fps | 43.4 ms → 23.1 | 70.8 ms → 14.1 |
| 3840×1632 | 33.0 ms → 30.3 源 fps | 49.5 ms → 20.2 | 79.2 ms → 12.6 |

即：4K 6× 只能吃 12.6 源 fps；4K 2× 恰好卡在 30fps 源的实时线上（30.3）。

### 开销分解（`PLAYKIT_TIMING=1` 逐帧计时，`--length=10`）

滤镜内层（`vf_playkit.c:612-616` 打印）每源帧一次，分摊到每输出帧：

| 分辨率 | 2× copy_wait+process | 4× | 6× |
| --- | --- | --- | --- |
| 1920×816 | 1.45+12.54 → 7.00 ms | 1.93+24.07 → 6.48 ms | 2.14+39.01 → 6.86 ms |
| 3840×1632 | 1.35+17.40 → 9.37 ms | 2.46+32.19 → 8.66 ms | 2.82+48.40 → 8.54 ms |

每生成一帧的边际成本（对 process 线性拟合）：1080p ≈ 基线 6.3 ms + **每生成帧 6.4 ms**；
4K ≈ 9.7 ms + **每生成帧 7.7 ms**。`bridge wall` 与 `process` 基本相等（1080p 6×：37.53 vs 39.01 ms），
`eval`/`gpu` 字段恒为 0（0.2.1 的 bridge 没填 stats）。

端到端减去滤镜后剩下的（VO + core）：1080p 2× 6.44、4× 3.86、6× 4.50 ms/输出帧；
4K 2× 7.13、4× 3.72、6× 4.66 ms/输出帧。基线（无滤镜）VO 为 5.5-5.7 ms/输出帧，
量级一致——**每输出帧有 ~4-6 ms 的 VO 固定开销，且不随分辨率变化**。

### 根因：每源帧一次 VO 重配

统计 mpv 输出行（`work/bench_npk/count_lines.ps1`）：

| 日志 | VO 行 | 其中 `=>` 重配 | 输出帧 |
| --- | --- | --- | --- |
| `mx_1080p_m0_30.out`（无滤镜） | 1 | 0 | — |
| `timing_m2.out`（1080p 2×） | 600 | 300 | 600 |
| `timing_m6b.out`（1080p 6×） | 600 | 300 | 1800 |

原生滤镜路径**每个源帧重建一次 VO**（`VO: [gpu-next] WxH => WxH d3d11[nv12]`），基线完全不重配。
按重配帧占比反解单次代价：1080p 每次 ≈10 ms、4K ≈13.6 ms（2× 有 50% 输出帧在重配、
4× 25%、6× 16.7%），这正好解释了「2× 比 4× 还慢」的反常现象。
`dbg_reconf`（`--msg-level=all=v`）尾部可见 `[vo/gpu-next/libplacebo] Spent 11.369 ms translating HLSL to DXBC`，
说明重配会重新编译/翻译渲染管线，代价在毫秒级。

## 三、结论

1. **实际显示路径 1080p 2× 只有 74 输出 fps**，6× 88 fps；4K 2× 60.6，6× 75.7。
   分辨率 720p→4K 只掉约 20%，说明瓶颈是固定开销而非像素吞吐。
2. **两个固定开销**：① 每源帧一次 VO 重配（10-14 ms/次，`vf_playkit` 输出帧参数不一致导致）；
   ② bridge 每生成帧 6.4-7.7 ms（`pk_wait` 的 `Sleep(0)` 忙等 + 每源帧 `Flush` + D3D12 侧同步）。
   两者之和已远超 DLSS-G 本身 ~1 ms/帧的生成成本。
3. **对比 VS 路径**：1080p 原生端到端（74-98 fps）**明显慢于** VS 路径（271-422 fps），
   因为 VS 路径没有 VO 重配；4K 时两侧接近（原生 75-81 vs VS 100-156），VS 路径的 CPU 传输
   在 4K 变成瓶颈，但原生路径浪费在重配上的时间把零拷贝的优势抵掉了。
4. 若要收掉重配：`vf_playkit` 应保证生成帧的 `mp_image_params` 与源帧完全一致（当前疑似
   逐帧属性不同/未复用输出 params），这是下一个值得修的点。

## 四、修复：生成帧 params 与源帧一致（已提交）

`vf_playkit.c` 里生成帧来自 `av_hwframe_get_buffer()` 的 hw frame pool，
`mp_image_from_av_frame()` 带的色彩元数据是未设置的默认值：

```
gen=[1920x816 [0:1] d3d11[nv12] auto/auto/auto/auto/auto CL=unknown ...]
src=[1920x816 d3d11[nv12] bt.709/bt.709/bt.1886/limited/display CL=mpeg2/4/h264 ...]
```

mpv 每帧比较 `mp_image_params`，不一致就重建 VO 渲染管线——这就是上面统计到的
"每源帧一次重配"。修法是在 `process()` 里把生成帧的参数覆盖成源帧的：

```c
img->params = cur->params;
```

（`mpv-src` commit `aa03ade`；`PLAYKIT_TIMING=1` 时会打一行 `playkit: params gen=.. src=..` 便于核对。
复测的 `fix_m6.out` 中 VO 行回到 1 条、重配 0 次，与无滤镜基线一致。）

### 修复后矩阵（同样方法，`work/bench_npk/matrix_fix.ps1`）

| 分辨率 | 无滤镜 | 2× | 4× | 6× |
| --- | --- | --- | --- | --- |
| 1280×544 | 175.7 fps | 147.1 fps / 6.80 ms | 132.2 fps / 7.57 ms | 113.0 fps / 8.85 ms |
| 1920×816 | 176.6 fps | 123.1 fps / 8.13 ms | 116.0 fps / 8.62 ms | 100.3 fps / 9.97 ms |
| 2560×1088 | 183.6 fps | 124.2 fps / 8.05 ms | 143.1 fps / 6.99 ms | 110.0 fps / 9.09 ms |
| 3840×1632 | 180.1 fps | 105.9 fps / 9.44 ms | 108.6 fps / 9.21 ms | 103.2 fps / 9.69 ms |

提升幅度（同一配置 2×/4×/6×）：

| 分辨率 | 2× | 4× | 6× |
| --- | --- | --- | --- |
| 1280×544 | 84.4 → 147.1（+74%） | 98.4 → 132.2（+34%） | 94.0 → 113.0（+20%） |
| 1920×816 | 74.4 → 123.1（+65%） | 96.7 → 116.0（+20%） | 88.0 → 100.3（+14%） |
| 2560×1088 | 68.8 → 124.2（+81%） | 92.2 → 143.1（+55%） | 84.7 → 110.0（+30%） |
| 3840×1632 | 60.6 → 105.9（+75%） | 80.8 → 108.6（+34%） | 75.7 → 103.2（+36%） |

修复后的源帧容量：1080p 2× 61.5 源 fps、6× 16.7；4K 2× 53.0、6× 17.2 源 fps。
"2× 比 4× 慢"的反常消失。

剩下的成本仍主要在内层：`PLAYKIT_TIMING` 显示 1080p 6× 的 `copy_wait+process`
仍是 ~48 ms/源帧（≈8 ms/生成帧），而 VO 残留已降到 ~2.5 ms/输出帧。
也就是说瓶颈已回到 `vs-dlssg` 的 bridge：`generateTexturesMulti()`
（`vs-dlssg/src/ngx_session.cpp:1856-1946`）对每个输出索引单独跑一次
`runPass()`，每次 `runPass()` 内部 `gpu->begin()` + `endAndWait()`
（`vs-dlssg/src/d3d12_ctx.cpp:1124-1154`，`ExecuteCommandLists` + fence 等待）——
m6 就是每源帧 8 次"提交并等待"（NVOF 1 次 + 转换 2 次 + 重置 1 次 + 5 个索引）。
把这些 pass 合并进同一条 command list、只在最后等一次 fence，是下一个该做的优化。

## 五、踩到的坑（复测必看）

1. **残留 mpv 进程会锁死 DLSS-G preset**：
   `vapoursynth.Error: vdlssg: timed out waiting for another process to pin its DLSS-G preset`。
   之前挂死的 `mpv.exe`（PID 35860）没退，杀掉后立刻正常。复测前先 `Get-Process mpv`。
2. **包内 `portable_config` 在无终端/重定向场景会卡在片尾**：`save_props_auto` 自动档在
   结束处把 mpv 打成 `(Paused)` 后不退出。基准一律加 `--no-config` 并显式给全部参数。
3. **`--vo=null` 会跳过原生滤镜**（`VO: [null] 1920x816 yuv420p`，没有 `bridge ready`），
   必须 `--vo=gpu-next`。
4. **`~~/vs/...` 不被 `vf=vapoursynth` 展开**；绝对路径的 `:` 会被 vf 子选项解析打断，
   实际可用写法是相对包目录的 `portable_config/vs/MEMC_DLSSG_NV.vpy`。
5. **中文路径经 `Start-Process -ArgumentList` 会乱码**（被按 ANSI 传出），改用 ASCII hardlink
   `work/bench_analysis/ah_4k.mkv`。
6. `.ps1` 无 BOM 时 Windows PowerShell 按 ANSI 读，文件内中文注释会把 `param(` 吃掉导致
   `Unexpected token ')'` —— 脚本一律只写 ASCII。
7. **`--frames=N` 是输出帧数**，不是输入帧；要测吞吐用 `--length` + 时间差分。
8. **`log=yes` 会触发逐帧 staging 回读**（`pk_hash_texture`），1080p 6× 10s 源从 ~22s 拖到 38.7s；
   计时只用 `PLAYKIT_TIMING=1`，别开 `log=yes`。

## 相关文件

- `work/bench_npk/vs_bench.py` — VS 路径吞吐基准（`--multi/--src/--frames/--nofilter`）
- `work/bench_npk/matrix.ps1` — 4 分辨率 × 4 倍率 × 2 时长的端到端矩阵
- `work/bench_npk/timing.ps1` — `PLAYKIT_TIMING` 计时批跑
- `work/bench_npk/avg_timing.ps1` — 平均 `playkit: timing` 行
- `work/bench_npk/count_lines.ps1` — 统计 VO 行/重配次数
- `work/bench_npk/run_mpv.ps1` — mpv 运行器（超时强杀、日志尾、`-EnvPairs` 传环境变量）
- `work/bench_npk/bench.lua` — 记录 vo-configured 起点与退出时刻
- 原始日志：`work/bench_npk/*.log`、`*.out`、`*.err`
