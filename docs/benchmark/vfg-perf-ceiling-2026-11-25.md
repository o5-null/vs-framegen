# VFG（vvfg）性能上限与三条被否掉的加速路线 — 2026-11-25

本文记录这轮把 VFG 从「原样 fixed high 档」推到当前形态的全部量测：先做画质/性能档位与宿主侧打包，
再把三条结构性加速路线逐一试过并否掉。结论是当前即平台期，可用的旋钮只有 `mode=` 与 `multi`。

## 环境与口径

| 项 | 值 |
| --- | --- |
| GPU | RTX 3080 10GB（sm_86，走插件内的 sm_89 解锁） |
| 插件 | `vs-dlssg/bin/vvfg.dll`（commit `b763bd5` 构建） |
| 引擎 | `vs-dlssg/bin/vfg/`（nvidia video effects 1.x 包，`nvVFXVideoFrameGeneration.dll` 207 MB） |
| 1080p 源 | `media/sintel_trailer_1080p.mp4`，Trim 90..113（24 源帧） |
| 4K 源 | `测试数据/阿哈角色PV 4k.mkv`（3840×1632，30 fps），Trim 300..323 |
| 宿主 | VapourSynth R74（`.venv`），ffms2 → `resize.Spline36(format=vs.RGBS, matrix_in_s="709")` |
| 计时 | 每个输出帧的 wall（ms/输出帧），预热 `min(multi*2, 8)` 帧后取中位数；`VFG_TIMING=1` 打印分段 |

**这台机器不是空载**：mpv（playkit dist）、MuMuPlayer、wallpaper_engine、Chrome、dandanplay、QQ 常驻，
空载 `nvidia-smi` util 39–41%、1905 MHz、154 W。同一配置三次复测 high 档能出现 94 / 37 / 30 ms，
所以本文只采信「同一时段内连跑、且差距 2 倍以上」的比较；单次 10% 以内一律当噪声。

## 一、模型档位 `mode=`

DLL 的公开参数里 `Mode` 是三档（`low (0u), medium (1u), high (2u)`），包内带 `model_medium_*` 与
`model_small_*` 两套权重（后者另有一套 `_fp8` 变体）。插件原先写死 high，现在暴露成滤镜参数；
**2026-11-25 夜起默认 1（medium）**，与引擎自己的默认档一致，理由见第四节。

| multi（1080p） | high `mode=2` | medium `mode=1` | low `mode=0` | DLSS-G 同口径 |
| --- | --- | --- | --- | --- |
| 2 | 14.8 ms | 6.3 ms | 5.7 ms | 12.4 ms |
| 4 | 18.8 ms | 5.9 ms | 4.4 ms | — |
| 6 | 20.2 ms | 6.7 ms | 4.5 ms | 5.7 ms |

4K（3840×1632，multi=2）每输出帧：high 51.9 / medium 15.5 / low 12.9 ms。
即 medium 在 1080p 2×/4× 下比 DLSS-G 还快近一倍，4K 下与 DLSS-G 同量级。

质量（PV 三个真实中间帧 f01 / f02(GT) / f03，PSNR / SSIM / 锐度与 GT 之比，三帧均值）：

| 档 | PSNR | SSIM | 锐度/GT |
| --- | --- | --- | --- |
| high | 33.91 | 0.9765 | 0.512 |
| medium | 33.52 | 0.9740 | 0.407 |
| low | 32.33 | 0.9692 | 0.427 |

挑选档位要按「软不软」权衡，而不是只看 PSNR：VFG 本来就是几家里最软的（`docs/benchmark.md` 里锐度/GT 0.652），
medium 的代价基本都在柔化上。裁切并排图（GT/high/medium/low）三档都合理。

## 二、分段耗时：瓶颈到底在哪

1080p multi=6，python 串行请求，机器较静时（`VFG_TIMING=1`）：

