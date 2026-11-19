# 星铁 PV 上的 6× 实测：速度、画质与切镜阈值

素材：`测试数据\《崩坏：星穹铁道》星神★阿哈角色PV——「让骰子飞」 [rBgIpbQ4EOY].mkv`
（3840×1632、5683 帧、30 fps、189.4 s，H.264 + YUV420；测色统一 `resize.Bicubic(format=vs.RGBS, matrix_in_s="709")`）

插件：`plugin\vdlssg.cpp` + `dist\engine45`（DLSS 4.5 引擎包，`MaxGeneratedFrames=5`，`multi=6`）。

## 1. 选段

`test\hsr_scan.py` 用 480×204 GRAYS 全片扫一遍：**全片没有一次切镜**（diff > 12 一处都没有），逐帧 diff p50 0.042 / p90 0.134 / p99 0.275 / max 0.841；最忙的三段是 2354–2383（78.47 s）、3515–3544（117.17 s）、4318–4347（143.93 s）。

截帧目视后排除了几处：

| 帧 | 内容 | 是否选 |
|---|---|---|
| 2431 | 骰子穿云 + 大量运动模糊粒子 | 否（粒子噪声会让差异度量失真） |
| 2919 | 放射爆炸闪白 | 否（整帧亮度跳变，切镜检测必误判） |
| 3510–3540 | 卡牌雨中角色动作，牌面细碎、遮挡多、有运动模糊 | **是** |

测试段固定为 **START=3510（117.0 s）**。

## 2. 速度

`test\hsr6x_bench.py 2 both`：每个配置渲染同样 30 对源帧（即 30 组输入），跑 2 轮取最好。日志来自 vspipe 自身输出（stderr 落在 `work\hsr_bench_*.err`）。

| 分辨率 | multi | 输出 fps | 源帧/s | `evaluate` 均值 | 备注 |
|---|---|---|---|---|---|
| 3840×1632 | 2 | 45.87 | 22.93 | 7.89 ms | |
| 3840×1632 | 4 | 43.01 | 10.75 | 5.53 ms | 输出帧率被 ffms2 解码 + RGBS 上传吃掉大半 |
| 3840×1632 | 6 | 16.01 | 2.67 | 22.62 ms | 第二轮掉到 8.34 fps / 53.10 ms，显存与带宽压力明显 |
| 1920×816 | 2 | 108.22 | 54.11 | 4.42 ms | |
| 1920×816 | 4 | 98.79 | 24.70 | 3.29 ms | |
| 1920×816 | 6 | 101.01 | 16.84 | 3.11 ms | 单帧成本与 multi 基本无关 |

两点读法：

- **`evaluate` 才是 NGX 单次调用**，1080p 三个 multi 都在 3–4.5 ms，说明多帧不额外收费；4K 6× 的 22.6 ms 是真实成本（输出 5 张 4K 帧的读写本身就很贵）。
- 4K 的端到端 fps 主要卡在解码与 `RGBS` 上传，不是插帧本身。要比纯插帧吞吐看 1080p 那一块。

## 3. 画质：先说口径

真实的 6× 用法是「相邻两帧之间插 5 帧」，**这两个源帧之间没有任何真实画面**，所以拿不到 GT，`MAE/VMAF` 这类指标在这里无从谈起。能做的只有两种 GT 代理：

| 代理 | 做法 | 含义 |
|---|---|---|
| α=0.5 中点 | `multi=N` + `SelectEvery(N)`，只比 5 个 slot 里正好落在真实帧上的那个 | 相当于要求引擎在两帧间隔内插出**整帧**，是 6× 真实难度的 6 倍（真实用法每步只走 1/6） |
| step=6 全 slot | 每 6 帧取一对，slot k 对应真实帧 `base+k` | 最狠：既跨越 6 帧间隔，又要求 5 个位置都准 |

`test\hsr6x_quality.py` 只比较 `(k*STEP) % MULTI == 0` 的 slot，因此 `STEP=2` 时 `multi=2/4/6` 都退化成「只比中点」，三者数字完全相同 —— 这本身就是一个结论：**同一时间位置生成的像素与 `multiFrameCount` 无关**，multi 只决定一次生成几张。

### 结果（4K，START=3510，16 组，`scene_threshold=1.0` 关掉切镜检测）

