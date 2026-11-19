# vdlssg vs rife 4.22：测试数据 1080p 的帧与稠密光流对比

一轮独立的质量复核，素材是 `E:\code\dlssg-vfi\测试数据\1080p` 的 40 张 PNG（1920×1080），
不是此前 `vdlssg-vs-models.md` 用的 Sintel。结论与那一轮一致（rife 胜），但这次多了一层
光流证据，能说清"为什么赢"。

完整报告：`E:\code\dlssg-vfi\测试数据\results\report_final.md`。

## 方法

- 40 张连续 PNG → 19 组三元组（A、真中间帧 GT、B）。两个模型只吃 A/B，GT 只用于评分。
- rife 4.22：`VFI-gui\models\rife_v2\rife_v4.22.onnx` + TensorRT FP16 engine（1088 补边，输出裁回 1080）。
  单输入 7 通道 `[A(3), B(3), t(1)]`，t=0 回 A、t=1 回 B，**不输出光流**。
- DLSS-G：`vdlssg.FrameGen(multi=2)`，19 帧全部 `fallback=0`。
- 光流由 OpenCV DIS（`DISOpticalFlow_PRESET_MEDIUM`）在 luma 上另算，三个方法（含 GT）同一套度量。

## 帧质量

| 指标 | rife422 | dlssg |
|---|---|---|
| VMAF | **62.35** | 58.12 |
| PSNR (yuv420p) | **31.97 dB** | 31.64 dB |
| SSIM | **0.9688** | 0.9656 |
| MAE | **0.008468** | 0.008893 |
| 锐度（Laplacian var / GT） | **0.839** | 0.614 |
| 逐组 MAE 胜 | **16** | 3 |

DLSS-G 只保留真中间帧 61% 的高频能量（rife 84%），所以它的"更平滑"是真的糊，不是错觉。

## 光流（luma，像素/帧）

| 指标 | rife422 | dlssg | gt |
|---|---|---|---|
| ‖F(A→M) − 0.5F(A→B)‖ | 1.774 | 1.950 | 2.299 |
| ‖F(M→B) − 0.5F(A→B)‖ | 1.933 | 2.040 | 2.378 |
| \|F(A→M)\| / 0.5\|F(A→B)\| | 0.836 | 0.890 | **0.989** |
| \|M − flow blend\| | 0.01947 | **0.01872** | 0.02098 |

**两条容易读反的地方：**

1. 光流误差指标奖励糊帧——两个模型的误差都低于真 GT（1.77/1.95 < 2.30），因为 DIS 在平滑图上
   自洽性更高。这一行测的是"有多软"，不是"运动对不对"，**不能用来排名**。
2. `ratio` 才是位移量的证据：GT 0.989，dlssg 0.890，rife 0.836。两者都把位移做小了，
   rife 欠 16%、dlssg 欠 11%——这也正是它们 MAE 低于 GT 的同源原因。

## 喂入真实光流（dlssg_mv）：换了也没用

假设：DLSS-G 质量差是因为插件喂的是零矢量。实测否定。做法是先给插件加了 `mv` 可选输入，
再用本机最好的光流（OpenCV DIS，`MEDIUM` + `finestScale=0` + `gradientDescentIterations=25` +
`variationalRefinementIterations=12`，1080p 约 0.78 s/对）按 `mvec_curr2prev` 约定
（定义在当前帧网格、指向上一帧、单位像素）写入 16-bit PNG 喂进去。

| 指标 | dlssg（零矢量） | dlssg_mv（DIS 顶配） | dlssg_mvenc（Encore 格式） | rife422 |
|---|---|---|---|---|
| VMAF | 58.123 | 58.126 | **58.137** | 62.353 |
| PSNR vs GT (dB) | 30.396 | 30.398 | 30.398 | 30.717 |
| SSIM | 0.96562 | 0.96563 | 0.96562 | 0.96875 |
| MAE vs GT | 0.008893 | 0.008892 | 0.008892 | 0.008468 |
| 锐度（Laplacian var / GT） | 0.6138 | 0.6134 | 0.6133 | 0.8392 |
| ‖F(A→M) − 0.5F(A→B)‖ | 1.950 | 1.940 | 1.938 | 1.774 |
| ‖F(M→B) − 0.5F(A→B)‖ | 2.040 | 2.065 | 2.061 | 1.933 |
| \|F(A→M)\| / 0.5\|F(A→B)\| | 0.890 | 0.888 | 0.890 | 0.836 |

- 光流确实被读了：19/19 帧输出都有变化，平均 5.7/65535 code，12–21% 像素被改动，个别帧最大 12128 code。
- 但质量指标**一点没动**（VMAF +0.003，PSNR +0.002 dB，SSIM +1e-5，锐度 −0.0004），光流自洽性一升一降。
- 因此瓶颈不在运动输入，而在运行库自己的**光流自估 + 单帧融合**：它把应用传入的 MV 当提示，
  仍以内部 `EstimateIntermMvecsScatter` / `WarpBlendingWeights` 的结果为主，缺的细节不会因为
  外部给了更好的场就长回来。
