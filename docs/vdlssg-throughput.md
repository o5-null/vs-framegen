# vdlssg 吞吐实测（2160p / 1080p）

测量日期 2026-11-16，RTX 3080 10GB（桌面有占用，计时取三次最好值），VapourSynth R74，
脚本 `test\perf4k.py`、`test\perf4k_base.py`、`test\perfstats.py`、`test\bench.py`。

## 结论

2160p 2× 插帧从 **9.3 fps 输出**（首版）提升到 **55.2 fps 输出**（`bench.py` 三次最好值），
即每秒 27.6 个源帧，**已超过** 24 fps 源素材实时 2× 所需的 48 fps 输出。2160p 下
DLSS-G 本身只占 **3.2 ms/生成帧**。1080p 为 **140 fps 输出**。

## 数据

| 场景 | 输出帧 | 生成帧 | 总耗时 | 输出 fps | evaluate 均值 |
|---|---|---|---|---|---|
| 2160p 基线（无插件，仅升采样+落盘） | 61 | 0 | 0.97 s | 62.98 | — |
| 2160p 首版 | 121 | 60 | 12.98 s | 9.32 | 8.39 ms |
| 2160p 多线程转换 | 121 | 60 | 5.85 s | 20.68 | 10.45 ms |
| 2160p +跳过常量拷贝 | 121 | 60 | 2.84 s | 42.63 | 3.85 ms |
| 2160p +复用上一对颜色纹理 | 121 | 60 | **2.19 s** | **55.15** | **3.32 ms** |
| 1080p 首版 | 41 | 20 | 1.27 s | 32.39 | 1.30 ms |
| 1080p 最终 | 41 | 20 | **0.29 s** | **139.98** | 1.58 ms |

2160p 的 55.15 fps 为 `test\bench.py` 三次运行的最好值（另两次 55.08 / 51.76），桌面占用会让单次结果有 ±5% 抖动。
4K 每生成帧摊到 (2.19 − 0.97) / 60 ≈ **20 ms** 墙钟，其中 GPU 3.32 ms，其余是一次 color 上传 +
一次回读（各约 50 MB half 数据）与每帧一次 fence 同步。

## 四处改动

1. **CPU 转换多线程**（`vs-dlssg\src\ngx_session.cpp`）：`float32 → half`（上传）与
   `half → float32`（回读）原本是标量单线程，有效带宽约 1 GB/s，2160p 一帧要搬约
   400 MB。新增 `parallelRows(height, fn)`，按行分带到 `hardware_concurrency()`（上限 16）。
   收益 12.98 s → 5.85 s。
2. **跳过常量输入拷贝**（`vs-dlssg\src\d3d12_ctx.cpp::recordPreEvaluate`）：深度恒 0.5、
   运动矢量恒 0，这两个纹理只在首趟 `fillConstantInputs` 之后不再变化，所以
   `copyUploadToTexture(depth/motion)` 只在首趟执行（`constantInputsCopied_`），
   2160p 每帧省下 66 MB 的 `CopyTextureRegion`。收益 5.85 s → 2.84 s。
3. `build.bat` 加 `/arch:AVX2`（F16C 内建在 MSVC 上不可用，见下）。
4. **复用上一对留下的颜色纹理**（`vs-dlssg\src\ngx_session.cpp::generate` + `d3d12_ctx.cpp`
   的 `recordPreEvaluate(bool copyColor)`）：第二趟结束时 `color_` 里正好是下一对的
   `prev`（源帧 k+1），所以顺序取帧时第一趟完全不用上传，2160p 每生成帧再省
   74 MB。靠 `lastPairIndex`/`lastPairValid` 判定，乱序取帧自动退回上传，仍然正确。
   收益 2.84 s → 2.19 s。⚠️ 首版用 `lastPairIndex = -1` 初值让 `pairIndex == 0`
   误命中复用，`parity2.py` 的 t00 立刻从 0.000000 跳到 0.38 —— 这个不变量必须靠
   显式 valid 标志，不能靠哨兵值。
   ⚠️ 第二个坑更隐蔽：session 是**进程级单例**，而缓存键只有 `pairIndex`（= `srcA`）。
   同一进程里建第二个 FrameGen 过滤器时它的第一对同样是 `pairIndex == 0`，于是
   命中上一个 clip 留下的 `pairSeeded`/`lastPairIndex`，跳过 reset 与上传，
   直接拿旧历史求值 —— 输出是一张与前一个 clip 相关的陈旧画面（RGBS 均值恒 0.488）。
   `test\diag_fp.py` 连跑 t01/t05/t07 时 t05 与 t07 的 slot1 md5 完全相同（`1b5ba7abf3df`）、
   `slot1-vs-gt` 0.15 以上，单跑则各自正常。修法是给每个过滤器实例一个 `streamId`
   （`NgxSession::newStream()`，`generate()` 第三个参数），代次变化时把
   `lastPairValid`/`pairSeeded`/`lastPairIndex`/`seededPairIndex` 全部复位。

## 正确性回归

`media\parity2.py`（10 组三帧，对比 `work\spike_tNN.ppm` 与真中间帧 `tNN_gt.rgb`）：

- 透传帧 vs `tNN_a.rgb`：MAE 0.000000（10/10）
- 插件输出 vs spike 输出：MAE **0.000000**（10/10，逐位相同）
- 插件输出 vs 真中间帧：均值 MAE **0.004109**，与优化前完全一致

即四处改动只影响速度，不影响像素。第 4 项改动一度让 t00 偏移（`gen-vs-spike` 0.379935），修好 `lastPairValid` 后十组恢复逐位相同。

第二个回归入口是**同进程多 clip**（`test\diag_fp.py t01 t05 t07`）：三组的 slot1 md5 必须与各自
单跑时一致（`1c68edd7ce55` / `a24b13cd3914` / `8790d6397f70`）。这一条专门守 `streamId`
代次隔离 —— 只要有人再把缓存键简化成裸 `pairIndex`，它立刻会退化成两两相同。

## 已知坑

- `std::min` 在包含 `windows.h` 的编译单元里会被 `min` 宏破坏（`error C2589`），
  `parallelRows` 内改用三元表达式。
- MSVC 仅 `/arch:AVX2` 时不声明 `_cvtss_sh`/`_cvtsh_ss`（`error C3861`），
  F16C 路径已回退为软件实现；若日后要再提速，需要显式 `#include <intrin.h>` 并
  确认 `__AVX2__` 已定义，或改用 AVX512 路径。
- 每生成帧仍有一次 `WaitForSingleObject(INFINITE)`，两趟 Evaluate 串行；要再压时间
  必须做上传/回读流水线（双 readback、上一帧的 fence 与本帧上传重叠）。
