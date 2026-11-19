# 八引擎帧生成基准（吞吐 + 质量）

> 图表、11 档完整数据表与 4K 生成帧 2×2 对比见 **[`benchmark.md`](benchmark.md)**；
> 本文是文字版小结。

完整报告、原始数据、图表与生成帧：`测试数据\engines\report.md`。
脚本、方法文档与结果快照在仓库内 `bench\`（`bench\docs\method.md` 记录口径与坑）。
2026-11-19，RTX 3080 10GB (sm_86)，1080p 动漫素材 40 帧，2 倍速。

## 一句话结论

DLSSG 在 1080p 及以下最快（1080p 8.00 ms/pair）但最软（锐度 0.61× 真实帧）；VFG 的耗时几乎与分辨率无关（18.4→22.6 ms），4K 反超 DLSSG 并拿第一（88 fps 输出）；质量由 RIFE 系列领跑（`rife4.26heavy` VMAF 63.19，19 帧赢 13 帧，`rife4.6` 62.37 且最快）；FSR4 与 XeSS-FG 在 3080 上又慢又差（4K ≈143 ms/pair、VMAF 56.2/59.6），不可用。

## 吞吐（ms/pair，2 倍速，越低越好；5 遍取最小）

| 引擎 | 720p | 1080p | 1440p | 4K |
|---|---|---|---|---|
| dlssg | **4.15** | **8.00** | **13.63** | 29.38 |
| vfg | 18.36 | 19.77 | 20.82 | **22.63** |
| xess | 17.68 | 37.63 | 64.38 | 142.13 |
| fsr4 | 17.99 | 38.19 | 65.50 | 143.98 |
| rife4.6 | 7.19 | 16.20 | 25.49 | 69.97 |
| rife4.25lite | 11.79 | 31.42 | 38.93 | 75.58 |
| rife4.26 | 14.55 | 37.78 | 37.99 | 83.63 |
| rife4.26heavy | 14.14 | 37.34 | 49.94 | 105.92 |

11 档分辨率阶梯（480×270 → 4K）与「每百万像素耗时」见报告与 `charts\time_per_pixel.png`：
DLSSG 13.4→3.5 ms/Mpx（固定开销+按像素），VFG 42.8→2.7（固定成本主导，4K 每像素最便宜），XeSS/FSR4 从 720p 起稳定在 17–19（纯按像素），RIFE 各变体接近水平。

## 质量（1080p，19 个真实中间帧）

| 引擎 | VMAF | PSNR-Y | SSIM | 锐度/GT | 赢帧 |
|---|---|---|---|---|---|
| rife4.26heavy | 63.19 | 32.16 | 0.9684 | 0.859 | 13 |
| rife4.26 | 62.79 | 32.07 | 0.9679 | 0.858 | 1 |
| rife4.6 | 62.37 | 32.11 | 0.9681 | 0.821 | 0 |
| rife4.25lite | 61.86 | 31.91 | 0.9665 | 0.864 | 2 |
| vfg | 61.43 | 32.13 | 0.9686 | 0.653 | 3 |
| xess | 59.61 | 31.64 | 0.9650 | 0.867 | 0 |
| dlssg | 58.11 | 31.63 | 0.9656 | 0.612 | 0 |
| fsr4 | 56.15 | 31.31 | 0.9642 | 1.029 | 0 |

## 口径与坑

- 四个硬件后端由 `参考插件\SmoothMyVideo-protable\resources\engine\live\smv-live.exe --offline` 驱动，取宿主 stderr 里 `X ms per pair`（不含进程启动/模型加载）；stdout 重定向到 NUL，stdin 是预写裸 RGB24 文件。该口径含宿主 I/O（1080p 每帧 6.2 MB，约 1–2 ms）。SMV 侧 **5 遍取最小**。
- RIFE 四变体：TensorRT fp16 **静态** engine（`work\engines\rife_<name>_<W>x<H>_fp16.engine`，每档一次 54–78 s 编译）。动态 profile 实测平均慢 5%（720p +7.8%、1440p +11.6%、4K −2.9%），且 min shape 必须 tile 对齐（`(1,11,360,640)` 被拒：`360 != 384`）。四个模型必须 **交错**跑（一遍覆盖四个），否则先跑的模型占冷启动便宜，排序是假的。
- RIFE 四变体的 ONNX 输入是 **11 通道**：`[A(3), B(3), t(1), gridx(1), gridy(1), 2/(W-1), 2/(H-1)]`，少 4 个 meshgrid/常量 plane 输出全错（mae≈0.24）。tile multiple：4.6=32、4.25lite=128、4.26/heavy=64。
- 质量测试必须喂 **20 张奇数帧**才得到 19 个 tween（喂 19 张只出 18 个，会少一格）。
- `vfg` 必须 `cwd=engine`；`SMV_DLSSG_LIMIT` 环境变量要清掉。VFG 启动偶发 `exit 3221225725`（0xC00000FD 栈溢出，卡在 `runtime folder`），脚本重试补齐，不是尺寸限制。
- **engine 缓存文件可丢弃**：`rife4.26heavy` 的 4K engine 曾病态到 6240 ms/pair（正常 106），删除重建即恢复。TRT tactic 选择在某些构建窗口下会落到极差解。
- 长跑噪声大：同档位跨轮差异 5–40%（`rife4.26` 1080p 曾读到 25.21 与 37.78），引擎间 20% 以内的差不宜下结论，结论只用趋势与量级。

## 复现

脚本在 `bench\src\`（口径与坑见 `bench\docs\method.md`）：

```powershell
E:\code\VFI\.venv\Scripts\python.exe bench\src\run_throughput.py   # 11 档 × 8 引擎
E:\code\VFI\.venv\Scripts\python.exe bench\src\run_quality.py
E:\code\VFI\.venv\Scripts\python.exe bench\src\metrics.py
E:\code\VFI\.venv\Scripts\python.exe bench\src\make_charts.py
E:\code\VFI\.venv\Scripts\python.exe bench\src\make_compare.py
E:\code\VFI\.venv\Scripts\python.exe bench\src\probes\probe_dynamic_engine.py
```
