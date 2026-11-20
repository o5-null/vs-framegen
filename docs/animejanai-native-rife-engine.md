# mpv-AnimeJaNai 的"自有 RIFE 引擎"到底做了什么

> 研究日期：2026-11-19　对象：`参考插件\mpv-AnimeJaNai` + 引擎源 `the-database/animejanai-inference`（libaji）
> 目的：搞清 3.12.0 release notes 里"RIFE interpolation up to 3x"是怎么做到的，以及哪些手法能搬到 vs-framegen。

## 0. 出处

`the-database/mpv-AnimeJaNai` release 3.12.0（2026-11-16）原文：

> On NVIDIA RTX 30, 40 and 50 GPUs the AnimeJaNai models now run on their own GPU kernels instead of TensorRT:
> upscaling is up to 1.8x faster, RIFE interpolation up to 3x, and there's no more waiting for a TensorRT
> engine to build, so a new profile starts playing in seconds.

| Speedup over TensorRT, 1080p | RTX 5090 | RTX 4090 | RTX A5000 (RTX 30) |
|---|---|---|---|
| 2x Performance（放大） | 1.7x | 1.7x | 1.8x |
| 2x Balanced（放大） | 1.6x | 1.4x | 1.15x |
| **RIFE interpolation** | **1.9x** | **3x** | **2.5x** |
| TemporalFix | 1.4x | 2.1x | 2.2x |

注意：本地 `参考插件\mpv-AnimeJaNai` 仓库里**没有引擎源码**，只有组装器（`BuildMpvUpscale2xAnimeJaNai/Program.cs`
下载并拼装 mpv fork + `aji.dll` + TensorRT/DirectML + RIFE 模型）。真正的引擎在两个兄弟仓库：
`the-database/mpv`（`vf_animejanai` 滤镜）与 `the-database/animejanai-inference`（libaji，本文件引用的
`src/native/*` 就在这里）。本次克隆在 `%TEMP%\opencode\aji-src`（AjiVersion v0.31.1）。

## 1. 它是什么：同一份 RIFE 权重，换执行器

不是新插帧算法。模型还是 vs-mlrt 布局的 RIFE v4.14 / v4.22 ONNX（`models-rife-fp16-2`，fp16 权重），
只是**不再交给 TensorRT 执行**，而是由 `src/native` 里手写的 CUDA kernel 直接跑：

- `native/rife_plan.cpp`：载入时读 ONNX 图，识别 vs-mlrt RIFE 的固定结构（`/encode/`、`/blockN/conv0/`、
  `convblock.K/{conv,Mul,Add,LeakyReLU}`、`lastconv`、`Resize`…），校验 shape 与连线，产出一份 plan。
- `native/rife.cpp`：按 plan 建 step 列表（GCONV / GATHER / MERGE / FINAL）、分配并复用 scratch、
  上传预打包权重，然后**把整个序列捕获成 CUDA Graph**，之后每插值帧一次 `cudaGraphLaunch`。
- `native/rife_kernels.cu`：gather / up / warp / merge / final 等点算子。
- `native/gconv.cu`：RIFE 用到的通用 fp16 tensor-core 3x3 卷积（与 SPAN/Compact 的专用 kernel 分开）。

布局不匹配（ensemble 某些组合、F>16、K>8、非 64 倍数等）会被拒绝并**自动回退 TensorRT**，日志写明原因
（`Native kernels not used for <model>: <why>; using TensorRT`）。`[global] native=off` 可整体关闭。
要求 sm_80+；AMD/Intel 与老卡仍走 DirectML / TensorRT。

## 2. 数字先摆出来

RIFE 单帧 GPU 时间（1080p，RTX 5090）：

| 路径 | 1080p ms/插值帧 | 备注 |
|---|---|---|
| TensorRT engine | ≈6（4K ≈25） | `BENCHMARKS.md` 的链时间分解，2026-06-17 |
| DirectML（ORT） | 10.6 | 经 `aji_infer_rife` 整帧 |
| CUDA native | **2.6–2.9** | 同一节记录 |
| Vulkan native（未发布） | 4.1（整帧 7.2） | `vk/rife`，输出对 CUDA 74.6dB |

