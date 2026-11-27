# k7 + aji：1080p 全量 RIFE 模型支持性、速度与一致性

结论先行：把测试集合换成 **aji 自己发布的那套 AnimeJaNai RIFE 权重**（对应 `测试环境` 里 vs-mlrt
四个模型的"上游"），1080p 下 **26 个模型全部跑通**，而且全部走 aji 的 native CUDA 内核，
没有一个退化到 TensorRT 兜底。同一份 onnx 喂给 vs-mlrt（vstrt）也能建静态 fp16 引擎并出结果，
因此可以在"同一网络 + 同一时间点"下比较：

- 速度：aji 插值帧中位数 **10.30 ms**，vs-mlrt **38.53 ms**，**3.70×**（26 个模型里 3.14×–4.50×）。
- 一致性：扣掉两侧色彩范围定义差异后，luma **psnr_y ≈ 51.5 dB**（26 个模型中位数），平均绝对差 ≈0.22 level。
- 直通帧：aji 0.05–0.10 ms（源帧原样返回），vs-mlrt 4.2–7.0 ms（源帧也要过一次色彩转换与搬运）。

## 1. 测试条件

| 项 | 值 |
| --- | --- |
| GPU | GPU0（RTX 3080 10GB，sm86），用户指定 |
| 模型集 | `vsaji/.deps/dl/rife-raw/animejanai/rife/` 里 26 个单体权重（4.7–4.26，含 7 个 `_lite`；不含 `_ensemble`、不含 `4.26_heavy`） |
| aji 侧 | `k7sfunc.RIFE_AJI`，模型号按 aji 规则编码（`4.14`→414，`4.14_lite`→4141），onnx 铺在 `work/aji-models/rife/` |
| vs-mlrt 侧 | 同一批 onnx，用 `work/aji-rife-test/probe_vsmlrt16.py` 复刻 `RIFE_NV(turbo=2)` 的口径：RGBH → 补边到 tile 倍数 → `vsmlrt.RIFE(_implementation=1, fp16, static_shape, use_cuda_graph)` → 裁回 → `YUV420P8` |
| 源 | `work/aji-rife-test/src1080p_ffv1.mkv`：`测试数据/1080p` 的 40 张 PNG → FFV1 无损、YUV420P8、BT.709 limited |
| 倍帧 | `fps_num=2`；1080p 40 源帧 → aji 79 帧 / vs-mlrt 80 帧 |
| 解释器 | 本仓库 uv venv（vapoursynth 74，见 `AGENT.md`） |
| aji 运行时 | `work/aji-models/`（`vsaji/dist/aji` 副本），`aji-runtime.json` 指向 TensorRT 11.4 完整包（含 trtexec + sm86 builder resource） |
| 计量 | 只统计奇数索引（插值帧），丢前 4 帧预热，报 median；两轮交错（引擎顺序每轮互换）取中位数 |

为什么 vs-mlrt 侧不能直接用 `RIFE_NV`：它的 `model` 是字面量枚举，只接受
`46 / 4251 / 426 / 4262` 四个旧模型号（`k7sfunc/src/k7sfunc/mod_memc.py:677` 用列表索引取文件名），
其余模型号进不去。`probe_vsmlrt16.py` 按同一套 k7 语义自建图，只把模型文件换成 animejanai 的 fp16 onnx。

为什么源必须是 FFV1 而不是 PNG：PNG 解码 + 1080p CPU resize 每帧 20–30 ms，全部落在插值帧上，
会把 aji 的 9 ms 污染成 73 ms。基线 `one.py --engine src` 是 1.0 ms/帧。

## 2. 支持性 + 速度 + 一致性（1080p）

两轮交错的中位数；`×` = vsmlrt ÷ aji；`psnr_y` 为 aji 与 vsmlrt 同索引插值帧（每模型 3–7 帧）的平均，
已把 vsmlrt 侧 full range 映射回 limited（见 §4）。

