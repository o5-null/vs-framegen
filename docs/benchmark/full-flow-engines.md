# 帧生成全流程吞吐：四引擎 × 两卡

测量日期 2026-11-22。RTX 3080 10GB ×2（同一台机器，两个 PCIe 槽位），VapourSynth R74 /
API R4.2，源 `media\sintel_trailer_1080p.mp4` 的第 600 帧起 32 对真实帧（1080p 原生；2160p
经 Bicubic 放大到 3840×2160）。每次取 3 轮里的最好值。

全套数据在桌面程序全部关闭的空载状态下测（同批 baseline：1080p 4.28 / 4.25 ms 每源帧，
2160p 16.38 / 16.29 ms）。有后台负载时整体会慢 20% 以上，跑之前先清空桌面。

配套的纯生成测量（只测 GPU evaluate，不含传输）见 [vdlssg-gen-only-throughput.md](vdlssg-gen-only-throughput.md)。

## 口径

**全流程** = 调用方每个输出帧实际付出的墙钟时间，主机侧夹在 `get_frame()` 外面：

```
源解码 + 缩放（ffms2 + Bicubic → RGBS）→ 颜色/运动纹理上传 → 引擎生成 → 回读 → VS 输出帧
```

只把 warm-up 帧排除在计时外（`3*multi+4` 帧），其余全部计入。源侧单独测一个不加载引擎
DLL 的 baseline，按倍数分摊后从全流程里扣掉，得到「引擎净耗时」。

各引擎自报的 `evaluate` 字段（`frame_gen.cpp:426-430` 的日志行首字段）口径不同，不能直接横比：

| 引擎 | evaluate 的含义 |
|---|---|
| DLSS-G / XeSS-FG | NGX/GPU timestamp 夹住的纯推理段，不含上传与回读（`d3d12_ctx.cpp:824`） |
| VFG / FSR4 | 插件内部整个生成段，**已包含**它自己的拷贝，只有这一个字段（wall/up/rb 恒为 0） |

两个 NGX 引擎的日志一次调用报一批（multi-1 趟），摊到每个输出帧要除以 multi；VFG/FSR4 的
lane 在单卡上串行，每对的开销约等于 `evaluate × (multi-1)`。

## 两张卡

| 槽位 | PCIe | nvidia-smi | DXGI | 桌面 | BAR1 | 备注 |
|---|---|---|---|---|---|---|
| x4 | gen4 ×4 | GPU 0 | 1 | 无 | 16384 MiB（ReBAR 开） | sm86 代理只给这张卡打补丁 |
| x16 | gen4 ×16 | GPU 1 | 0 | 有 | 256 MiB（ReBAR 关） | 桌面卡，测试时关闭全部程序 |

DXGI 序号与 nvidia-smi/CUDA 相反，插件 `gpu=` 参数用的是 DXGI 序号。**DLSS-G 与 XeSS-FG
只能跑 x4 槽**：代理只钩一张卡的 CUDA 桩，指定另一张会报
`CreateDlssgFeature failed (NGX 0xbad00002): the sm86 proxy patches one card only`（见
`docs/vdlssg-adapter.md`）。VFG 与 FSR4 两张卡都能跑。

## 主表：2× 下四引擎两卡（ms / 每个输出帧）

| 分辨率 | 引擎 | 卡 | 全流程 | 输出 fps | 源侧分摊 | 引擎净耗时 | 自报 evaluate/输出帧 |
|---|---|---|---|---|---|---|---|
| 1080p | DLSS-G | x4 | 7.05 | 142 | 2.14 | 4.91 | 0.65 |
| 1080p | XeSS-FG | x4 | 5.84 | 171 | 2.14 | 3.70 | 0.63 |
| 1080p | VFG | x4 | 13.10 | 76 | 2.14 | 10.96 | 11.70 |
| 1080p | VFG | x16 | 12.72 | 79 | 2.12 | 10.60 | 11.32 |
| 1080p | FSR4 | x4 | 9.78 | 102 | 2.14 | 7.64 | 7.93 |
| 1080p | FSR4 | x16 | 7.79 | 128 | 2.12 | 5.67 | 5.76 |
| 2160p | DLSS-G | x4 | 25.02 | 40 | 8.19 | 16.83 | 1.37 |
| 2160p | XeSS-FG | x4 | 20.30 | 49 | 8.19 | 12.11 | 1.87 |
| 2160p | VFG | x4 | 26.75 | 37 | 8.19 | 18.56 | 20.03 |
| 2160p | VFG | x16 | 23.68 | 42 | 8.15 | 15.53 | 16.94 |
| 2160p | FSR4 | x4 | 33.15 | 30 | 8.19 | 24.96 | 25.99 |
| 2160p | FSR4 | x16 | 25.10 | 40 | 8.15 | 16.95 | 17.62 |

## 倍数阶梯（x4 卡）