整链（含放大）里 RIFE native 的收益就小了，因为放大才是大头：

| 场景（每源帧 ms） | TensorRT | native |
|---|---|---|
| 5090 1080p RIFE 2x after Performance | 19.68 | 18.03 |
| 5090 Linux 同上 | 19.01 | 17.55 |
| RTX 4090 同上 | 37.18 | 35.20 |
| RTX A5000 同上 | 62.85 | 58.23 |

精度：所有 case 输出与 TensorRT 差 1–2 个 8-bit 级（Y 58–65 dB）；native conv 单算子最差误差 0.0077
（阈值 0.02）。

## 3. TensorRT 跑 RIFE 为什么慢

RIFE v4（IFNet）的图是"少量小卷积 + 大量重采样"：

- 输入 11 通道（A 帧 3 + B 帧 3 + timestep 1 + meshgrid 2 + …），encoder 下采样出 F 个特征通道；
- 5 个 block（v4.14：scales 16/8/4/2/1，F=4），每块：几个 stride-2 conv + ConvTranspose + DepthToSpace 出
  4 通道 flow 与 mask，然后 **Resize(bilinear, half_pixel, 1/s)** 把上一级结果缩到本块尺度、
  **GridSample/warp** 把 A/B 与 encoder 特征按 flow 采样、再 Concat 上 timestep/mask/特征喂下一块；
- 最后把所有 block 上采样到全分辨率的 flow 相加，warp 两帧、按 sigmoid(mask) 混合。

在 ONNX/TensorRT 里这些是**每个 operator 一个 kernel + 一份中间张量**：half_pixel 的 Resize 是 4-tap gather，
GridSample 更贵；中间张量按全分辨率 materialize，带宽吃满；TensorRT 对这类 shape 的通用 kernel 效率一般，
且默认 fp32 累加。RIFE 单帧几十个小 kernel，launch 与同步开销占比也高。

## 4. 六个机制（速度从哪来）

### 4.1 载入期把图变成 plan，而不是运行时解释图

`rife_plan.cpp` 只认一种布局，全部 shape 与连线做断言，然后编译成固定 step 序列。好处：
没有 TRT engine 构建（首次播放不再等 trtexec，切 profile 秒开）；每个 step 的 shape 已知，可特化。
代价：模型换了布局就得回退 TRT。

### 4.2 融合 gather：一个 kernel 顶掉 warp + resize + concat + flow 求和

这是 RIFE 提速的核心。`rife_kernels.cu` 的 `k_gather<F,NM,KV,NT,B0,REV>` 对**每个输出像素**一次算完：

- 块 0 的输入就是 `[A, B, featA, featB, timestep]`，直接按块尺度 s 池化；
- 后续块的输入 `[warped A, warped B, warped features, timestep, mask, encoder features, flow]`
  在寄存器里累加，末尾按 8 通道 `uint4` 向量写 half；
- **下采样不是"先全分辨率算再缩"**：block 的输入按 s 的采样点直接求值。`up_taps` 说明：线性 half_pixel
  放大 u=(X+0.5)/s-0.5 夹到 0，4 taps；**s 为偶数时正好落在 2x2 块均值上**，4 taps 塌缩成 4 次加法；
  s>1 时取 4 个采样点各 0.25（`NT=2`）；
- flow 求和也在同一遍里：第 j 块输出通道 0–3 是 flow，乘本块尺度 s 后按块顺序累加
  （`flow_sum`），最后一块还能复用**已经上采样过**的 `lv`，省一次重采样；
- warp 用 `mesh + flow*2/(n-1)` 换成像素坐标，双线性取 3 通道图像 + NF 个 encoder 特征
  （`h8` 一次 `uint4` 载 8 个 half），边界夹紧；
- mask（可选 9 通道）、timestep 都在寄存器里，**全分辨率张量从不落地**。`rife.h` 的原话：
  "Unlike the graph, no block works at full resolution unless its scale is 1."

效果：ONNX 图里每块 6–8 个大张量算子，塌成一次 kernel。

### 4.3 专用 conv：fp16 累加 + 权重预打包 + 相位散布

`gconv.h` / `gconv.cu` 一个 kernel family 覆盖 RIFE 全部卷积：

