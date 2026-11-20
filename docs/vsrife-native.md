# vsrife — native RIFE CUDA kernels as a VapourSynth filter

把 mpv-AnimeJaNai / libaji（the-database/animejanai-inference）里那套手写 CUDA RIFE 内核
从播放器里拆出来，包成一个独立的 VapourSynth 4 插件：`rife_native.Rife`。
没有 TensorRT、没有 DirectML、没有 Python，模型直接读 vs-mlrt 的 ONNX。

## 0. 一句话

`RIFE::create()` 解析 ONNX 图 → 每层由 `k_gconv`（mma.sync m16n8k16，fp16 累加）算，
重采样全部折进 `k_gather`（一像素一线程），整条链捕获成一个 CUDA Graph。
插件只负责 YUV ⇄ 内部 fp16 RGB 的搬运和 VS 的帧映射。

## 1. 来源与授权

- 内核源码 vendored 自 `the-database/animejanai-inference`（`src/native/`，commit 3c9efa0），
  原样放在 `vsrife/src/native/`。
- **该仓库没有 LICENSE 文件**，按"保留所有权利"处理：这份代码只在本机自用，
  不对外发布、不随任何分发包再分发。文件头保留来源标注。
- 上游的 Vulkan 端口（`src/vk/rife.cpp`）没有搬；这里只有 CUDA。

## 2. 用法

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin(r"E:\code\dlssg-vfi\vsrife\bin\rife_native.dll")

