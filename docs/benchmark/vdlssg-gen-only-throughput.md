# vdlssg 纯帧生成速度（排除数据传输）

测量日期 2026-11-22。RTX 3080 10GB（adapter LUID `0x1244b`，桌面占用约 50%，取多次运行最好值），
VapourSynth R74，DLSS-G `310.9.16.0` sm86 代理。测试脚本 `work\bench_gen.py` +
`work\gen_bench.vpy`（环境变量 `FG_W/FG_H/FG_MULTI/FG_PROXY` 选择分辨率、倍数与引擎包）。

## 指标定义

插件日志把每生成帧拆成五个字段（`frame_gen.cpp:426-430`）：

- **evaluate** = `d3d12_ctx.cpp:824` 的 `span(1,2)`，GPU timestamp 夹住 `NGX_D3D12_EVALUATE_DLSSG`，
  **不含** D3D12 上传（`span(0,1)`）与输出纹理 copy（`span(2,3)`）。这就是「仅帧生成」的耗时。
- up / rb / cp / gpu / wall = 主机侧上传、回读、GPU copy、整趟 GPU、整个 `generate()` 调用。
- 每对的第一个 slot 多跑一趟 `reset=true` 的 Evaluate，所以：
  multi=2 → 每生成帧摊 2 趟；multi=4 → 4/3 趟；multi=6 → 6/5 趟。

## 主表：`bin\dlss45` 引擎包（支持到 6×）

| 分辨率 | multi | evaluate | 生成帧率（仅引擎） | wall | 端到端生成帧率 | upload / readback / copy |
|---|---|---|---|---|---|---|
| 1080p | 2 | **1.30 ms** | **768 fps** | 12.2 ms | 82 fps | 1.72 / 1.88 / 2.51 ms |
| 1080p | 4 | **0.86 ms** | **1162 fps** | 10.1 ms | 99 fps | 0.67 / 2.21 / 3.39 ms |
| 1080p | 6 | **0.70 ms** | **1436 fps** | 6.8 ms | 147 fps | 0.37 / 1.84 / 2.50 ms |
| 2160p | 2 | **2.63 ms** | **381 fps** | 39.6 ms | 25 fps | 6.52 / 7.95 / 10.06 ms |
| 2160p | 4 | **1.68 ms** | **597 fps** | 26.6 ms | 38 fps | 1.96 / 7.70 / 10.06 ms |
| 2160p | 6 | **1.48 ms** | **674 fps** | 24.2 ms | 41 fps | 1.18 / 8.27 / 10.05 ms |

## 副表：`work\dlssg_sm86` 引擎包（`MultiFrameCountMax=3`，最高 4×）

| 分辨率 | multi | evaluate | 生成帧率（仅引擎） | wall | 端到端生成帧率 |
|---|---|---|---|---|---|
| 1080p | 2 | 1.44 ms | 692 fps | 18.8 ms | 53 fps |
| 1080p | 4 | 0.80 ms | 1250 fps | 8.1 ms | 124 fps |
| 2160p | 2 | 3.42 ms | 292 fps | 57.7 ms | 17 fps |
| 2160p | 4 | 1.67 ms | 598 fps | 26.3 ms | 38 fps |

两个包在 multi=4 上一致（1.67 vs 1.68 ms），multi=2 的差异（3.42 vs 2.63 ms）来自引擎包
配置而非插件：`work\dlssg_sm86` 的 ini 在 2× 下明显更慢。multi=6 只有 `bin\dlss45` 能跑，
用 `work` 包会报 `vdlssg: multiFrameCount 5 exceeds DLSSG.MultiFrameCountMax 3`。

## 结论

- **单趟 Evaluate** 从两档倍数反推一致：**2160p ≈ 1.25–1.32 ms，1080p ≈ 0.58–0.65 ms**。
  即引擎可以 2160p 每秒生成 600–670 帧、1080p 每秒 1400 帧以上。
- **倍数越高，每生成帧越便宜**：reset pass 与颜色/运动纹理上传摊到更多帧。2160p 从 2× 的
  2.63 ms 降到 6× 的 1.48 ms（−44%），1080p 从 1.30 降到 0.70 ms（−46%）。
- **端到端仍由传输主导**：2160p multi=6 的 wall 24.2 ms 里 evaluate 只占 6%，输出纹理
  copy 10.05 ms + 回读 8.27 ms 才是大头；multi=2 时还有 6.5 ms 上传。要提墙钟只能压
  回读/copy（GPU 转 YUV 只回读 8.3 MB，或 GPU 驻留输出），压引擎没有意义。
- 与 `docs\vdlssg-throughput.md`（2026-11-16 记录 3.2 ms/2160p 生成帧）量级一致。
- 实测抖动：桌面占用 GPU 时单次 wall 可差 ±20%，evaluate 较稳（±5% 以内）。
