# 测量口径与方法

这份文档回答一件事：表里那些毫秒数和分数**到底量了什么**，以及哪些坑已经踩过。
结论与数字见 [`../README.md`](../README.md) 与 [`../results/report.md`](../results/report.md)。

## 1. 输入

`测试数据\1080p\0001.png … 0040.png`：40 张连续帧（1920×1080，动漫场景，含摇镜、
平移与局部形变）。所有分辨率都由这 40 张缩放出 来，保证各档比较的是同一段运动。

- 缩小用 `cv2.INTER_AREA`，放大用 `cv2.INTER_CUBIC`（`src/common.py: load_frame`）。
- 2 倍速插帧：N 帧输入 → 2N−1 帧输出，即 N−1 个生成帧（"对"）。
- 吞吐用满 40 帧（39 对）；质量只用**奇数帧**做邻居，所以偶数帧本身就是真值。

11 档分辨率阶梯（`src/common.py: RESOLUTIONS`）：

| tag | 尺寸 | Mpx | 为什么选它 |
|---|---|---|---|
| w480 | 480×270 | 0.13 | 探测固定开销 |
| w640 | 640×360 | 0.23 | |
| w800 | 800×450 | 0.36 | |
| w960 | 960×540 | 0.52 | |
| 720p | 1280×720 | 0.92 | 常用档 |
| w1600 | 1600×900 | 1.44 | |
| 1080p | 1920×1080 | 2.07 | 质量测试档 |
| w2240 | 2240×1260 | 2.82 | |
| 1440p | 2560×1440 | 3.69 | 常用档 |
| w3200 | 3200×1800 | 5.76 | |
| 4k | 3840×2160 | 8.29 | 上限 |

阶梯取的是「短边 270/360/450/540/720/900/1080/1260/1440/1800/2160，16:9」，
所以横轴是像素数的等比或近等比增长，能看清 ms/pair 是走直线（按像素计费）
还是走平（固定成本主导）。

## 2. 两个宿主，两种计时

八引擎分属两个程序，口径只能"各自最干净"再加归一说明。

### SMV 硬件后端（dlssg / xess / vfg / fsr4）

```powershell
smv-live.exe --offline --w W --h H --multi 2 --frames 40 --pixfmt rgb24 <flag>
```

- `cwd` 必须是 `…\resources\engine`（后端 DLL 按相对路径找）。
- stdin 是一份**预写好**的裸 RGB24 文件（`W*H*3` 字节/帧），stdout 重定向到 NUL：
  没有编码器、没有磁盘写，避免把 I/O 混进计时。
- 环境里清掉 `SMV_DLSSG_LIMIT`，否则 DLSSG 的帧数上限会被外部限制。
- 指标取宿主自己打印的稳态行（`src/smv_run.py` 的 `FINAL` 正则）：

  ```
  offline: 40 frames in, 79 out, 39 pairs, 0.31 s, 8.00 ms per pair
  ```

  这一行由 `smv-live.cpp:4090` 的 `loopS` 计时，从模型 ready **之后**开始，
  覆盖：裸帧读入 → 显存搬运 → 推理 → 回读 → 输出。也就是**含宿主 I/O**。
  1080p 每帧 6.2 MB，这一段的成本约 1–2 ms，不要忽略。
- 每档跑 5 遍，报**最小值**（长跑里最快那遍最不容易被降频污染）。

### RIFE 四变体（TensorRT）

`src/rife_trt.py` 用 `src/trt_runner.py` 的 `build_engine` + `TrtRunner`：

```
[1, 11, Hp, Wp]  fp16  →  NCHW 输入，裁出 [0:3] 为 RGB 输出
```

- `Wp/Hp` 按模型的 tile multiple 上取整、边缘 padding（4.6=32、4.25lite=128、
  4.26 / 4.26heavy=64 —— 与 vsmlrt 一致）。输出裁回原尺寸。
- 每次 `tween` 只拷 7 个会变的 plane（A 三通道、B 三通道、t）；meshgrid 与两个
  `2/(n−1)` 常量在构造时上传一次（`_fill_static`）。