| 档 | 打包 up | `NvVFX_Run` | 等待 wait | 解包 unpack | 合计 | 端到端 |
| --- | --- | --- | --- | --- | --- | --- |
| high | 0.51 | 3.94 | 17.84 | 2.63 | 24.95 ms | 20.19–20.25 |
| medium | 0.47 | 0.47 | 3.41 | 2.52 | 6.90 ms | 6.71–6.73 |

`NvVFX_Run` 只入队，真正等模型的是 `download` 里的 `streamSynchronize`，所以 `wait` 里含
H2D + 模型 + DtoH。high 档 71% 是模型计算（GPU util 70–78%）；medium 档 3.4 ms 几乎全是
每帧 16 MB 的 PCIe 往返（8 MB 上传 + 8 MB 回读），模型自身只要 0.5 ms。

## 三、建 lane 为什么中低档慢 4 倍

`VFG_TIMING=1` 下 `createLane` 按五段打印，1080p：

| 档 | context | pinned | create | **model load** | surfaces | 合计 |
| --- | --- | --- | --- | --- | --- | --- |
| high | 65–124 | 104–129 | 271–342 | **+409** | — | 751 ms |
| medium | 同 | 同 | 同 | **+1947** | — | 2269 ms |
| low | 同 | 同 | 同 | **+1527** | — | 1798 ms |

差别全在 `NvVFX_Load` 一步。把 `CUDA_CACHE_PATH` 指到空目录再跑，跑完目录里 0 个文件，
所以不是 PTX JIT 缓存在重建，是引擎自己在装载中低档权重时慢约 4 倍。这是 `multi=6` 首帧尖峰
从约 2.5 s 涨到约 10 s 的原因，插件侧无法优化（除非换引擎版本）。

## 四、三条被否掉的加速路线

目标都是「把宿主侧的打包/解包藏到别的 lane 的 GPU 时间后面」。

### 4.1 lane 并发（去全局锁）

做法：把 `VfgSession::generate` 的会话级互斥锁删掉，改成 per-lane 锁；`packed`/`hostOut` 从共享
改为 per-lane（这一点必须做，否则异步 H2D 读到被覆盖的暂存——第一版每 lane 只给一个缓冲时，
首个 tween 的 frame1 与旧构建差 max 49 / mean 1.09，改成两槽交替才逐字节一致）；`parallelRows`
的自由函数改成池化的 `RowPool`，避免 5 条 lane 各起 16 线程互抢。

结果：功能与画质完全正确（逐帧 md5 与串行构建全同：1080p 2× 46 帧、6× 138 帧、YUV420P10 46 帧、
4K 6 帧），但**收益为 0**。在 `generate` 里数在途调用，python 多线程与 `vspipe -r 6` 下峰值恒为 1。

根因：`frame_gen.cpp` 用 `fmUnordered` 注册（VapourSynth 注释：每请求都改内部状态的滤镜，
同一实例一次只调一帧）。对照实验证明 core 本身能并发（纯滤镜链 `vspipe -r 1` 19.1 fps vs `-r 6` 68.2 fps），
是这个插件的派发模式决定的。已回退。

### 4.2 改 `fmParallelRequests` 强开并发

把 `createVideoFilter2` 的派发模式改成 `fmParallelRequests` 后 core 会同时持有多个在途输出帧，
10 GB 卡的 5 条 lane（各约 1.4 GB）直接进 WDDM 分页：

- 1080p 6× medium，python 串行：130 帧 91.75 s（705 ms/输出帧）；
- 同配置 5 线程：130 帧 274 s（2108 ms/输出帧）；
- vspipe 1200 帧：`-r 1` 130.94 s、`-r 6` 40.00 s。

用户侧表现为「显存占用爆了所以卡顿」。已改回 `fmUnordered`，并在 `frame_gen.cpp` 的注册处留下注释
记录这次实验（那里是最容易被下一个「想加速」的人踩第二次的地方）。