- `mma.sync m16n8k16`，**fp16 累加**（fp32 累加实测比 TensorRT 还慢；深求和需要时用 `acc32` 单独打开）；
- 输入 tile 带 halo 放 shared memory；K 按 16/32/64 通道切块，所以任意输入宽度都能跑；
  输出通道每次 launch 最多 64；
- 输入/输出/残差都带**运行时像素 stride**，所以一个 conv 可以"读宽 buffer 的通道前缀、写另一 buffer 的
  通道切片"——**concat 不花钱**（RIFE 块之间正好一直 concat）；
- epilogue 折叠 bias、`v += c1*r1 + c2*r2`（残差）、ReLU/LeakyReLU；
- `up2`：需要 2x 最近邻上采样时在载入处就地做，不产生中间张量；
- `R/sy/sx` 相位散布：配合重排权重，把 **ConvTranspose + DepthToSpace 变成 R 个普通 3x3 conv**，
  直接写 `dst[R*y+sy][R*x+sx][c]`，跳过反卷积和 depth-to-space 两个算子；
- 权重在主机侧预打包成 B-fragment 顺序（`uint4[9][cin_pad/16][coutp/16][32]`），运行时无需 swizzle。

### 4.4 用 fp16 舍入复现图的行为，给"降精度换速度"兜底

`lerp4` 每做一次插值就 `rh(v) = __half2float(__float2half(v))`，把图里该是 fp16 的位置重新舍入一遍；
mesh / multiplier / flow 全部按 fp16 舍入（注释明确写了这一点）。所以即使 fp16 累加、即使把
Resize/GridSample 换成手写点算子，输出仍能落在 TensorRT 的 1–2 个 8-bit 级内。
这不是"精度无所谓"，而是**把误差预算算清楚后用相同舍入复现**。

### 4.5 零主机往返：帧从解码器直接进模型，出来就是 YUV

`aji_trt.cpp:2903-3138`（`aji_infer_rife_with_scene`）每帧只做：

1. NV12/P010/444 帧按 64 倍数 pad（studio black 边框，只填一次）；
2. `aji_run_pre` 一次 kernel：YUV→RGB(BT.709)，**直接写进输入张量的通道 0–2 与 3–5**（fp16 NCHW）；
3. timestep 一个 fill；native 路径连 plane 6 都不读（`t` 作为标量传给 kernel），mesh 在 gather 里现算；
4. `R.native->run(...)` → 整个 RIFE 序列是一条已捕获的 CUDA Graph；
5. `aji_run_post` 写回 padded YUV，再裁掉边框。

没有 Python、没有 numpy、没有逐 plane 上传、没有回读（唯一的同步是场景检测的 4 字节 D2H）。
scratch 在 create 期一次分配、所有 block 共享（encoder 的 ping-pong 直接用 block 输入 scratch）。

### 4.6 在真卡上实测挑 tile variant

`rife.cpp` create 期对每个 conv 用真实 GPU 跑候选 tile variant（`gpick`），选最快的；
每层 variant 记录在 step 里。这是为什么同一份二进制在 5090 / 4090 / A5000 上都能拿到收益，
而不是只对开发卡调优（反例也在 `BENCHMARKS.md`：64 通道模型在 Ada 上收益明显小于 Blackwell，
因为 variant 是在 sm_120 上选的）。

## 5. 引擎之外、但常被一起算进"大量提升"的

这些不是 kernel 优化，但对实际播放/编码的墙钟时间影响更大：

| 手法 | 效果 | 说明 |
|---|---|---|
| RIFE-first（先 1080p 插帧，再放大） | 31.2 → 21.7 ms/源帧（1.4x） | 成本由 RIFE 主导：RIFE 4K ~25ms、1080p ~6ms；1013 槽已是默认 |
| held-frame reuse | 8%/31% 静态帧 → 7.3%/28.0% 墙钟更快；最多省 ~40% GPU 功耗 | 静态帧跳过推理，直接重发 anchor 输出；RIFE 链需 `--reuse-held-frames-rife` |
| depth-3 流水线（帧在飞） | TRT +6~34%，DML +35~36% | DML 原先每帧 CPU fence 等待把 ORT 派发串行化；framemd5 逐位一致 |
| fp16 模型（vs fp32） | DML 1080p 13.8→11.1；4K 55.4→41.0 | 外加 models-rife-fp16-2 给 DirectML 固定 H/W，再快 ~10% |
| 场景检测 / enhanced SCD | 跨镜不插帧（质量 + 省算） | 在未 pad 的 luma 上求平均差，阈值默认 0.150；`misc.SCDetect` 口径 |

