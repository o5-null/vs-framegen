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
`model_small_*` 两套权重（后者另有一套 `_fp8` 变体）。插件原先写死 high，现在暴露成滤镜参数，
默认仍是各引擎里最保守的 2。

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

- `mode=0|1|2` 滤镜参数（默认 2），其它引擎给了会报错而不是静默忽略；`VFG_MODE` 环境变量仍可覆盖。
- 打包/解包改 AVX2 八像素并行（打包 1080p 2.7→2.1 ms、4K 6.5→4.4 ms），`upload` 去掉多余的
  `streamSynchronize`（H2D 与 `NvVFX_Run` 同 stream，顺序天然保证，join 放在 `download`）。
- `VFG_TIMING=1` 的分段诊断：建 lane 五段、`up`/`run`、`wait`/`unpack`。

剩余空间（插件侧做不到，若要再往前必须先改引擎或换路线）：

- medium 档若能把「每对 10 次 8 MB 源帧上传」合并成 2 次，还能省 1–2 ms/输出帧（约 20%）——
  需要引擎支持跨 context 的源帧共享或 D2D 拷贝。
- 中低档建 lane 的 1.5–2 s 全在 `NvVFX_Load` 内，只能等引擎修。
- 双卡分 lane（每卡一组 context + 自己的一份上传）能把 4K 6× 从「不可行」变成可行，
  但不会让 1080p 单卡更快。

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