- 深度同理（内容常量 0.05/0.5/0.95/噪声全部逐位相同），所以"补深度"也不可能改善。

### 换成 Encore 自己的数值格式（dlssg_mvenc）：仍然没用

Encore 估计器输出的是 `S10.5_RG16_SINT`、每 2×2 像素一个矢量、1/32 px 精度（`rtx-encore-motion-estimator.md`）。
按这个格式复现一遍：DIS 场 → 2×2 块均值降采样到 960×540 → 量化到 1/32 px → 展开回 1920×1080
（每个 2×2 块填同一矢量）→ 喂给插件。

- 半幅纹理这条路走不通：直接喂 960×540 的 mvec 会在 `EvaluateFeature` 报
  `NGX 0xbad00005`（`NVSDK_NGX_Result_FAIL_InvalidParameter`），`CreateFeature` 却是通过的 ——
  纹理尺寸是 NGX 的参数面硬校验，必须整幅，所以只能把 2×2 网格展开成整幅。
- 展开后输出同样变了（19/19 帧、mean 5.77 code、11.6–21.2% 像素、个别帧 max 16704），
  但 VMAF 只到 58.137（基线 58.123，+0.014），MAE 完全不变（0.008892），锐度反而略降（0.6133）。
- 结论：**限制不在场的精度或格式，而在消费端**。1/32 px 定点精度、2×2 网格这些 Encore 特征值
  对 `nvngx_dlssg` 的输出没有可分辨的影响；把 MV 做到"Encore 级"也不会让 DLSS-G 追上 rife。

> 说明：Encore 自研矢量算法**无法调用**——rtx-encore 的 DLL 导出表只有 21 个 `Encore_*` 状态/UI
> API 和 `RTXEncore_ClaimsProcess`，没有任何流/插帧入口；本机也没有 NVOF SDK
> （`C:\Program Files\NVIDIA Corporation` 不存在）。所以"用 Encore 的矢量算法"这一条在技术上不可行，
> 实验改用本机可及的最高质量光流（DIS 顶配），结论仍成立。
>
> Encore 那套算法本身已被逆向清楚（`docs\rtx-encore-motion-estimator.md`：10 个 compute pass 的
> 深度加权分层块匹配，2×2 px 一格、S10.5 定点、1/32 px）。但它**改不了这里的结论**，理由是消费端：
> 我们喂的 DIS 场与运行库内部自估场的量级和质量同档（变分法通常还更强），换成一等一的块匹配
> 也换不来画质；而 Encore 的深度门槛作用在它自己的匹配权里，`nvngx_dlssg` 侧对深度内容
> 逐位不敏感（见 `dlssg-on-3080-spike.md` §7）。这不是推测：按 Encore 的数值格式（2×2 块共享、
> 1/32 px 定点）原样复现后 VMAF 只动 +0.014（见上节 dlssg_mvenc）。

## 判读

- rife 4.22 在帧质量上全面胜出，差距主要来自细节重建（不是运动估计）。
- 换成真实稠密光流（dlssg_mv）后指标不动，证明运动输入不是瓶颈；DLSS-G 的糊来自运行库内部融合。
- 相机矩阵是最后一个被排除的输入：只给投影矩阵输出逐位不变，声明帧间相机运动（`camera=2/3`）
  后 19/19 帧都变（平均 27.40 code，是喂 MV 的近五倍）但 VMAF 反降 0.289、锐度降 2% ——
  它被真正消费了，只是消费的方向是把更多像素交给 warp 候选。
- DLSS-G 的唯一优势是运动量更接近真实（ratio 0.890 vs 0.836），代价是细节被抹平；
  它赢的 3 组（0004/0006/0020）都是 1e-5 量级的险胜，且 ratio 都更接近 1。
- 作为纯视频插帧器，DLSS-G 质量落后 rife 4.22 一个档次；它的价值在速度
  （1080p GPU 合成 1.37 ms vs rife 的 23.73 ms，约 17×；端到端单帧 11.4 vs 23.7 ms，约 2.1×）。

### 为什么"补输入"注定无效：运行库内部有完整的一套

`work\dlssg_sm86\kernels\fg_kernels.dll`（11.9 MB，Compute 内核 blob）里只有 9 个 kernel，
全部围绕它自己的估计与融合（`test\probe_kernels.py` 扫出，`Kernel_*` 是 `__global__` 入口）：

| kernel | 作用 |
|---|---|
| `Kernel_InputMvecProcessing` | 处理应用传入的 MV（`mvec_curr2prev`） |
| `Kernel_InitMvecQualityMask` | **给传入的 MV 打质量掩码** |
| `Kernel_EstimateIntermMvecsScatter` | 自己估计中间帧的运动场 |
| `Kernel_WarpBlendingWeights` | 融合权重 |
| `Kernel_BlendCandidatesFused` / `Kernel_BlendCandidatesFGLite` | 候选帧融合（Lite 是省算力的轻量路） |
| `Kernel_DetectMenus` / `Kernel_DetectMenusHudless` / `Kernel_MenuDetectionOutput` | HUD/菜单保护 |