model = open(r"...\rifemodels\x\rife_v4.26.onnx", "rb").read()   # data 参数收 bytes
out = core.rife_native.Rife(clip=src, model=model, factor=2, sc=0.15)
out.set_output()
```

模型建议用 aji 自己的 fp16 一套（`the-database/animejanai-inference` 的 `models-rife-fp16-2`
资产，`gh release download models-rife-fp16-2 -p rife-fp16-2.7z`，Windows 自带 `tar -xf` 可解）。
它已经是 fp16、节点名就是 `rife_plan.cpp` 要的 `/Split`、`/encode/…`、`/blockN/…`，直接可用。
本地已放三个（`vsrife\models\`）：`rife_v4.16_lite.onnx`（默认，最快）、`rife_v4.26.onnx`
（常规版）、`rife_v4.17_ensemble.onnx`（质量最高档，成本约两倍）。全家族 47 个模型的实测见 5.1。
mpv-lazy / vs-mlrt 里那批 dynamo 导出的 fp32 模型（`Conv_6` 之类节点名、Split `{3,3,1,2,2}`）
不被识别，需要用 `vsrife/tools/normalize_rife_onnx.py` 改名重排后才能跑。

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `clip` | vnode | — | 源，YUV 4:2:0/4:2:2/4:4:4，8-16 bit |
| `model` | data | — | vs-mlrt 布局的 RIFE ONNX（`rife_v4.6.onnx` 等），由插件自己解析 |
| `factor` | int | 2 | 2-16。输出 `(N-1)*factor+1` 帧 |
| `device` | int | 0 | CUDA 设备号 |
| `sc` | float | 0.15 | 场景切换阈值（平均 |ΔY|/peak），0 关闭 |
| `matrix` / `range` | int | -1 | 覆盖帧属性里的 `_Matrix` / `_ColorRange` |

帧映射：输出第 `o` 帧用源 `n=o/factor`、`n+1`，时间步 `t=(o%factor)/factor`。
所以 `o%factor==0` 的帧就是源帧（`copyFrame` 直通，零推理），时间轴长度不变。
`sc>0` 且判为场景切换时，输出 `n` 而不是插值帧。

## 3. 构建

```bat
E:\code\dlssg-vfi\vsrife\src\build.bat
```

- 需要 MSVC x64（`cl.exe` 在 `C:\software\msvc\...\14.44.35207`）、nvcc 13.2、VS R74+ 头文件。
- 环境变量可覆盖：`CUDA_PATH`、`VS_INCLUDE`、`VCVARS`（默认值写在 bat 里）。
- `.cu` 用 nvcc 编，`.cpp` 用 cl 编，cl 链接 `cudart.lib`。
- `-gencode arch=compute_80,code=[sm_80,compute_80]`：Ampere 及以后跑 SASS，更新的架构 JIT PTX。
- 产物 `vsrife\bin\rife_native.dll`。语法自检用 `src\check.bat`（cl `/Zs` + nvcc `-ptx`）。
- 安装：把 dll 拷到 `...\site-packages\vapoursynth\plugins\`，之后 `core.rife_native.Rife(...)` 直接用。
  注意这样就**不要**再 `LoadPlugin` 同一个 dll，VS 会报 "already loaded"；开发时建议不装、
  在脚本里显式 `LoadPlugin(r"...\vsrife\bin\rife_native.dll")`，测的永远是最新构建。
- `vsrife\models\rife_v4.26.onnx` 是这儿默认用的模型（aji `models-rife-fp16-2` 资产里的 fp16 版）。

坑：`.bat` 里不要用 `set CL=...`，`CL` 是 cl.exe 的保留附加参数变量，会被当成源文件读入。

## 4. 内部结构

```
src/plugin.cpp    VS 包装：参数、帧映射、场景直通、属性
src/engine.cpp    GPU 这一段：pad64、缓冲、上传/推理/下载、场景检测
src/convert.cu    YUV <-> fp16 RGB（BT.601/709/2020，limited/full，色度双线性）
src/native/*      libaji 的 native 内核，原样 vendored
```

一帧的路径：

1. A/B 三平面 `cudaMemcpy2DAsync` 到紧凑 staging（`plane_upload`）。
2. 可选：luma SAD kernel → 回读 8 字节 → 判场景。
3. `k_yuv_to_rgb`：8/10/12/16 bit → fp16 RGB，色度双线性上采样、边缘复制到 64 倍数。
4. `Rife::run(in11, t, out3)`：encoder×2 → 每块 `k_gather`+conv → `k_final`。
5. `k_rgb_to_yuv`：RGB → YUV，色度按其覆盖的 2×2 RGB 取平均。
6. 三平面 `cudaMemcpy2D` 回 host 帧（`plane_download`），一次性 `cudaStreamSynchronize`。

GPU 工作在 `RifeData::mu` 下串行：VS 的 `fmParallel` 会让多个工作线程同时进来，
一个 stream、一份 scratch，同时只能跑一帧。

## 5. 性能（实测）

RTX 3080 10GB，`rife_v4.16_lite`，`factor=2`，合成片（无解码开销），每个模型独立进程，
warm 6 帧后计 24 帧：

| 格式/尺寸 | ms/输出帧 | ms/插值帧 |
|---|---|---|
| YUV420P8 1080p | 3.08 | 6.2 |
| YUV420P10 1080p | 3.75 | 7.5 |
| YUV444P8 1080p | 3.89 | 7.8 |
| YUV420P8 4K | 10.3 | 20.6 |

- `factor=2` 时一半输出帧直通（`copyFrame`），所以 ms/输出帧 ≈ 插值成本的一半。
- 同条件 `rife_v4.26`（5 blocks 常规版）：1080p 3.04、4K 9.90；
  `rife_v4.17_ensemble`：1080p 5.68、4K 20.8。ensemble 走两遍图，接近翻倍。
- `factor=3/4` 时每插值帧降到 5.96ms：直通帧被摊薄，边际成本只由插值帧决定。
- 10bit / 4:4:4 比 8bit 4:2:0 多约 1.3-1.7ms/输出帧，是 YUV⇄fp16 RGB 转换的开销（2 帧往返）。
- 同一模型重跑的抖动约 ±0.3ms（GPU 时钟/温度）；早期一版测出的 5.09ms 是冷时钟结果，偏悲观。
- 真实片源（`测试数据\阿哈角色PV 1080p.mkv`，48 源帧 → 95 输出帧）整条 ffms2 解码 + 插值
  `268 fps`（含解码，0.35s）。
- 首次 `init` 要在真卡上为每个 conv 选 tile variant（`gpick`），1080p 约 1-3 秒。
- 未做：pinned staging、多 stream、graph 化上传下载。若瓶颈在 PCIe（4K 输入），先加 pinned。

### 5.1 模型版本矩阵

`vsrife/test/model_matrix.py` 把 aji `models-rife-fp16-2` 里全部 47 个模型各跑一遍
（`test/model_matrix.json` 是完整数据）。**47 个全部被 `rife_plan` 接受，没有一个被拒绝**——
同一套 kernel 覆盖 v4.7 到 v4.26 的所有变体，因为它们的图只是块数/K/特征通道不同。

质量口径：60fps 10bit 1080p 片源隔帧抽成 30fps，插回 60fps，与被丢掉的那一帧比 Y 平面
PSNR（30 个插值帧平均），所以数字包含格式往返转换的损失。1080p 8bit 计时。

| 模型 | ms/输出帧 | PSNR | 最差帧 |
|---|---|---|---|
| v4.17_ensemble | 5.68 | 55.24 | 49.65 |
| v4.16_lite_ensemble | 4.59 | 54.94 | 49.45 |
| v4.17 | 3.60 | 54.90 | 48.90 |
| v4.15_lite_ensemble | 4.74 | 54.89 | 49.29 |
| v4.16_lite | 2.89 | 54.74 | 49.21 |
| v4.15_lite | 2.95 | 54.69 | 49.17 |
| v4.24_ensemble | 5.73 | 54.68 | 49.93 |
| v4.20 | 3.92 | 54.52 | 49.02 |
| v4.15_ensemble | 5.70 | 54.47 | 49.26 |
| v4.24 | 3.71 | 54.33 | 48.39 |
| v4.13_lite | 3.15 | 54.30 | 48.40 |
| v4.22_lite | 2.98 | 54.25 | 47.86 |
| v4.14 | 3.62 | 54.13 | 49.48 |
| v4.26 | 3.10 | 53.46 | 48.67 |
| v4.26_heavy | 3.44 | 53.00 | 48.01 |
| v4.21 | 4.06 | 53.24 | 46.77 |
| v4.7 | 3.33 | 53.23 | 47.83 |

结论：

- 常规版速度都在 3.3-4.2ms，`v4.20-v4.23` 这代最慢（4.0-4.2ms），`v4.25/v4.26` 回到 3.1ms。
- lite 版 2.89-3.15ms，比同代常规版省 15-20%，在这段片子上 PSNR 不比常规版低——
  但 lite 是为低功耗/弱卡做的，复杂运动与遮挡下会掉，不能只看这一段动画。
- ensemble 稳定比同代高一档：成本 +2.5ms（1080p）/ +10ms（4K），增益只有 0.0-0.35dB，
  性价比低，除非追求最稳的边缘。
- `v4.26_heavy` 只比 v4.26 慢 0.3ms，PSNR 反而低 0.5dB；在这套 kernel 上没优势。

## 6. 限制

- 模型必须是 vs-mlrt 的 RIFE 图：输入 `Split{6,1,2,2}`（ensemble 为 `{3,3,1,2,2}`）、
  encode/block/lastconv 结构、`F∈{0,4,8,16}` 特征通道、每块 `K≤16`（ensemble ≤8）、
  最多 6 块、`Resize` 尺度为 2 的幂且不递增。不符合就报错拒绝，不会静默走别的后端。
- 需要 sm_80+（Ampere 起）。`__CUDA_ARCH__ < 800` 直接 `__trap()`。
- 宽高会 pad 到 64 的倍数（1920×1080 → 1920×1088），RIFE 特有的 4*s 整除要求由 pad 满足。
- 只支持整数 YUV；RGB/灰阶/浮点格式报错。
- 不写回 `_Duration`/时间戳，VS 里靠 `factor` 与下游 `AssumeFPS` 调整。

## 7. 源码阅读顺序

1. `native/rife_plan.cpp` — 怎么从 ONNX 认出 RIFE 的结构（`RifePlan`，块尺度、K、feeds）。
2. `native/rife.cpp` — 把 plan 变成 step 列表，scratch 复用、`phase_weights` 拆相位 conv、
   CUDA Graph 捕获。
3. `native/gconv.cuh` / `gconv.cu` — 通用 3×3 conv：ldmatrix + mma.sync、cp.async 载入、
   stride-2 去交错、split-K reduce、epilogue 融合 bias/残差/激活。
4. `native/rife_kernels.cu` — `k_gather`（一像素一线程，寄存器里做 up_vec/flow_sum/warp/mask）、
   `k_merge`、`k_final`。
5. `engine.cpp` → `plugin.cpp` — 插件这一侧的胶水。

## 8. 上机记录

**验证**（`vsrife/test/`）：

- `check_output.py`：真实 1080p 源，偶数帧与源帧逐位相同（`PASSTHROUGH_EXACT`），
  奇数帧对相邻帧均值 28-38dB（更接近均值而非前一帧，符合插帧）。
- `check_formats.py`：`YUV420P8/P10/444P8` × `factor 2/3/5`，直通帧全部 bit-exact。
- `check_scene.py`：硬切处 `sc=0` 输出混合帧（Y=82.6），`sc=0.05` 输出 anchor 帧（Y=16）。
- `bench.py` / `run_full.bat`：第 5 节的数字。

**坑 1（已修，花了最久）**：`createVideoFilter2(..., &d->vi, ..., d.release(), core)` 里
MSVC 按**从右往左**求值实参，`d.release()` 先把 `unique_ptr` 置空，再求 `&d->vi` 就成了
`nullptr + offsetof(RifeData, vi)`，`rife_plan`/VS 读 `vi.format` 时在 `0x10` 处 AV，
表现是 `vspipe` 静默 exit 1、无任何报错。修法：

```cpp
RifeData *raw = d.release();                 // 先接管指针
outNode = vsapi->createVideoFilter2("RifeNative", &raw->vi, rifeGetFrame, rifeFree,
                                    fmParallel, deps, 1, raw, core);
```

教训：任何"释放智能指针"与"取该成员的地址"同时出现在一个实参列表里的调用都会踩这个坑。

**坑 2**：`.bat` 里 `set CL=...` 会被 cl.exe 当附加参数读入（`D9024/D9027`），变量改名 `CLX`。

**已知小问题**：`vspipe` 结束时打印
`Warning: Core freed but 2 filter instance(s) still exist` 与约 113MB 未释放 framebuffer，
是脚本对象在 core 释放时仍持有节点引用所致，不影响输出；接在自己工程的 .vpy 里用 `clip.set_output()`
不会出现。