| 模型 | aji (ms) | vs-mlrt (ms) | × | psnr_y (dB) |
| --- | --- | --- | --- | --- |
| `4.7` | 10.35 | 36.49 | 3.53 | 51.07 |
| `4.8` | 10.30 | 38.25 | 3.71 | 51.67 |
| `4.9` | 10.52 | 37.22 | 3.54 | 51.43 |
| `4.10` | 11.06 | 41.91 | 3.79 | 51.53 |
| `4.11` | 10.79 | 41.30 | 3.83 | 51.31 |
| `4.12` | 11.33 | 41.86 | 3.70 | 52.32 |
| `4.12_lite` | 10.02 | 39.56 | 3.95 | 51.01 |
| `4.13` | 12.13 | 49.06 | 4.04 | 52.12 |
| `4.13_lite` | 9.57 | 34.86 | 3.64 | 51.23 |
| `4.14` | 10.30 | 39.27 | 3.81 | 51.58 |
| `4.14_lite` | 10.30 | 36.73 | 3.57 | 51.87 |
| `4.15` | 10.24 | 37.06 | 3.62 | 51.72 |
| `4.15_lite` | 8.69 | 33.16 | 3.82 | 51.49 |
| `4.16_lite` | 9.01 | 35.87 | 3.98 | 51.55 |
| `4.17` | 10.64 | 37.11 | 3.49 | 51.38 |
| `4.17_lite` | 8.87 | 33.80 | 3.81 | 52.12 |
| `4.18` | 10.44 | 36.66 | 3.51 | 52.17 |
| `4.19` | 10.28 | 39.16 | 3.81 | 51.15 |
| `4.20` | 11.02 | 38.89 | 3.53 | 51.95 |
| `4.21` | 14.56 | 45.78 | 3.14 | 51.52 |
| `4.22` | 12.12 | 39.64 | 3.27 | 51.29 |
| `4.22_lite` | 8.69 | 38.81 | 4.47 | 50.66 |
| `4.23` | 11.85 | 38.89 | 3.28 | 51.51 |
| `4.24` | 10.11 | 37.39 | 3.70 | 51.59 |
| `4.25` | 8.95 | 36.02 | 4.02 | 52.05 |
| `4.26` | 9.36 | 42.12 | 4.50 | 51.69 |
| **中位数** | **10.30** | **38.53** | **3.70** | **51.54** |

要点：

- **`4.21` / `4.22` / `4.23` 两边都偏慢**（18.7 MB 权重，比 10.7 MB 那批重），但加速比反而略低（3.1–3.3×），
  因为它们让 aji 从 10 ms 涨到 12–14.6 ms，而 vs-mlrt 只从 38 涨到 40–46 ms。
- **`_lite` 普遍更快**：aji 8.7–10.3 ms，vs-mlrt 33–40 ms，加速比 3.6–4.5×。
- **`4.20`（30.8 MB，最重的单体权重）aji 只要 11.0 ms**，说明 native 内核的耗时随通道宽度增长得很温和。
- GPU0 有 wallpaper engine 断续占用，单轮绝对值有 ±10% 抖动（`4.13` vs-mlrt 两轮 49.1 / 44.6 ms），
  所以表里用两轮中位数。

### 为什么全都能走 native

`vsaji/.deps/engine/src/native/rife_plan.cpp:13-104` 逐条匹配节点名（`/Split`、`/encode/…`、
`/blockN/conv0/conv0.0/conv0.0.0/Conv`、`/lastconv/lastconv.0/ConvTranspose`、`/lastconv.1/DepthToSpace`…）。
animejanai 这套 4.7–4.26 权重是同一个导出流水线出来的（`traiNNer-redux`，io_dtype fp16），
名字与结构一致，所以 plan 全部匹配。证据：跑完 26 个模型后 `work/aji-models/rife/` 里
**没有任何新的 `.engine` 或 `.build.log`**，只有之前 vs-mlrt 那四个模型留下的 `rife_v4.6` 引擎与
`rife_v4.25_lite` 失败日志。

## 3. 速度差异的来源

vs-mlrt 的 38 ms 不是"网络更慢"，而是管线形态：

