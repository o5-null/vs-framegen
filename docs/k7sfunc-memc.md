# K7sfunc 1.8.1 的补帧模块是怎么写的

> 对象：`E:\code\dlssg-vfi\测试环境\mpv-lazy\Lib\site-packages\k7sfunc`（PlayKit 随包）。
> 补帧全在 `mod_memc.py`（889 行），配套 `mod_helper.py` 的切镜检测与 `mod_fmt.py` 的
> `FPS_CTRL/FMT_CTRL`；`portable_config\vs\MEMC_*.vpy` 是用户入口。
> 全部是 CPU 侧 VS frame + 现成插件/推理后端，**不碰 D3D12、不碰 NGX、没有 GPU 共享纹理**。

## 1. 四个家族，一个外壳

| 家族 | 引擎 | 后端 | 关键旋钮 |
|---|---|---|---|
| `MVT_LQ` / `MVT_MQ` | mvtools（CPU 块匹配） | — | blksize 32 / 4–32；pel 1 / 2；`BlockFPS` 还是 `FlowFPS`；`thscd1/2` |
| `DRBA_DML` / `DRBA_NV` | distilDRBA v1 / v2-lite（ONNX） | ORT-DirectML / TensorRT | `turbo` 0/1/2 → ap / scale；tile 64 / 128；gpu_t 流数 |
| `RIFE_COREML` / `_DML` / `_NV` | RIFE 4.6 / 4.25lite / 4.26 / 4.26heavy | ORT-CoreML / ORT-DML / TRT（vsmlrt） | `ensemble`（两次前向）、proc-layer 还是 `_implementation=2`、TRT shape 档 |
| `RIFE_STD` | vs-rife（ncnn Vulkan） | — | `sc`、`skip` + `stat_th`（VMAF 跳帧） |
| `SVP_LQ` / `SVP_HQ` / `SVP_PRO` | SVP SDK svp1+svp2 | CPU / GPU 块匹配，`SVP_PRO` 可走 NVOF | block 32×16 或 32×32、mask、scene limits、algo 13/21/23 |

两条「同一家族的瘦后端」写法值得注意：`DRBA_HUB`（`mod_memc.py:139-239`）与
`RIFE_ORT_HUB`（`:420-529`）吃全部公共逻辑，DML/CoreML/NV 只剩一个
`def backend_param(...)` 闭包（`:261-263`、`:316-339`、`:592-594`）——和本仓
`frame_gen.cpp` + `profiles.h` 把引擎差异压进一张表的思路是同一个。

## 2. 每个函数都走同一段外壳

以 `RIFE_NV` 为例（`mod_memc.py:715-741`）：

1. `SCENE_DETECT(input, sc_mode)` — 先把切镜属性写到帧上（`:715`）。
2. `resize.Bilinear(..., format=vs.RGBS, matrix_in_s="709")`（`:717`；DRBA 用 `RGBH`，`:219`）。
   输入是 YUV 时一律按 709 转，RGB 输入时不传 matrix。
3. `AddBorders` 补到 tile 的整数倍，中间帧算完再 `Crop`（`:719-730`）——只补右/下边，
   因为模型对输入尺寸有 32/64/128 的整除要求。
4. 推理：`vsmlrt.RIFE(clip, multi=Fraction(num, den), ...)`，`video_player=True`。
5. `resize.Bilinear` 回源像素格式，`range=1 if _ColorRange==0 else None`（`:739`）——
   有限范围进、有限范围出，全范围不动。
6. **只有倍率不是整数时**才 `AssumeFPS(fpsnum=fps_in*num*1e6, fpsden=den*1e6)`（`:740-741`）。

用户入口脚本再在前面加两道闸：`FPS_CTRL(..., fps_ret=True)` 对 >32 fps 的源直接抛错
（`mod_fmt.py:222-224`，默认 `fps_max=32`），`FMT_CTRL` 可按显示器高度预缩放。

## 3. 切镜：检测器只写属性，引擎自己读

`SCENE_DETECT`（`mod_helper.py:617-633`）三档：

- `sc_mode=0` 关；
- `sc_mode=1`（默认）走 `SCDDetect2`，`threshold=0.15`；
- `sc_mode=2` 走 mvtools `SCDetection`，`thscd1=240, thscd2=130`。

`SCDDetect2` 的四个算法（`mod_helper.py:517-615`）：

| algo | 计算 | 建议阈值 |
|---|---|---|
| 1（默认） | 灰度 `PlaneStats` 的 MAE | 0.10–0.15 |
| 2 | 先 `Sobel` 再 MAE（对整体亮度变化不敏感） | 0.05–0.10 |
| 3 | 256 bin 直方图 L1 差 / (2N) | 0.20–0.40 |
| 4 | 直方图 **AND** 边缘 MAE，两者同时超阈值 | 0.10–0.15 |

两个工程细节：
- **先降到 ≤2 304 000 像素再比**（`max_size`，`:556-564`），1080p 也只在 ~1.5 MP 上算，
  大分辨率不会把检测变成瓶颈；非 GRAY 先 `resize.Point` 成 `GRAY8/16`。