## 6. 对 vs-framegen 的可迁移点

现状对比（本仓库 `bench`，RTX 3080）：RIFE 4.6 1080p **16.20 ms/pair**，4K 69.97；计时口径为
"numpy→tensor→device 拷贝 + `execute_async_v3` + sync"，不含回读。按上面拆解，可逐项对账：

1. **先量化主机侧占比**。`trt_runner` 的 tween 每次上传 7 个会变的 plane（A 3ch、B 3ch、t），
   1080p fp16 ≈ 25 MB/pair。跑 `ms_gpu` 与总时间的差值就能知道这块有多大；aji 的做法是
   在 device 上从 NV12 直接生成 11 通道输入，**一次 H2D 都不做**。
2. **RIFE 的主要成本在 Resize/GridSample，不在 conv**。TensorRT/vs-mlrt 无法跨算子融合这部分；
   要拿到 2–3x 级提升只能自写（或至少手工融合）"warp + resize + concat + flow 累加"的 gather，
   并让块输入按尺度直接求值（s 为偶数 → 2x2 均值，4 taps 塌缩）。
3. **ConvTranspose + DepthToSpace 用相位散布变成普通 conv**，省两个算子与一份中间张量；
   权重在载入时重排，运行期零 swizzle。
4. **fp16 累加可行**（误差 0.0077 / 阈值 0.02），但必须把图里该舍入的位置按 half 复现，
   否则和参考输出的差会放大到肉眼可见。
5. **整个 RIFE 序列做成 CUDA Graph**：RIFE 是几十个小 kernel，比大 conv 链更吃 launch 开销。
6. **held-frame / 静态帧跳过**：动画素材 8–31% 的帧是静止的；这部分收益与 kernel 无关，
   但直接决定 4K 播放的功耗和掉帧。
7. **顺序**：如果同时做放大，先插帧后放大（4K RIFE 是 1080p 的 ~4x 成本）。

风险与边界：native 只保证 vs-mlrt RIFE 布局（ensemble 不许 feeds、K≤8）；尺寸必须 64 倍数；
需要 sm_80+；不匹配自动回退 TRT。RIFE 的 3x 是"相对 TensorRT 在同卡上"的数字，
不是相对 fp32/ORT 的数字；RTX 30 上放大只有 1.15x（64 通道在 Ampere 是算力受限），
但 RIFE 仍 2.5x——因为 RIFE 更吃重采样而非纯算力。

## 7. 阅读顺序（源码）

| 文件 | 看什么 |
|---|---|
| `src/native/rife.h` | 设计边界与"每块只在本块尺度工作"的说明 |
| `src/native/rife_plan.cpp` | ONNX 结构识别与校验、block 尺度/特征通道/feeds 规则 |
| `src/native/rife.cpp` | step 列表、scratch 复用、tile variant 选择、CUDA Graph 捕获/回退 |
| `src/native/rife_kernels.cu` | `k_gather` / `up_*` / `flow_sum` / `warp` / `k_merge` / `k_final`，fp16 舍入 |
| `src/native/gconv.h`、`gconv.cu` | fp16 累加 mma、K 分块、任意 stride、`up2`、相位散布 |
| `src/aji_trt.cpp:2903-3138` | `aji_infer_rife_with_scene`：pad → pre → native/TRT → post → crop |
| `BENCHMARKS.md` | 全部数字与口径（含 RIFE-first、held-frame、三张卡的 native vs TRT） |
| `CLAUDE.md:159-271, 389-400, 939-981` | native 融合策略、Vulkan RIFE、held-frame reuse 细节 |

## 8. Kernel 级实现：一帧 RIFE 到底跑什么