### 4.3 整对下发 / 两帧流水

> **2026-11-26 更正**：这一节的结论只对「整对全发、一次交付」成立。改成「serve i 时 sync i →
> issue i+1 → unpack i」的流水形态后，medium 拿到 26%（1080p 5.64 → 4.18 ms），见 §7。
> 下面留作对照记录。

两种形态都在 `Impl` 里临时实现过（`issue()` / `serve()` / `drain()`，把 `download` 拆成只解包）：

1. 整对全发：一次把 5 个 in-between 全排进各自 lane，再逐个回读解包。high multi=6 24.22 / 25.11 /
   24.85 / 24.96 ms（4 次很稳）vs 基线 22.24 / 23.45 —— 慢 8–10%；medium 在噪声内。
2. 两帧流水（kWindow=2）：serve 完一帧再补发下一帧。high 中位 22.73（5 次 22.58–22.90）vs 基线 20.50；
   medium 6.03 vs 6.09。同样没有收益。

原因：high 档 GPU 本来就饱和，多个 context 同时跑会切碎 SM 占用与缓存局部性，损失大于 CPU 解包
重叠的收益；medium 档的 `wait` 主要是每条 lane 各自重传同一个源帧（每对 10 次 8 MB H2D 走同一条
PCIe），重叠省不下带宽。

「让 5 个 in-between 共享一个 CUDA context 以省掉源帧重复上传」也不成立：模型的「上一帧」状态
记在 CUDA context 上而不是 effect 上（同一 context 里第二个 effect 仍走 pass-through），
这正是每 lane 一个 context 的设计原因。

## 五、当前形态与剩余空间

已落地（本地 commit `2156189`、`b763bd5`，未 push）：

- `mode=0|1|2` 滤镜参数（默认 2 → **1，2026-11-25 夜**），其它引擎给了会报错而不是静默忽略；`VFG_MODE` 环境变量仍可覆盖。
- 打包/解包改 AVX2 八像素并行（打包 1080p 2.7→2.1 ms、4K 6.5→4.4 ms），`upload` 去掉多余的
  `streamSynchronize`（H2D 与 `NvVFX_Run` 同 stream，顺序天然保证，join 放在 `download`）。
- `VFG_TIMING=1` 的分段诊断：建 lane 五段、`up`/`run`、`wait`/`unpack`；`VFG_DIAG=1` 再叠一遍解包
  用来区分冷页与内核成本。
- 源帧跨 lane 共享（托管内存，见 6.1）：1080p 每对上传 5×8 MB → 1×8 MB，显存 80 → 16 MB。

剩余空间（插件侧做不到，若要再往前必须先改引擎或换路线）：

- medium 档若能把「每对 10 次 8 MB 源帧上传」合并成 2 次，还能省 1–2 ms/输出帧（约 20%）——
  需要引擎支持跨 context 的源帧共享或 D2D 拷贝。（2026-11-25 夜：已用托管内存做到，实测约 8%。）
- 中低档建 lane 的 1.5–2 s 全在 `NvVFX_Load` 内，只能等引擎修。
- 双卡分 lane（每卡一组 context + 自己的一份上传）能把 4K 6× 从「不可行」变成可行，
  但不会让 1080p 单卡更快。
- 唯一还开着的大头：把 CPU 解包与 GPU 重叠（理论上限 1.6× @1080p、~1.9× @4K）——**已落地，见 §7**。

## 六、2026-11-25 夜：默认 medium、显存账本、真实瓶颈

### 6.1 源帧跨 lane 共享：IPC 不行，托管内存可以

- CUDA IPC 在 Windows/WDDM 上不可用：`cuIpcGetMemHandle` 成功，但 `cuIpcOpenMemHandle` flags=0 →
  `1 CUDA_ERROR_INVALID_VALUE`、flags=1 → `201 CUDA_ERROR_INVALID_CONTEXT`，在导出方自己的 context
  里打开也是 1；`cuCtxEnablePeerAccess` → 217。