- vsmlrt 每插值帧要把 **11 通道 × 1088 × 1920 的 fp16 输入**从主机传到设备（≈45 MB），
  再把 3 通道结果传回来（≈12 MB），中间还有一次 RGBH 与 YUV 的转换。trtexec 自报
  GPU compute 只有 **23.5 ms**（4.14，`--useCudaGraph`），其余是拷贝与同步。
- aji 是 YUV 进 YUV 出、整链在设备上（`libaji` 的 C-ABI 设计），native 内核直接吃 `1x11x1088x1920`
  的 fp16 buffer，只回传 3 通道；直通帧甚至不用出门。
- 这也解释了直通帧的差距：vs-mlrt 4.2–7.0 ms（源帧也要过色彩转换），aji 0.05–0.10 ms。

## 4. 输出一致性

直接比是 psnr_y ≈ 29.9 dB、平均绝对差 7.2 level —— 差异几乎全部是**范围定义**：
对 luma 做最小二乘拟合得到 `aji = 0.8588 × vsmlrt + 15.99`，`0.8588 = 219/255`，即标准 full→limited 映射。
（`work/aji-rife-test/diag2.py`）把 vs-mlrt 侧映射回 limited 后：

- 26 个模型 psnr_y 均值 50.7–52.3 dB（中位数 51.5 dB），`pct_y_gt1` 1.2–1.7%；
  色度更高（psnr_u/v 56 dB 量级）。`4.14` 的逐帧明细：52.1 / 51.4 / 50.9 / 52.9 / 52.2 / 51.1 / 50.6 dB。
- 平移搜索（±2 像素）最优仍是 (0,0)，说明时间点与空间位置对齐没问题；残差是 fp16 舍入 + 两种实现的
  归一化/插值细节差异。

范围差异的根因在 k7 的 vs-mlrt 分支：`k7sfunc/src/k7sfunc/mod_memc.py:533`
`core.resize.Bilinear(clip=fin, format=fmt_in, matrix_s="709", range=1 if colorlv==0 else None)`，
limited 输入被展开成 **full** 输出并打 `_ColorRange=FULL`，连直通帧都被改动；
`RIFE_AJI` 则保持 limited。**接 aji 输出进 limited 管线不需要额外处理，接 `RIFE_NV` 输出需要重新 tag/映射。**

## 5. 复现

```
uv venv --python 3.12 .venv
uv pip install --python .venv\Scripts\python.exe vapoursynth==74 numpy vsrepo
.venv\Scripts\vsrepo.exe install ffms2 imwri
.venv\Scripts\python.exe work\aji-rife-test\mk_src.py                 # PNG → FFV1
.venv\Scripts\python.exe work\aji-rife-test\stage_aji_models.py       # 26 个 onnx → work/aji-models/rife
.venv\Scripts\python.exe work\aji-rife-test\run_all.py --rounds 2 --models <26 个模型>
.venv\Scripts\python.exe work\aji-rife-test\summarize.py              # 速度表 + 一致性
```

单次：`one.py --engine {src|aji|vsmlrt16|vsmlrt} --model <m> --source ffv1 --out <dir>`；
单帧比对：`compare.py --a <vsmlrt.yuv> --b <aji.yuv> --map f2l`。

TRT-RTX 后端（§7）：`one.py --engine aji --aji-dir work\aji-rtx --model 4.14`（默认仍是 native）；
`trtrtx_engine.py 4.14`（conf 里 `native=off`，真正跑 trtrtx 引擎）；`--backend=TensorRT` 可让同一个脚本走 TRT 11.4 包。

分阶段耗时（§8）：先 `$env:AJI_PROFILE='1'; $env:AJI_PROFILE_EVERY='37'`，再看 stderr；
vs-mlrt 侧的纯内核用 `trtexec_load.py <engine> 200`（`trtexec --loadEngine --useCudaGraph`）。

脚本坑（已修）：同一个进程里 `LoadPlugin(vstrt.dll)` 两次会让 `core.trt.Version()["path"]` 返回乱码字节，
`import vsmlrt` 直接 `UnicodeDecodeError`；`probe_vsmlrt16.setup()` 现在按 `hasattr(core, "trt")` 跳过重复加载。

## 6. 没覆盖 / 局限

