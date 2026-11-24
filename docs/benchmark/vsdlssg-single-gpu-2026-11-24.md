# vs-dlssg 单卡（x16 槽 RTX 3080）测速 — 2026-11-24

## 环境

| 项 | 值 |
| --- | --- |
| GPU | 单卡 NVIDIA GeForce RTX 3080 10GB（x16 槽卡，display_active Disabled，DXGI 0，LUID 0x11a58） |
| 包 | `mpv-playkit/dist/stage/mpv-playkit-pre-2026.11.20-e84b3d4-windows-x64-vsdlssg`（2026.11.20-e84b3d4，vs-framegen 0.2.1，dlss45 4.5） |
| 引擎 | `vs-plugins/vdlssg.dll`（216064 B）+ `vs-plugins/dlss45`，preset a |
| 源 | `work/bench_analysis/ah_1080p.mkv`（HEVC 1920×816 30fps）、`work/bench_analysis/ah_4k.mkv`（hardlink → `测试数据/阿哈角色PV 4k.mkv`，HEVC 3840×1632 30fps） |

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

## 二、原生滤镜路径（`vf=playkit`，mpv 端到端）

包已经是"零拷贝"版本，本次实测**不再出现** `OpenSharedHandle (input) failed (0x80070057)`
与 `duplicating frames`：

```
[playkit] playkit: D3D11 adapter LUID=0000000000011a58 name=NVIDIA GeForce RTX 3080
[playkit] playkit: vdlssg bridge ready 1920x816 nv12 bits=8 multi=2
```

命令（`work/bench_npk/run_mpv.ps1` 封装，`--frames=N` 计的是**输入帧**，已用 3000 帧跑验证）：

```
mpv.exe --no-config --vo=gpu-next --gpu-api=d3d11 --hwdec=d3d11va --no-audio --untimed \
  --frames=N --vf=playkit:multi=M:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45 \
  --script=work/bench_npk/bench.lua --script-opts=bench-file=<log> <src>
```

### 1920×816

| multi | 源 fps | 输出 fps | 每源帧 |
| --- | --- | --- | --- |
| 无滤镜 | 181 | 181 | 5.53 ms |
| 2 | 73.9 | 147.8 | 13.5 ms |
| 4 | 80.5 | 321.9 | 12.4 ms |
| 6 | 82.1 | 492.7 | 12.2 ms |

### 3840×1632

| multi | 源 fps | 输出 fps | 每源帧 |
| --- | --- | --- | --- |
| 2 | 54.7 | 109.5 | 18.3 ms |
| 4 | 60.5 | 241.8 | 16.5 ms |
| 6 | 55.4 | 332.2 | 18.0 ms |

对比 VS 路径：1080p 高端倍率两者接近（原生 458-493 vs VS 422 输出 fps），
4K 时原生路径明显更快（m4 242 vs 136，m6 332 vs 156）——VS 路径的 CPU 传输在 4K 成为瓶颈，
而原生零拷贝不吃这份开销，与历史 `docs/benchmark/vdlssg-gen-only-throughput.md`
"端到端由传输主导"的结论一致。

注意：mpv 1080p m2 异常偏低（13.5 ms/源帧，反而比 m4/m6 更慢），单卡独显无显示输出时
VO 侧存在额外同步；该点建议后续用无 VO 的原生基准复核。

## 三、踩到的坑（复测必看）

1. **残留 mpv 进程会锁死 DLSS-G preset**：
   `vapoursynth.Error: vdlssg: timed out waiting for another process to pin its DLSS-G preset`。
   之前挂死的 `mpv.exe`（PID 35860）没退，杀掉后立刻正常。复测前先 `Get-Process mpv`。
2. **包内 `portable_config` 在无终端/重定向场景会卡在片尾**：`save_props_auto` 自动档在
   `--frames` 结束处把 mpv 打成 `(Paused)` 后不退出。基准一律加 `--no-config` 并显式给全部参数。
3. **`--vo=null` 会跳过原生滤镜**（`VO: [null] 1920x816 yuv420p`，没有 `bridge ready`），
   必须 `--vo=gpu-next`。
4. **`~~/vs/...` 不被 `vf=vapoursynth` 展开**；绝对路径的 `:` 会被 vf 子选项解析打断，
   实际可用写法是相对包目录的 `portable_config/vs/MEMC_DLSSG_NV.vpy`。
5. **中文路径经 `Start-Process -ArgumentList` 会乱码**（被按 ANSI 传出），改用 ASCII hardlink
   `work/bench_analysis/ah_4k.mkv`。
6. `.ps1` 无 BOM 时 Windows PowerShell 按 ANSI 读，文件内中文注释会把 `param(` 吃掉导致
   `Unexpected token ')'` —— 脚本一律只写 ASCII。

## 相关文件

- `work/bench_npk/vs_bench.py` — VS 路径吞吐基准（`--multi/--src/--frames/--nofilter`）
- `work/bench_npk/run_mpv.ps1` — mpv 运行器（超时强杀、日志尾、`-EnvPairs` 传环境变量）
- `work/bench_npk/bench.lua` — 记录 vo-configured 起点与退出时刻
- 原始日志：`work/bench_npk/*.log`、`*.out`、`*.err`