- `cuMemAllocManaged(..., CU_MEM_ATTACH_GLOBAL)` 的指针没有 context 归属，UVA 下任意 context 可寻址
  （普通 `cuMemAlloc` 的指针跨 context `cuMemsetD8` 返回 1）。
- 实现：lane 0 分配两槽托管内存，所有 lane 的 `SrcImage0/1` 指向它；`upload` 在上传后立刻
  `streamSynchronize`（别的 context 读，本流顺序无意义），`source` 用 `sharedIndex[2]` 记录槽位。
- 收益：medium multi=6 每对少 32 MB H2D ≈ 0.6 ms/输出帧（8%）；high 档被模型计算掩盖；multi=2 无冗余。
- 验证：除 lane 0 外所有 lane `up` = 0.00；1080p 2×/6×、YUV420P10、4K 四组 md5 与共享前一致
  （同时证明引擎只读 `SrcImage*`）。

### 6.2 现在的账本：GPU 与解包完全串行

medium，python 串行，机器较静，每 tween：

| 场景 | up | `NvVFX_Run` | wait | unpack | 端到端 |
| --- | --- | --- | --- | --- | --- |
| 1080p multi=6 | 0.48 | 0.36 | 2.90 | 2.13 | 5.3 ms |
| 1080p multi=4 | 0.78 | 0.35 | 2.96 | 2.09 | 5.3 ms |
| 1080p multi=2 | 2.22 | 0.35 | 3.10 | 2.13 | 5.5 ms |
| 4K multi=4 | 1.77 | 0.39 | 5.06 | 5.45 | 11.6 ms |
| 4K multi=2 | 5.06 | 0.35 | 5.00 | 5.34 | 12.6 ms |

GPU 约 3.0 ms（模型 ~2.3 + D2H ~0.7），CPU 解包 1.9 ms 与之完全串行，SM 只有 44%；
4K 的解包 5.4 ms 已与 wait 持平。解包内核已接近带宽上限：微基准（1920×1080，10-bit→float×3）
现有 AVX2 内核 1/4/8/16 线程 = 0.90/0.72/0.82/1.01 ms，`_mm256_stream_ps` 版本反而更慢
（1.19/1.06/1.01/1.21）。`VFG_DIAG=1` 二遍解包：unpack 1.94 / repeat 1.65 ⇒ 冷页只占 ~0.3 ms。

⇒ 理论重叠上限 `max(3.0, 1.9)`：5.3 → ~3.3 ms/输出帧（1.6×），4K ~1.9×。按帧返回的 API
（`fmUnordered`）拿不到跨调用重叠，入口仍是整对批量化；4.3 当时测成负收益是在忙机器上取的，
需要在静机器上重测才能定论。

### 6.3 显存账本与默认档

每 lane 一个 `cuCtxCreate` context，`NvVFX_Load` 在 context 内整份加载权重：

| 分辨率 | high | medium | low |
| --- | --- | --- | --- |
| 480×270 | 622 | 238 | 216 MiB |
| 720p | 1310 | — | — |
| 1080p | 1406 | 714 | 354 MiB |
| 1440p | 1526 | — | — |
| 2160p | 1828 | — | — |
| 3840×1632 | 1990 | — | — |

固定开销（权重+context）约 0.6 GB（high）/ 0.2 GB（medium/low）；high 随分辨率强烈次线性。
5 条 lane：1080p high ≈7 GB、4K ≈10 GB；medium ≈3.5 GB。`nvidia-smi` 采样（multi=6，1080p）：
high 18.3–19.1 ms / SM 83–84% / 7.7 GB / 302 W；medium 5.5–5.7 / 44–49% / 4.3 GB / 131–177 W；
low 4.3–4.4 / 32% / 2.4 GB / 97–100 W。medium/low 明确不是 GPU 受限 —— 默认档因此定在 medium。
官方文档补充：batch size 只能 1；不提供显存数字；明确「unsupported on RTX 2060 and RTX 3080」。