- **`_ensemble`（20 个）**：k7 的 `RIFE_AJI` 有 `ensemble` 参数，vsmlrt 侧 `RIFE` 也有，
  但本次全部按 `ensemble=False` 测；ensemble 会成倍增加算力，两边都要单独量。
- **`4.26_heavy`**：aji 的模型号映射（`vsaji/.deps/engine/src/aji_trt.cpp:707-721`）只有 `_lite` 分支，
  没有 heavy；`4.26` → `rife_v4.26`，`4262` 也只会加载 `rife_v4.26`。k7 的 `RIFE_AJI` 参数层同样拒绝
  `"4.26_heavy"`（`:1223` 的 `mini.isdigit()`）。想测只能改引擎或改名冒充，不属于"支持"。
- **DirectML / Vulkan 后端**：包内没有对应 DLL（`Version()["missing"]` 里为缺失），未测。
  TensorRT-RTX 后端已单独测过，见 §7。
- **GPU0 有背景占用**，绝对值 ±10%；要严格数字得独占 GPU（或换 GPU1，但那台桌面占用更重）。

## 7. aji 的 TensorRT-RTX 后端（trtrtx）

aji 还有一个自带的 **TensorRT-RTX** 后端（`vsaji/dist/aji-rtx/`：`aji_trtrtx.dll` 18.8 MB，
`trtrtx/tensorrt_rtx_1_7.dll` 229 MB + `tensorrt_onnxparser_rtx_1_7.dll` 2.3 MB，
`aji-runtime.json` = `{"trtrtx":"trtrtx","format":1,"trt":"trt"}`）。**结论先说：native 可用时它完全不参与；
只有强制关掉 native，它才作为兜底出手，速度比 native 慢 1.6–1.8×，但仍是 vs-mlrt 的 2.1–2.3×。**

### 7.1 默认路径：TRT-RTX 包 + k7 原样调用 = 还是 native

k7 的 `RIFE_AJI` 把 conf 写死 `backend=TensorRT`（`k7sfunc/src/k7sfunc/mod_memc.py:1238`），
在只放了 `aji_trtrtx.dll`（删掉 `aji_trt.dll`）的包上，插件按自己的 auto 梯队改选后端
（`vsaji/src/plugin.cpp` 的 `pick_auto()`：`trt+native=1` → `TensorRT-RTX` → `trt+native=0`）。
aji_trt.dll 不在包里时它直接选 TensorRT-RTX，不打 warning；只有 GPU 低于 compute capability 8.0
时才会附一条 note，说明原生内核为什么没被选中。

`Version()["backend"]` 变成 `TensorRT-RTX`，`missing` 里 TensorRT / DirectML / Vulkan 都缺。
4.14 跑出来 **11.56 ms**（p10 10.72 / p90 19.05，直通帧 0.08），`rife/` 下没有任何引擎文件，
输出与 TensorRT 后端的 native 结果**逐像素相同**（psnr_y = ∞）——native CUDA 内核编在每个后端 DLL 里，
和选哪个后端无关。

> 注：旧的 `runtime_probe.cpp` / `pick_backend` 已在「改用上游 release」的收窄里删除，
> 现在只剩上述梯队这一处选择逻辑。

### 7.2 关掉 native 才轮到 trtrtx builder

conf 加 `native=off`（`work/aji-rife-test/trtrtx_engine.py`），后端就地建 TensorRT-RTX 引擎：

| 模型 | native（同包） | trtrtx 引擎（中位） | 建引擎（冷/热） | 引擎体积 | vs-mlrt |
|---|---|---|---|---|---|
| 4.14 | 11.56 | **17.34**（冷 17.64） | 11.97 s / 0.57 s | 21 MiB | 39.27 |
| 4.26 | 9.36 | **16.98** | 11.68 s | 23 MiB | 42.12 |
| 4.15_lite | 8.69 | **14.95** | 8.23 s | 16 MiB | 33.16 |

（native 列取自 §2 的 26 模型矩阵，那批跑在 `work/aji-models` 包上；4.14 在 TRT-RTX 包里实测 native 为 11.56 ms，
与矩阵的 10.30 ms 属同量级噪声。vs-mlrt 列为同一 onnx 的 `vsmlrt16` 中位数。）