| 目录 | 指标 | slot1 | slot2 | slot3 | slot4 | slot5 |
|---|---|---|---|---|---|---|
| `hsr6x_m6`（STEP=6） | MAE | 0.1085 | 0.1233 | 0.1280 | 0.1282 | 0.1109 |
| | PSNR dB | 17.17 | 16.39 | 16.19 | 16.03 | 16.55 |
| | VMAF | 35.51 | 26.85 | 21.21 | 18.83 | 18.35 |
| | vs blend | 1.06× | 1.07× | 1.07× | 1.05× | 1.02× |

| 目录 | MAE | PSNR | vs blend | VMAF |
|---|---|---|---|---|
| `hsr6x_m2_s2` / `hsr6x_m4_s2` / `hsr6x_m6_s2` | 0.15587 | 13.28 dB | 0.16077 → **1.03×** | 28.31 |
| `hsr6x_m6_s2_mv`（喂 DIS 光流） | 0.155869 | 13.28 dB | 1.03× | 28.31 |

读法：

- **在 2 帧间隔的中点上，DLSS-G 只比线性 blend 好 3%**，MAE 0.156 甚至不如直接 hold 前一帧（该片相邻帧平均差 0.105）。目视 `hsr6x_m6_s2_mv\gt3\0000.png` 与 `slot3\0000.png`：真实帧清晰，生成帧卡牌重影、整体发糊、右侧卡片叠影 —— 典型 ghosting。
- 早期 `测试数据\results\hsr6x`（默认 `scene_threshold=0.15`）那组 MAE 0.115 / VMAF 69–74 的漂亮数字**不算画质**：那一轮每一对都被判成切镜、全部 hold 真帧（`gt\0000.png` 与 `slot1..5\0000.png` 的 md5 完全相同）。
- 该片的运动量远超 Sintel trailer：同一插件在 Sintel 2× 上是 VMAF 77.1，这里的中点只有 28.3。**不是引擎退化，是素材难度**。
- 真实的 6× 输出（`hsr6x_sample`）里，半帧位置的 `slot3` 与真实帧 `slot0` 对比：人物清晰、卡牌位置合理、仅略软 —— 与「2 帧间隔的中点」完全是两个难度，不能拿上面的 0.156 直接否定 6×。

## 4. MV（mvec）实测：运行库基本无视

`docs\vdlssg-vs-rife422-flow.md` 里 2× 的结论是「喂真实光流 VMAF 只动 +0.003」。这次在 6× 上重新量化：

- `test\hsr_flow.py` 用 cv2 DIS 算反向光流（当前帧→上一帧，像素单位，半分辨率），经 16-bit PNG 载体（`work\hsr_mv\<tag>\{u,v}\NNNN.png`，`K_FLOW_SCALE=512` 即 ±256 px）喂给 `mv=`。踩的坑：Python 侧 ModifyFrame 的帧只读（`cannot set WRITEABLE flag to True`），必须走 PNG 载体；imwri 读出来是 30 fps 而 `SelectEvery` 后是 15 fps，不加 `AssumeFPS` 会报 `vdlssg: mv must be the frame size or half of it, one frame per source frame, and the same frame rate`。
- 结果：同一对帧喂常数 `(30,0)` 与 `(-30,0)`，输出相对零 MV 只变 MAE ≈1.5e-4，且正负对称。DIS 稠密光流换来的变化是 0.000001。
- 结论：**视频帧输入下，这个代理的 mvec 基本不参与计算**（插件侧上传路径 `plugin\ngx_session.cpp:390-408` 是好的，NGX 也确实收了）。不必为画质去算光流。

## 5. 切镜阈值：默认 0.15 在这类素材上太紧

`plugin\vdlssg.cpp:56-77` 的 `isSceneCut` 是「三平面、每 8 像素采样的平均绝对差 > 阈值」，numpy 复算与之逐位一致。`test\hsr_cutrate.py` 在 gap=1（真实用法）上统计帧 3400–3900：

| 阈值 | 被判切镜（hold）的 pair 比例 |
|---|---|
| 0.15（默认） | **11.0%** |
| 0.20 | 4.0% |
| 0.25 | 1.4% |
| 0.30 | 0.6% |
| 0.35 | 0.2% |
| 0.50 | 0.0% |