`InitMvecQualityMask` 是关键：运行库不盲信传入的 MV，而是先算一个可信度掩码，再与
`EstimateIntermMvecsScatter` 的自估场配合使用。这解释了两件事 ——
① 换更准的 MV 只会改变"掩码认可的那部分像素"，所以 19/19 帧输出有差异但指标不动；
② 极端量级（10000 px）会瞬间被掩码判为不可信，输出反而劣化（0.00416→0.00451）。
换言之，**在这条链路上"输入矢量"的上限就是自估场，不可能靠喂矢量越过它**。

逐组核对（`test\triplets_delta.py`，10 组有 VMAF 数据的三元组）也印证：按 Encore 格式
喂场后，VMAF 变化范围 −0.05 … +0.01 分、MAE 变化 −2.0e-5 … +4e-6，与帧内运动量
（|mv| 0–12.9 px）无稳定相关（相关系数 0.52，样本只有 10 且幅度是噪声级）。

### 最后一个官方输入也试过了：相机矩阵（`camera=0..3`）

插件原来给运行库的是 `cameraViewToClip`/`clipToPrevClip` 等五个单位阵、`orthoProjection=true`、
`cameraMotionIncluded=true`。社区反向工程（DLSSG-Transfusion）指出 `clipToPrevClip` 退化成
单位阵会破坏 midpoint warping，也可能短路 `NearFarPartition` 的近距离/远景分离 —— 这正好能解释
"喂 depth 逐位不变"。于是给插件加了 `camera` 参数（`test\vd_gen_camera.py`）：

| `camera` | 描述 | VMAF | PSNR | SSIM | 锐度 | 与 cam0 的输出差 |
|---|---|---|---|---|---|---|
| 0 | 单位阵 + `orthoProjection=true`（原状） | **58.126** | 31.641 | 0.96563 | **0.6134** | — |
| 1 | 真实透视投影 `P`，无帧间相机运动 | 58.126 | 31.641 | 0.96563 | 0.6134 | **逐位相同**（0/19 帧有差异） |
| 2 | 透视 + 从该 pair 的场拟合 `clipToPrevClip` | 57.837 | 31.643 | 0.96561 | 0.6010 | 19/19 帧变，平均 27.40 code |
| 3 | 同 2，且 `cameraMotionIncluded=false` | 57.837 | 31.643 | 0.96561 | 0.6010 | 与 2 逐位相同 |

三个结论：
1. **只给投影矩阵没用**：`camera=1` 输出逐位不动 —— 运行库只在声明了帧间相机运动时才走那条通路。
2. **声明帧间相机运动后确实生效**：`camera=2` 让 19/19 帧都变，改动量（27.40 code）是喂 MV
   （5.69 code）的近五倍，说明 `clipToPrevClip` 被真正消费了。
3. **但方向是变差**：VMAF −0.289、锐度 0.6010（−2%），PSNR/SSIM 基本不动。也就是说即使把它
   当作相机真的在动、把这份运动如实告诉它，融合权重只会把更多像素倾向 warp 候选，
   细节进一步被抹 —— 与 `BlendCandidatesFused` 那套"两候选取平均"的行为一致。
   `cameraMotionIncluded=false` 与 `true` 逐位相同，说明这份运动没有被拆成"相机/物体"两段使用。

至此，DLSS-G 暴露给应用的全部输入（backbuffer、MVecs、depth、五个相机矩阵、`mvecScale`、
subrect、`DynamicResolution`）都被逐一试过，没有一个是画质杠杆：**瓶颈在它自己的融合环节**，
与应用能给什么无关。

## 耗时（1080p，已停掉动态壁纸）

首次测量被 `wallpaper_engine\bin\webwallpaper64.exe`（5 个渲染进程占 GPU 95%）污染，重测后：

| 方法 | GPU 合成 median (min) | 端到端/生成帧 median (min) | 生成 fps |
|---|---|---|---|
| dlssg | **1.37 ms**（1.34） | 11.42 ms（4.39） | 730 |
| rife422 | 23.73 ms（22.28） | 23.73 ms（22.28） | 42 |

纯合成快 17×，但端到端只快 2.1×——DLSS-G 侧的 1080p 上传/回读约 10 ms，把 1.37 ms 的合成淹没。
两侧都不含 PNG 解码（`imwri` 读一张 1080p PNG 中位数 61.6 ms，比两个模型都贵）。

## 脚本

`test\` 下：`vd_common.py`（RGB float32 约定 + 中文路径 IO）、`vd_gen_rife422.py`、`vd_gen_dlssg.py`、
`vd_flow_compare.py`、`vd_vmaf.py`、`vd_bench.py`、`show_triplets.py`、`trt_rife422.py`。
通用运行器 `test\run.bat <name>`（写 `test\<name>.out`）。