- 冷启动那 ~11–12 s 里，引擎生成本身只要 **0.55–0.62 s**（`Engine generation completed in 0.615 s`，
  "Compiler backend is used during engine build"），其余是首次 **warm-up JIT**（`warm-up inference done (10.60 s)`）；
  `.jitcache` 落盘后热启动 graph 只要 0.57 s。
- 引擎缓存名 `<hash>.trtrtx-1.7.1.gpu-<gpu>-sm8.rtxengine` + `.jitcache` + `.onnxinfo` + `.build.log`，
  比 TensorRT 11.4 全量 builder 的 746 MB 小两个数量级，构建也快两个数量级。
- 与 native 输出对比：f0001 psnr_y 65.0 dB（max 差 2）、f0009 psnr_y 58.4 dB（max 49，0.32% 像素差 >1），
  同一画面，只剩 fp16 数值差。

### 7.3 fp32 权重（vs-mlrt 那套）在 trtrtx 上依然不可用

- `rife_v4.25_lite.onnx`（fp32）：建引擎直接失败，报错与 TensorRT 11.4 一模一样——
  `Error Code 4: API Usage Error (IElementWiseLayer /Mul: broadcast dimensions must be conformable ...)`，
  是导出图本身的问题，换 TRT 版本救不了。
- `rife_v4.6.onnx`（fp32）：trtrtx **能建能跑**（不再像 TRT 11.4 那样 `enqueueV3 failed`），但输出是坏的
  （与任一正常模型比 psnr_y 只有 6.4 dB、亮度均值差 102 level）——aji 的视频路径按 fp16 buffer 绑定，
  fp32 图只是"能出帧"，不能当可用结果。

**因此 k7 侧的可选路径只有一条：native 可用就用 native；TRT-RTX 引擎路径必须手写 conf（`native=off`），
`RIFE_AJI` 没有这个开关，k7 管线触达不到。**

## 8. 分阶段耗时（插桩）

§2/§3 只能看到"一帧 8–10 ms"，但里面混着 YUV 上传、CPU 交织、GPU 内核、后处理、回传。
为了拆开，给 aji 的插件和引擎都加了**可选**计时桩（默认关闭，零开销），口径与 `one.py` 一致：
只统计插值帧、每帧一次、单位 ms。

启用方式：

```
$env:AJI_PROFILE='1'          # 打开
$env:AJI_PROFILE_EVERY='37'   # 每 N 帧汇总输出一行（默认 10）
```

stderr 会出现两类行（一行 = 该窗口每帧平均）：

```
[aji-profile/plugin] frames=37 up_a=0.57 up_b=0.64 pack=0.54 h2d=0.67 engine=7.43 sync=0.00 download=1.37 d2h=0.73 split=0.64 total=10.02
[aji-profile/trt]    mode=native frames=37 setup=0.08 scene=0.10 staging=0.05 pre=0.14 fill=0.01 infer=6.85 post=0.12 crop=0.02 total=7.38
```

### 8.1 插件侧（主机路径，1080p 4:2:0 8bit）

| 段 | 含义 | 4.14 native | 4.26 native |
|---|---|---|---|
| `up_a` / `up_b` | 前一帧 / 后一帧上传（含下面的 pack + h2d） | 0.57 / 0.64 | 0.58 / 0.67 |
| `pack` | CPU 把 U/V 交织进 staging 缓冲（960×540 逐像素） | 0.54 | 0.57 |
| `h2d` | 两次 `cuMemcpy2D`（luma + chroma，pageable） | 0.67 | 0.68 |
| `engine` | 调 `aji_infer_rife_with_scene` 整段 | 7.43 | 5.93 |
| `sync` | `cuStreamSynchronize` | 0.00 | 0.00 |
| `download` | 回传（含 d2h + split） | 1.37 | 1.44 |
| `d2h` | 两次 D2H 拷贝 | 0.73 | 0.77 |
| `split` | CPU 把交错 chroma 拆回 U/V | 0.64 | 0.67 |
| **total** | 一帧插件侧总时间 | **10.02** | **8.63** |