- 直方图用 numpy `bincount` + 归一化 L1（`:507-510`），0–1 语义，与 MAE 同量纲。

输出的是 **属性**而不是裁剪过的 clip：`_SceneChangePrev/_SceneChangeNext`
（`:594-595`、`:610-611`）。引擎侧（`vsmlrt.RIFE(..., sc=True)`、RIFE 插件）
自己读这两个属性，所以检测算法、阈值、引擎三者互不耦合。这正是参考插件
`vsdlssg` 用的同一套约定，也是本仓目前没有接的。

## 4. fps 与倍率

- 倍率是 `fractions.Fraction(fps_num, fps_den)`，**任意分数**：23.976 → 59.94 填
  `fps_num=5, fps_den=2`（2.5×），落点正好 59.94。`SVP_PRO` 另有 `abs` 参数在
  「倍数」与「绝对目标帧率」两种语义间切换（`:870`、`:876`）。
- 时间戳只做 `AssumeFPS`，**不写** `_DurationNum/_DurationDen/_AbsoluteTime`——
  在 mpv 播放链里够用；只有 vspipe→x264 这类封装路径才需要逐帧时长（参考插件补的就是这块）。
- `MVT_LQ/MQ` 是唯一用绝对 fps 的一族（`fps_in`/`fps_out` 两个参数，`:37-38`），
  其余都是「倍数 + fps_in」。

## 5. 分辨率与显存

- 硬上限 `4096×2176`（约 8.9 MP），超了直接抛「源分辨率超过限制的范围」（`:207-208`、
  `:485-486`、`:683-684`）。
- 动态 shape 的 TRT engine 必须给 min/opt/max 三档，K7sfunc 按 tile 宽度查表
  （`:705-713`）：32 → (320,256)/(1920,1088)/(4096,2176)/(2048,1088)；
  `model>=47`（RIFE 4.25 系）改静态 engine（`:681-682`）。
- `>2048×1088` 时 `scale_model=0.5` 半分辨率推理，但只在 proc-layer 路径生效
  （`:689-694`，引 vs-mlrt issue #1002）。
- 模型文件缺失时抛中文错并点名路径（`:670-672`），插件缺失由 `_check_plugin` 拦。

## 6. 一个真实缺陷

`RIFE_STD`（`:361-414`）在 `:386` 就 `if skip :`，而 `skip` 到 `:394-402` 才赋值，
函数内无 `global`——Python 会把它判为局部变量，**调用即 `UnboundLocalError`**。
即 ncnn Vulkan 那条路径在 1.8.1 是死的（静态读出的结论，没实跑）。
同函数 `:387` 引用的 `vmaf` 插件也因此从未真正被检查。

## 7. 与本仓 vdlssg 的对照

| | K7sfunc | 本仓 `vdlssg`+`MEMC_DLSSG_NV.vpy` |
|---|---|---|
| 倍率 | 任意分数 `num/den` | 整数 `multi` 2..6 |
| 切镜 | 属性化，检测器/引擎解耦，四算法，先降采样 | 内联在插件里，step 8 三平面均值，单条件 |
| fps | `AssumeFPS` 微帧率 | 同（`fpsnum=round(fps*Multi*1e6)`） |
| 补边 | 补到 tile 整除 | 无（固定 1:1） |
| 色彩 | RGBS/RGBH + 709，回程按 `_ColorRange` 还原 | RGBS + 709，回程 full→limited |
| 后端抽象 | `_HUB` + `backend_param` 闭包 | `frame_gen.cpp` + `profiles.h` 四 DLL |
| 保护栏 | 源 fps ≤32、≤8.9 MP、模型存在性 | 只有 `Pre_H` 预缩放 |

**可借鉴（按价值排序）**

1. **分数倍率**：mpv 播放 23.976 素材时，整数 2×(47.95)/3×(71.9) 都对不齐 59.94 显示器，
   2.5× 才是干净解。DLSS-G 的 `multiFrameCount` 是整数，插件侧拿不到 2.5×，只能在脚本层
   用「3× 输出后按 5:6 抽帧」或接受重复帧近似——值得先量一下抽帧后的观感。
2. **切镜降采样 + 四算法分档**：把检测成本钉在 2.3 MP 灰度上，双条件（algo 4）直接对应
   参考插件的 hist AND spatial，且阈值有分档建议可写进 `MEMC_*.vpy` 注释。
3. **检测器只写 `_SceneChangePrev/Next`**：本仓本来就要支持「源带切点优先」，同一套约定。
4. **保护栏**：`fps_ret=True` 式的源帧率上限（>32 fps 插帧无意义且实时预算减半）、
   8.9 MP 上限、模型/插件缺失的中文点名报错。

**不必借鉴**：mvtools/SVP 的块匹配族——K7sfunc 自己把它当 LQ 档，质量比 RIFE 低一档，
本仓已有 `vsrife` 原生引擎。