| 分辨率 | 引擎 | 2× | 4× | 6× |
|---|---|---|---|---|
| 1080p | DLSS-G | 7.05 / 142 fps | 6.19 / 162 fps | 5.88 / 170 fps |
| 1080p | XeSS-FG | 5.84 / 171 fps | 4.99 / 200 fps | —（上限 4×） |
| 1080p | VFG | 13.10 / 76 fps | 16.69 / 60 fps | 18.01 / 56 fps |
| 1080p | FSR4 | 9.78 / 102 fps | —（上限 2×） | — |
| 2160p | DLSS-G | 25.02 / 40 fps | 22.07 / 45 fps | 21.19 / 47 fps |
| 2160p | XeSS-FG | 20.30 / 49 fps | 17.32 / 58 fps | —（上限 4×） |
| 2160p | VFG | 26.75 / 37 fps | 30.16 / 33 fps | 31.71 / 32 fps |
| 2160p | FSR4 | 33.15 / 30 fps | —（上限 2×） | — |

x16 卡的 VFG：1080p 12.72 / 15.87 / 16.98 ms（m2/m4/m6），2160p 23.68 / 26.67 / 28.08 ms。
两卡规律一致，x16 整体快 3–12%，与其核心时钟高于 x4 卡相符。

> 早先一次测量里 4K VFG 6× 在 x16 卡上出现 107 ms（evaluate 89 ms）的异常，关掉桌面程序后
> 回到 28 ms。那张卡 ReBAR 关闭、BAR1 只有 256 MiB，但重测证明这是后台程序占用 GPU 造成的，
> 与 ReBAR 无关；同时也把当时整批数据拖慢了约 20%（baseline 21.2 → 16.3 ms）。

## 结论

- **DLSS-G 与 XeSS-FG 随倍数变便宜**，与纯生成测量一致：4K 从 2× 25.02 降到 6× 21.19 ms
  （−15%），1080p 7.05 → 5.88 ms（−17%）。省下的是每对都要付一次的颜色/运动上传与调度。
- **VFG 反而变贵**：4K 26.75 → 31.71 ms（+19%），1080p 13.10 → 18.01 ms（+38%）。它的
  multi 是逐对串行推理，lane 并行只在多卡/多 stream 下才摊得开，单卡上没有收益。
- **全流程里纯生成占比极小**：DLSS-G 4K 6× 的 21.19 ms 中 evaluate 只有 0.25 ms（1.2%）；
  1080p 6× 5.88 ms 里 0.12 ms（2.0%）。瓶颈是 4K RGBS 每输出帧约 50 MB 上行 + 50 MB 回读
  的 PCIe 传输（x4 槽有效约 4–5 GB/s，2× 时摊到每输出帧约 15.5 ms）。
- **槽位差异在 FSR4 上最明显**：1080p x16 快 20%（7.79 vs 9.78），4K 快 24%（25.10 vs
  33.15）；VFG 4K 也快 12%，1080p 只快 3%。两个 NGX 引擎受代理限制无法上 x16 比较。
- **引擎排序（4K 全流程，2×）**：XeSS-FG 20.3 ms > VFG x16 23.7 > DLSS-G 25.0 ≈ FSR4 x16
  25.1 > VFG x4 26.8 > FSR4 x4 33.2 ms。1080p：XeSS 5.8 > DLSS 7.1 > FSR4 x16 7.8 >
  FSR4 x4 9.8 > VFG x16 12.7 ≈ VFG x4 13.1 ms。
- 绝对值仍受时钟/温度影响：同一配置跨批次基线可差 ±5%（空载 16.29–16.38 ms），有负载时
  可达 +30%。表内数据是同一批 32 对 / 3 轮的最小值，相对关系稳定。

## 图

| 文件 | 内容 |
|---|---|
| [flow-fps-by-card.png](flow-fps-by-card.png) | 2× 下各引擎两卡的输出 fps |
| [flow-multi-ladder.png](flow-multi-ladder.png) | 每输出帧耗时随倍数变化（x4） |
| [flow-pcie-gap.png](flow-pcie-gap.png) | 同引擎同倍数、两个槽位的对比 |
| [flow-budget.png](flow-budget.png) | 全流程三段构成：源侧 / 引擎 evaluate / 传输调度 |
| [flow-gen-vs-full.png](flow-gen-vs-full.png) | DLSS-G 纯生成 vs 全流程 |

原始数据：[flow.json](flow.json)（每档 3 个样本与 baseline）、[flow.csv](flow.csv)。

## 复现

```powershell
$env:FLOW_RUNS="3"; $env:FLOW_PAIRS="32"
E:\code\VFI\.venv\Scripts\python.exe work\flow_bench.py            # 全矩阵
E:\code\VFI\.venv\Scripts\python.exe work\flow_bench.py --only vfg --res 2160p --gpu x16
E:\code\VFI\.venv\Scripts\python.exe work\make_flow_charts.py      # 出图
```

`work\flow_bench.py` 每档起一个独立进程（插件持有进程级引擎会话，换参数必须重启），
`work\flow_one.py` 负责单次墙钟。两卡同时被占用时数字会整体变慢，跑之前关掉桌面负载。