所谓"原生内核"不是一个 kernel，而是一个 **模型专用的 runtime**：载入期把 ONNX 图编译成几十个
step（形状特化、邻接算子已融合的 kernel），运行期整段用 CUDA Graph 重放。下面按
`src/native/rife.cpp:90-249` 的构建顺序展开。

### 8.1 一帧的 step 序列

**encoder，跑两遍（A、B 各一次）**

1. 首个 conv 直接读 NCHW 帧平面：`a.nchw = true, stride = 2`（输入步长 2 就是 encoder 的
   空间降采样，不再单独做 downscale），输出 `H/2 × W/2`；KCH 固定 16，只支持 3 通道，越界补零。
   `src` 在 run 时才 patch 成当帧指针。
2. 中间 7 个 `16ch 3×3 + ReLU`，权重是预打包的 GWeights。
3. **最后一个 encoder conv 被拆成 2 个行相位 conv**：`phase_weights(last, R=2, sy, FP)`，
   直接写 `feat[k]`（stride `FP = pad8(F)`）。这一刀同时吃掉 ConvTranspose 与 DepthToSpace，
   输出一步到位落进后面每块都要读的特征缓冲。

ping-pong 缓冲 `hb0/hb1` 在容量足够时直接 alias 到 `sX`（即复用 block 输入 scratch）。

**每个 block b（块输出 `bw × bh`，尺度 `s`）**

| # | step | 说明 |
|---|---|---|
| 1 | `k_gather` | `g.nprev=b`、`g.prev[]` 指向前面各块的输出 `O`、`feeds_feat`、`prevK`、`s`、`dst=X`、`dstride=cinp` |
| 2 | `conv0a` | `X → T1`，stride 2，LeakyReLU |
| 3 | `conv0b` | `T1 → A`，stride 2，LeakyReLU |
| 4 | `K` 个 residual conv | `cur → oth`，`r1=cur, c1=1`，beta 折进权重与 bias（`bb[o] = res[k].b[o]*beta[k][o]`），LeakyReLU，然后 swap |
| 5 | 4 个 lastconv 行相位 conv | `phase_weights(last, R=4, sy, KP)`，输出相位散布进 `O` |
| 6 | `k_merge` | 仅 ensemble：flow 前后半平均、mask 相减 |

`KP = K <= 8 ? 8 : 16`；`O` 必须留存，后面的块求和 flow 与最后的 FINAL 都要读它。

**FINAL**：对每个像素求全块 flow 和 → warp A/B 两帧 → mask 过 sigmoid
（`rh(1/(1+expf(-m)))`）加权混合，写 half 三平面。

**scratch 预算**（一次分配，全 block 共用）：

- `needX  = max(px * pad16(c0a.cin))`，`px = W/s × H/s`
- `needT1 = max(px/4 * pad16(c0a.cout))`
- `needA  = max(px/16 * pad16(c0b.cout))`

即峰值由最大尺度那块决定，而不是所有块相加。

### 8.2 `k_gconv`（`src/native/gconv.cuh`）

- **布局**：一个 CTA 算 `WARPS` 行 × 16 列输出像素、`COUTP` 个输出通道；每个 warp 一行 16 像素
  = 一个 m16 tile；`blockIdx.z = 输出 chunk * ksplit + K split`。
- **共享内存布局**：输入 tile 连同 halo 一次拷一个 K chunk（KCH 通道），像素 stride = `KCH + 8`
  halfs（给 ldmatrix 留出无冲突的 bank 分布）。stride-2 的 tile 按列奇偶**去交错存放**
  （先偶列后奇列），这样每个 tap 的 A fragment 16 行在 smem 里仍然是连续像素。
  halo 尺寸 S==1 时 `(WARPS+2)×(TW+2)`，S==2 时 `(2*WARPS+1)×(2*TW+1)`；
  smem 取 halo 与 stage（`WARPS*TW*(COUTP+8)*2`）的较大者。
- **权重**：host 侧已打包成 B-fragment 顺序，运行期直接
  `__ldg(wt + ((t*p.kct + kk)*NJ + j)*32 + lane)`（uint4），零 swizzle。
- **载入**：`cp.async.cg.shared.global ...,16,bytes`；越界时 `bytes = 0` 由 cp.async 补零，
  不用分支；每个 chunk 一次 `commit_group` + `wait_all`（单缓冲，不用 double buffer——
  整个 RIFE 是几十个小 launch，隐藏延迟靠 Graph 与 occupancy，不靠预取）。