- 计时区间（`tween` 内 `perf_counter`）：numpy→tensor→device 拷贝 + `execute_async_v3`
  + `torch.cuda.synchronize()`，**不含**输出回读到 CPU（那一步只发生在质量测试）。
  `ms_gpu` 单列了 enqueue+sync 那段，可以看到上传占了多大比例。
- 每档 3 遍，每遍 39 对取均值，再对 3 遍取最小值。

### 为什么 RIFE 必须交错跑

按模型顺序跑完四个变体时，先跑的那个拿到的是"刚从空闲态进入"的时钟，
后跑的已经在热态/降频态，排序直接被时钟状态决定。所以 `bench_rife_group`
改成：**一遍覆盖四个模型**，再跑下一遍。代价是显存要同时放四个 engine，
收益是四个变体看到的是同一段热历史。

## 3. 静态 engine 还是动态 engine

RIFE 的 ONNX 输入本来就是 `[1,11,None,None]`，TensorRT 完全可以建一个动态 shape
engine（min/opt/max 三个 bucket）省掉每档一次编译。这里仍然选了**静态**：

| 形状 | 静态 | 动态 | 差 |
|---|---|---|---|
| 1280×720 | 5.76 ms | 6.21 ms | +7.8% |
| 1920×1080 | 11.65 ms | 12.00 ms | +3.0% |
| 2560×1440 | 21.29 ms | 23.75 ms | +11.6% |
| 3840×2160 | 47.52 ms | 46.13 ms | −2.9% |

（`src/probes/probe_dynamic_engine.py`，20 次迭代，只看 enqueue+sync。opt bucket 是
1088×1920，所以 1080p 几乎无损、其它档 3–12% 抖动。）
省编译时间会让"每像素耗时"这条曲线带上 10% 的假波动，而这条曲线正是本次要看的
东西，所以用静态。engine 落盘在 `VFI_TRT_CACHE\rife_<name>_<W>x<H>_fp16.engine`，
每档一次 54–78 s，之后直接加载。

坑：动态 profile 的 min shape 必须满足 tile 对齐，`(1,11,360,640)` 会被 TRT 拒绝：

```
Profile kMIN values are not self-consistent. Mul_52: dimensions not compatible
for elementwise. Condition '==' violated: 360 != 384
```

改成 `MIN=(1,11,384,640)`、`OPT=(1,11,1088,1920)`、`MAX=(1,11,2176,3840)` 才通过。

**engine 缓存是可丢弃中间产物**：`rife4.26heavy` 的 4K engine 有一次在机器忙碌时
构建，tactic 落到极差解，读数是 6240 ms/pair（正常 106）。删掉重建即恢复。
任何"某一档突然慢 50 倍"先怀疑 engine，不要怀疑代码。

## 4. RIFE 的 11 通道输入

`vsmlrt.RIFEMerge` 喂的是 11 个 plane（`_external/vsmlrt.py:726/831`，
`get_rife_input` 在 `:618-671`）：

| 通道 | 内容 |
|---|---|
| 0–2 | 帧 A，RGB，[0,1] |
| 3–5 | 帧 B，RGB，[0,1] |
| 6 | 时间点 t ∈ [0,1]（这里恒为 0.5） |
| 7 | 水平 meshgrid，`2*j/(W−1) − 1` |
| 8 | 垂直 meshgrid，`2*i/(H−1) − 1` |
| 9 | 常量 `2/(W−1)` |
| 10 | 常量 `2/(H−1)` |

怎么确认的：`src/probes/probe_rife_layout.py` 用 onnxruntime CPU 在 256×256 上
喂 t=0 / 1 / 0.5 —— t=0 必须原样返回 A，t=1 返回 B，t=0.5 应落在真中间帧上。
四个模型的实测（MAE）：t→A 0.0000–0.0020，t→B 0.0000–0.0014，t=0.5 vs GT
0.0064–0.0069。

**死路**：只喂 7 通道（`[A,B,t]` + 4 个零）输出全错（MAE ≈ 0.24），乘 255 也不行。
少的就是那 4 个 meshgrid/常量 plane，模型靠它们把归一化坐标换回像素。