## 七、2026-11-26：流水式批量拿到 26%，并摸到 host 地板

§6.2 量出的「解包与 GPU 完全串行」现在被消掉了。落地形态（`Impl::batch`，默认开，`VFG_BATCH=0` 关闭，
一对只有 1 个 in-between 时走直通）：

- 一对的第一次调用：把这一对的所有 in-between 各发到自己的 lane（`issue(i)` = `NvVFX_Run` + D2H 到该帧
  的 pinned 槽）；之后每次 serve i 是 **sync i → issue i+1 → unpack i**。
- 与 §4.3 的「整对全发」的区别就在这里：全发是 5 条 lane 同时入队、同时完成，GPU 算完才轮到串行解包；
  流水式则让 unpack(i)（2.1 ms 纯 CPU）与 GPU(i+1)（~3 ms）真正并行。

| 场景（medium） | 旧形态 | 流水批量 | 变化 |
| --- | --- | --- | --- |
| 1080p multi=6 | 5.64 | 4.18 | −26% |
| 1080p multi=4 | 6.05 | 4.49 | −26% |
| 4K multi=4 | 12.45 | 10.74 | −14% |
| 1080p multi=6（high 档） | 17.81 | 16.64 | −6.6% |

GPU 被其它进程压着时绝对值上浮（1080p medium multi=6 7.16 → 5.32），比例仍是 −26%。

### 7.1 账对不上是因为 host，不是插件

4K 每对账内 ~27.9 ms，端到端却是 42.8（10.7×4）。成因用空滤镜量出来：`core.std.BlankClip(RGBS)` 纯
VS 每帧成本 **3840×1632 = 3.525 ms、1920×1080 = 1.146 ms**（≈0.55 ms/Mpx），与缺口逐毫秒吻合。
所以插件真实占用：1080p medium ~2.6 ms/帧（端到端 4.18 里 1.15 是 VS 地板）、4K ~6.9 ms/帧
（端到端 10.7 里 3.5 是 VS 地板）。`core.max_cache_size` 加大没用（600 MB 时反而 32.7 ms/帧）。

### 7.2 现在的结论

- medium/low 已经贴着「GPU 算一帧」的下限，插件侧没有下一步；剩下的大头是 VS 宿主地板。
- high 档 16.6 ms 里 ~14 是模型本身，解包与宿主都已藏住 ⇒ 旧的「平台期」判断只对 high 成立。
- 代价：pinned 槽每对最多 7 × frameBytes（1080p 5×16 MB、4K 5×75 MB）；一次请求一对里的全部
  in-between（取单帧会多算几次）。
- 验证：默认流水路径与旧基线逐帧 md5 一致（1080p 2×/6×、YUV420P10、4K），`VFG_BATCH=0` 亦一致。

## 复现

```bash
# 档位吞吐（1080p，每档每 multi 两遍）
C:\Users\benben\AppData\Local\Temp\opencode\bench_all.bat <tag>

# 中位数（更可靠，连跑 N 次）
python C:\Users\benben\AppData\Local\Temp\opencode\rep.py <multi> <mode> <runs>

# 分段
set VFG_TIMING=1 && python vs-dlssg\test\engine_bench.py vfg 6

# 建 lane 分步 / 4K
C:\Users\benben\AppData\Local\Temp\opencode\load_probe.bat <mode>
C:\Users\benben\AppData\Local\Temp\opencode\load4k.bat <mode>
```

坑：同一进程里重复跑同一个帧对会走 pass-through（模型的新鲜度规则），任何档位/质量对比都必须
每条用例一个进程；`.bat` 里不要直接写中文路径（cmd 代码页会把解释器名字弄坏）。