- `sync` 恒为 0：引擎最后的 crop 是同步 D2H（`cudaMemcpy2D`，非 Async），流早在那里就被等干了。
- 主机侧固定成本 ≈ **2.6 ms/帧**（上传 1.2 + 回传 1.4），与模型无关；1080p 每帧只搬 ~3 MB（2×(Y 1920×1080 + UV 1920×540)），
  慢的是 **pageable 内存**——理论 3 MB 在 PCIe 4.0 x16 上不到 0.3 ms。想再压，得换 pinned + 异步流。
- CPU 的 `pack`+`split` 合计 1.2 ms/帧（占插值帧 12%），是"逐像素交织 518k 次"的直接代价。

### 8.2 引擎侧（GPU，CUDA event 打点）

| 段 | 4.14 native | 4.26 native | 说明 |
|---|---|---|---|
| `setup` | 0.08 | 0.09 | 首次/换格式建 staging（之后每帧只剩记录 event） |
| `scene` | 0.10 | 0.10 | k7 传了场景标记，引擎侧只做判定 |
| `staging` | 0.05 | 0.05 | 两帧拷进 pad 缓冲（D2D） |
| `pre` | 0.14 | 0.15 | YUV→fp16 张量（6 通道） |
| `fill` | 0.01 | 0.01 | 时间步通道 6 写入 |
| **`infer`** | **6.85** | **5.35** | native CUDA 内核 |
| `post` | 0.12 | 0.12 | fp16 RGB→padded YUV |
| `crop` | 0.02 | 0.02 | 拷回输出平面（D2H，同步点） |
| **total** | **7.38** | **5.89** | 即插件侧 `engine` 的 99% |

**固定开销（除 infer 外全部）只有 0.5–0.55 ms，推理内核占 91–93%。** 所以"aji 快"不是靠省拷贝，
就是内核快；换更小的模型/更少的层数才是主要收益来源（4.26 比 4.14 的内核少 1.5 ms）。

### 8.3 各引擎的纯 GPU 内核对比（同一 onnx / 1080p / fp16）

| 路径 | 后端 / builder | GPU 内核 | 一帧端到端 |
|---|---|---|---|
| aji native | 自带 CUDA 内核（mma.sync fp16） | **6.85**（4.14）/ 5.35（4.26） | 10.0 / 9.0 |
| aji + TensorRT-RTX | TRT-RTX 1.7 编译器后端 | **11.70**（4.14）/ 11.22（4.26） | 15.2 / 14.5 |
| aji + TensorRT | TRT 11.4 trtexec 构建 | **16.60**（4.14） | 20.0 |
| vs-mlrt（vstrt + TRT 10.16） | TRT 10.16，静态 1×11×1088×1920 | **21.30**（4.14，`trtexec --loadEngine`） | 38.5 |

vs-mlrt 那行的 21.30 是 `trtexec --loadEngine --useCudaGraph --warmUp=2000 --iterations=200`
（`work/aji-rife-test/trtexec_load.py`）自报的 GPU Compute Time，同一次测量里 H2D 6.86 ms、D2H 1.91 ms、
`trtexec` latency 30.08 ms；图内实测 38.5 ms 与之差 ~8.4 ms，那部分是 zimg 的 YUV420P8→RGBH、RGB→YUV
和 VS 框架开销——vsmlrt 走的是"主机构图 + 11 通道 fp16 张量"，aji 是"YUV 进 YUV 出、整链在设备"。

同一个 animejanai onnx，纯内核 CNN 部分就能从 21.3 ms（TRT 10，cudnn/cublas tactic）降到 16.6 ms（TRT 11.4）、
11.7 ms（TRT-RTX 编译器后端）、6.85 ms（aji 手写 native）——**换引擎比换模型更值钱**。

### 8.4 桩的位置与重建