## 5. 质量指标

1080p，19 个真中间帧（`quality/frames/<engine>/0002.png …`）。

| 指标 | 怎么算 |
|---|---|
| VMAF | ffmpeg `libvmaf`，`log_fmt=json`，`feature=name=psnr\|name=float_ssim`，取 pooled 值与逐帧值 |
| PSNR-Y | 同一个 libvmaf 调用里的 `psnr_y`，Y 平面（人眼权重最大） |
| SSIM | `float_ssim`（不是 8-bit 整数版） |
| MAE | numpy 在 RGB [0,1] 上 `mean(|gen − gt|)`，对 19 帧整体求 |
| 锐度/GT | `var(Laplacian(gen)) / var(Laplacian(gt))`，< 1 偏软，> 1 过锐 |
| 赢帧 | 逐帧 VMAF 最高的引擎记一分，19 帧合计 |

ffmpeg 的 filter graph 里不能出现非 ASCII 路径，也不能出现盘符冒号，
所以序列被硬链到 `VFI_TMP\vseq` 下的 ASCII 目录再跑（`src/metrics.py`）。

## 6. 对比图

`src/make_compare.py` 对三帧（`PICKS = [2, 9, 16]` → 0006 / 0020 / 0034）各出三张：

- `panel_*`：5 列 × 2 行的网格，第一行「左邻帧 A | 真值 | 4 个引擎」，第二行剩下 4 个，
  每块上方一条标题条（标题 + 该引擎的主色）。
- `zoom_*`：自动找"最忙"的 640×360 窗口（Laplacian 响应经 boxFilter 求和取极大），
  1:1 裁剪，看边缘与细线。
- `diff_*`：`|生成 − GT| × 4`，看误差集中在哪。

标题条上的颜色直接用各引擎曲线的主色，深色（如 vfg 的 `#1a1a1a`）会按亮度提亮，
否则在黑底上看不见（`readable()`）。

## 7. 已知噪声与异常

| 现象 | 结论 |
|---|---|
| 同档位跨轮差异 5–40% | SM 时钟在 boost / power-limit 之间漂移；已用 5 遍取最小 + 交错抵消系统性偏差，但压不掉全部。**引擎间 20% 以内的差不要下结论。** |
| `rife4.26` 1440p 比 w2240 还快 | 同上，属于读数噪声，不是真实的非单调。 |
| VFG 启动偶发 `exit 3221225725`（`0xC00000FD` 栈溢出） | 卡在 `native: runtime folder …` 之后就没了。重试即可（脚本最多试 15 次）。**与分辨率无关**：960×540 和 3840×2160 都会犯。 |
| VFG 耗时几乎与分辨率无关 | 真实特性：固定成本约 18 ms 主导，4K 也只 22.63 ms。 |
| 1080p 只出 18 个 tween | 质量测试要喂 **20** 张奇数帧才得到 19 个（喂 19 张少一格）。 |
| `cv2.imread` 打不开 `测试数据\…` | 非 ASCII 路径，改为 `np.fromfile` + `cv2.imdecode`。 |
| 一次性进程启动 0.8–3.2 s | 模型加载不进计时；但这也意味着**墙钟时间**不能当指标。 |

## 8. 复现

```powershell
python src\probes\probe_smv.py           # 五个 SMV 后端能不能跑
python src\probes\probe_rife_layout.py   # 11 通道布局自证（CPU，几秒）
python src\run_throughput.py             # 11 档 × 8 引擎（RIFE 侧要编译 44 个 engine，约 40 分钟）
python src\run_quality.py                # 1080p 生成帧
python src\metrics.py                    # VMAF / PSNR / SSIM / MAE / 锐度
python src\make_charts.py                # 5 张图
python src\make_compare.py               # 9 张对比图
python src\probes\probe_dynamic_engine.py
```

`run_throughput.py` 支持位置参数过滤（`720p 4k vfg rife4.6`）与 `--force`；
结果增量写入 `$VFI_DATA\throughput.json`，中断重跑不会丢已完成的部分。