- **计算**：`ldmatrix.sync.aligned.m8n8.x4.shared.b16` 取 A 四寄存器 →
  `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`（fp16 累加，全速率；
  `.f32.f16.f16.f32` 是 acc32 变体，GeForce 上半速率）。累加器在寄存器 `acc[NT][2]`，
  tap 循环 `t∈[0,9) × kc∈[0,KCK)`。
- **epilogue `g_epi`**：bias、`v += c1*r1 + c2*r2`（half2 hfma）、激活 None/ReLU/LeakyReLU/GELU
  （精确 erf，除数 1.4140625）。
- **输出**：先写 smem staging（stride `COUTP+8`）再 `uint4` 向量写 global；
  `OUT==1` 时相位散布 `dst[(R*yy+sy)*OW + R*xx + sx][ch]`，`CHP = coutp/R` 保证是 8 的倍数，
  所以向量写不会跨相位。
- **split-K**：小图时 9 taps × K chunks 分给 `ksplit` 个 CTA，各写 fp32 部分和到 workspace，
  `k_gconv_reduce` 汇总后再做 bias + epilogue。
- **`up2`**：需要 2× 最近邻上采样时在载入处就地映射 `sp = (yy>>1)*(IW>>1) + (xx>>1)`，
  不生成中间张量。
- **`SUMS` 必须是独立实例化**：注释写明把"求通道和"塞进通用模板会改变所有其它实例的寄存器分配，
  在 sm_89 上让 ESRGAN 的 conv 慢约 20%。
- 门槛：`__CUDA_ARCH__ >= 800`，否则 `__trap()`。

### 8.3 `k_gather`（`src/native/rife_kernels.cu`）

一个线程负责**一个输出像素**，把该像素需要的整条输入向量攒在寄存器的 `float acc[CP8]` 里，
`CP8 = (CIN + 7)/8*8`，`CIN = 6 + 2F + 1`（RGB×2 + 两帧特征 + timestep），
非块 0 再加 `NM`（mask）+ 4（flow）。写回时每 8 通道一个 `uint4`，尾部补零。

- **采样点**：`NT==1`（s=1）只算 `(x, y)`；`NT==2` 算 4 个点
  `X = s*x + s/2 - 1 + tx`，权重各 0.25——这正是 s 为偶数时"2×2 块均值"的直接实现。
- **块 0（`B0`）**：直接读 `[A, B, featA, featB, t]`，`acc[c] += wt * half(...)`；
  `REV`（ensemble 反向 pass）用 `(c + 3*A0) % 6` 交换 A/B，特征取 `feat[k ^ A0]`，
  timestep 取 `rh(1 - rh(t))`。
- **其他块**：`up_vec<KV>` 按尺度从上一块输出 `O` 采样 → `flow_sum` 把所有更早块的 flow
  累加起来（最后一块若已上采样则直接乘 `s`，省一次重采样）→ `warp<F>` 两次（A、B）：
  现算 mesh `mx = rh(2X/(W-1) - 1)`、`gx = mx + rh(fx)*mw`，双线性 4 taps，
  边界夹紧（`x1 = x0+1 < W ? x0+1 : W-1`），RGB 走三平面、特征用 `h8()` 一次读 8 个 half。
  然后 RGB、特征、timestep、mask（`REV` 时首通道取负）、`flow/s` 依次累加。
- **每次插值后 `rh()`**，把图里该是 fp16 的位置重新舍入一遍——这就是 fp16 累加还能对齐
  TensorRT 的原因。
- 只对 `(F ∈ {0,4,8,16}, (NM,KV) 组合, NT=1/2, B0, REV)` 生成特化版本；组合不支持就
  `rife_gather_supported` 拒绝整个模型、回退 TensorRT——**宁可不用，也不跑通用慢路径**。

一句话概括实现风格：**把"通用算子 + 全分辨率中间张量"换成"形状特化 + 融合 + 寄存器累加"，
再用"复现 fp16 舍入"把精度风险关掉，用"真卡实测选 variant"把跨卡性能风险关掉。**