该段 diff 均值 0.0736 / p50 0.0579 / p90 0.1571 / p99 0.2643 / max 0.4179。插件侧交叉验证：帧 3514–3517（numpy diff 0.30 / 0.19 / 0.21 / 0.17）在阈值 0.15 下全部 `_DLSSGSceneCut=1`、`_DLSSGFallback=1`。

对快节奏、粒子/闪白多的素材（本片就是），**建议 `scene_threshold=0.25`**：6× 下 11% 的位置退化成重复帧，观感上是明显的卡顿。也顺带说明质量脚本为什么必须把它关掉：抽稀后 pair 跨 6 帧（diff 0.268），必然全部误判。

## 6. 产物与复现

| 路径 | 内容 |
|---|---|
| `测试数据\results\hsr6x_sample` | 真实 6× 样本：`pair0\slot0..5.png`、`frames\0000..0071.png`、`hsr6x_180fps.mp4`（180 fps 实时）、`hsr6x_slowmo.mp4`（同帧按 30 fps 放，6 倍慢放）、`summary.json`，293 MB |
| `测试数据\results\hsr6x_m6` | STEP=6 全 slot GT 对照（含 `gt1..5\`、`slot1..5\`、`metrics.json`），382 MB |
| `测试数据\results\hsr6x_m6_s2` / `hsr6x_m2_s2` / `hsr6x_m4_s2` | 2 帧间隔中点对照（三者输出相同），各 128 MB |
| `测试数据\results\hsr6x_m6_s2_mv` | 同上 + DIS 光流，128 MB |
| `work\hsr_preview` | 选段用的截帧 |
| `work\hsr_bench_*.err` | 测速原始日志 |

脚本：

```bat
:: 选段扫描（全片 cut、diff 分位、最忙窗口）
E:\code\VFI\.venv\Scripts\python.exe test\hsr_scan.py

:: 测速：2 轮 ×（原生 4K + 1080p）× multi 2/4/6
E:\code\VFI\.venv\Scripts\python.exe test\hsr6x_bench.py 2 both

:: 画质：midpoint 代理（等价于真实 6× 的六倍难度）
set VDLSSG_SCENE=1.0
E:\code\VFI\.venv\Scripts\python.exe test\hsr6x_quality.py - 3510 16 6

:: 画质：step=2 中点，与 2×/4× 对照
set VDLSSG_STEP=2
E:\code\VFI\.venv\Scripts\python.exe test\hsr6x_quality.py - 3510 16 2

:: 真实 6× 样本（12 源帧 → 72 帧 + 两个 mp4）
E:\code\VFI\.venv\Scripts\python.exe test\hsr6x_sample.py 3510 12 0.25

:: 切镜阈值曲线（含插件交叉验证）
E:\code\VFI\.venv\Scripts\python.exe test\hsr_cutrate.py 3400 500 0.15
```

## 7. 顺带修掉的 bug

首跑质量脚本直接报 `vdlssg: CreateCommittedResource (motion) failed (HRESULT 0x80070057)`：`plugin\ngx_session.cpp:491` 传给 `Gpu::init` 的是**未解析**的 `config.motionWidth/motionHeight`（不给 `mv` 时为 0），`plugin\d3d12_ctx.cpp:28-35` 又原样写进成员，`createResources` 就用 0 宽建纹理。改成传上一行刚算好的 `motionWidth, motionHeight` 后 4K 正常出帧。这个 bug 只在不给 `mv` 且走 4K 资源重建时触发，之前的 1080p 测试没覆盖到。

## 8. 结论

1. 6× 在 1080p 上单价与 2× 相同（约 3 ms/次），4K 6× 是真实的 22 ms 级开销，且**重复跑会掉到 8 fps**，长片建议降到 `multi=4`。
2. 真实 6× 的观感可用（半帧位置清晰），但没有 GT 就无法给数字；任何拿「2 帧间隔中点」得出的 0.156 MAE / 28 VMAF 都只代表极端工况。
3. 光流不必喂，运行库基本无视。
4. 默认 `scene_threshold=0.15` 对快节奏素材偏紧，会 hold 掉 11% 的 pair，建议 0.25 起步。