| 位置 | 内容 |
|---|---|
| `vsaji/src/aji_host_internal.h` | `struct AjiProf` + 四个接口声明 |
| `vsaji/src/aji_host.cpp` | `g_aji_prof`、`aji_prof_on/ms/flush/frame_end`、`infer()` 五段计时、析构时 flush |
| `vsaji/src/aji_frames.cpp` | upload 的 `pack`/`h2d`、download 的 `d2h`/`split` |
| `vsaji/.deps/engine/src/aji_trt.cpp` | 匿名 namespace 里的 `Prof`/`prof_flush`/`PROF_MARK`，RIFE 路径 8 个标记，`aji_destroy` 里兜底 flush |

```
powershell -ExecutionPolicy Bypass -File vsaji\scripts\build-plugin.ps1 -VsInclude E:\code\dlssg-vfi\.venv\Lib\site-packages\vapoursynth\include
```

> 注：本节数据来自当时从源码自建后端的版本；`build-engine.ps1` / `fetch-engine.ps1` /
> `fetch-trt-rtx.ps1` 已在 2026-11-27 随「后端改用上游 release」退役（现在只需
> `fetch-aji.ps1` + `build-plugin.ps1` + `pack.ps1`）。`AJI_PROFILE` 插桩在插件侧仍然有效。

产物在 `vsaji/out/engine/`、`vsaji/out/rife_aji.dll`；本轮的 DLL 已复制到 `work/aji-models/`（TensorRT 包）
与 `work/aji-rtx/`（TRT-RTX 包）。插桩只影响 `AJI_PROFILE=1` 时的输出，**不改变计算结果**——
同一帧在 native / TRT / TRT-RTX 三条路径下 psnr_y 64.6 dB（max 差 6），差异纯属 fp16 舍入。

## 附录 A：vs-mlrt 自带的那四个模型（第一轮结论，保留）

这轮是为了回答"原本用 vs-mlrt 的模型，aji 能不能替代"。四个模型里 aji 只能原生吃 `4.26`
（animejanai 的 fp16 版本），失败点各不相同：

| 模型 | vs-mlrt（`RIFE_NV`） | aji（`RIFE_AJI`） | aji 的失败点 |
| --- | --- | --- | --- |
| `4.6` | ✅ 28.5 / 29.2 ms | ❌ `rife_aji: aji_infer_rife: rife enqueueV3 failed` | TRT 引擎建成（746 MB，44.4 s），首帧推理失败：ONNX 是 fp32，aji 按 fp16 绑定 |
| `4.25_lite` | ✅ 30.8 / 33.8 ms | ❌ `aji_configure: trtexec failed building engine for rife_v4.25_lite` | trtexec：`Error[4]: API Usage Error (IElementWiseLayer /Mul: broadcast dimensions must be conformable …)`，随后 `Attempting to access an empty engine!` |
| `4.26` | ✅ 33.4 / 37.0 / 39.5 ms | ✅ 9.4–11.2 ms（native） | — |
| `4.26_heavy` | ✅ 40.8 / 42.5 ms | ❌ `模块 RIFE_AJI 的子参数 model 的值无效` | k7 参数层拒绝；aji 也没有 heavy 的模型名 |

根因三条（任何一条都足以否掉一个模型）：

1. **fp32 导出与 fp16 buffer 不兼容（4.6）。** `aji_trt.cpp:3078` 用 `aji_fill_f16(tin + plane*2*6, …)`
   写时间通道，`:3092-3099` 把这块 buffer 绑到输入张量 —— 视频路径硬编码 fp16 I/O。
   vs-mlrt 的 onnx（4.6 / 4.25_lite / 4.26_heavy）输入都是 `[1,11,?,?] elem=1`（fp32），
   stronglyTyped 引擎按 fp32 绑定，`enqueueV3` 必然失败；animejanai 的权重是 `elem=10`（fp16）。
2. **native 内核只认 animejanai 的导出。** vs-mlrt 的 fp32 导出节点命名不同 → 一律落到 TensorRT 兜底。
3. **模型名映射没有 heavy、也没有"任意文件名"。** 见 §6。

结论：想用 aji 的 native 内核，就得用 animejanai 那套 fp16 权重 —— 也就是 §2 那 26 个模型；
vs-mlrt 自带的四个 fp32 onnx 在 aji 上不是"慢"，而是不成立。
