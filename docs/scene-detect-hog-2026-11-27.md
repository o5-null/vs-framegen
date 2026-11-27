# 切镜/重复帧检测：从亮度 MAE 换成梯度方向直方图（2026-11-27）

## 一句话结论

补帧引擎的切镜判定原来用「相邻两帧亮度差」，在 HDR 爆闪、番剧速度线定格、摇镜上大面积误报。
本日把判据换成 **梯度方向直方图（HOG）距离**，重复帧用 **32px 块内最大亮度差**，两者都在
`vs-dlssg\src\frame_gen.cpp` 里实现，四个插件（vdlssg / vvfg / vxessfg / vfsr4）共用。
真实素材上误报从 26.3 次/100 帧降到 1.3 次/100 帧，4K 上检测开销测不出来（128.0 vs 128.4 ms/源帧）。

## 为什么亮度不好用

| 场景 | 亮度 MAE | HOG 距离 |
| --- | --- | --- |
| 真切镜（结构全变） | 0.14 ~ 0.20 | 0.168 / 0.170 / 0.178 / 0.192 / 0.221 / 0.233 |
| 一帧爆闪（曝光跳变） | 0.12 ~ 0.17 | 0.035 ~ 0.069 |
| 摇镜 / 速度线定格 | 0.10 ~ 0.25 | 0.000 ~ 0.050 |
| 重复帧 | 0 | 0 |
| 近黑场 / 渐隐尾帧 | 0 ~ 0.03 | 0（覆盖门限拦掉） |

爆闪和真切镜在亮度上完全重叠（前者 0.12~0.17，后者 0.14~0.20），**没有任何亮度阈值能同时分开**；
HOG 只看梯度方向分布，与整体增益无关，两类之间还有 2.5 倍以上间隔。

## 判据与默认值

落点：`vs-dlssg\src\frame_gen.cpp` 的 `measureScene()`（替换原 `isSceneCut()`）。

- 采样：`step = 4`（每 4 像素取一个亮度样本），亮度归一化到 [0,1]
- HOG：`bins = 9`，无符号方向（0~π），幅值门限 `minMag = 0.02`
- 覆盖门限：有效梯度像素占比 `< minCoverage = 0.05` 或总能量 ≤1e-8 → 距离记 0（黑场、纯色、渐隐尾帧）
- 距离：`hog = 0.5 * Σ|histA - histB|`（0~1）
- 切镜：`hog >= scene_threshold`，默认 **0.12**
- 重复帧：32px（`block = 8` 个采样点）块内最大亮度差 `< duplicate_threshold`，默认 **0.005**
  （luma [0,1]；1 个 10bit limited 码值 = 1/876 ≈ 0.00114，0.005 ≈ 4.4 个码值）
- 重复帧命中 → 复制前一帧并标记 `_DLSSGFallback = 4`、`_DLSSGDuplicate = 1`，跳过引擎
- 外部检测器（上游写了 `_SceneChangePrev/_Next`）仍然优先，内置检测自动让位

分档参考（HOG）：切镜 0.17~0.23 / 爆闪 0.035~0.069 / 摇镜与特效 0.000~0.050 / 重复帧 0。
阈值在 0.07~0.15 区间内都能正确分开，取 0.12 留出两侧余量。

## 算法对照（合成片源，6 真切镜 + 闪光 + 重复帧 + 淡出）

| 检测器 | 最优阈值 | TP/FN/FP | F1 |
| --- | --- | --- | --- |
| 插件旧版（亮度 MAE，step 8） | 0.140 | 4/0/2 | 0.80 |
| 插件旧版减均值 | 0.121 | 3/1/0 | 0.86 |
| PySceneDetect ContentDetector（亮度） | 0.058 | 4/0/3 | 0.73 |
| ContentDetector（含边缘） | 0.126 | 4/0/2 | 0.80 |
| 运动补偿 MAE | 0.131 | 4/0/2 | 0.80 |
| k7f SCDetect2 algo1（MAE） | 0.060 | 4/0/2 | 0.80 |
| k7f SCDetect2 algo3（直方图） | 0.500 | 2/4/1 | 0.44 |
| k7f SCDetect2 algo4 | 0.020 | 3/3/3 | 0.60 |
| PySceneDetect AdaptiveDetector | ratio 4 / min 0.08 | 6/0/0 | 1.00 |
| **HOG（本实现）** | **0.10** | **5/1/0** | **0.86**（唯一漏的是"运动速度突变"，实际内容未变，判非切镜是对的） |
| k7f SCDetect2 algo5（HOG，同口径） | 0.12 | 同上 | 与插件判定逐边界一致 |

关键点：**所有亮度类度量的上限都是 0.80**，差距不在度量而在判定规则；AdaptiveDetector 的
邻域比值机制能排除闪光，代价是动作/快摇段漏检。HOG 用一个度量同时解决闪光与特效误报。

## 真实素材验证（300 帧窗口，插件实测）

| 素材 | 切镜/100帧 | 重复帧/100帧 | 说明 |
| --- | --- | --- | --- |
| 番剧 TANKCHAIR | 4.0 | 0.7 | 动作密集，切镜多属正常 |
| 实拍 Ramparts | 1.3（原 26.3） | 5.3 | 大量漫画速度线/定格特效，正是旧判据失控处 |
| 阿哈 PV | 3.7 | 2.3 | MV 快切 |
| HFR 60fps | 0.7 | 0 | — |
| HDR10 4K | 0 | 0 | 10bit 输出正常，无 HDR 相关误报 |

人工逐帧核对推翻了一批"共识标注"：TANKCHAIR 230/251（同镜头微移）、Ramparts 171/173（速度线
定格，两帧几乎相同）都不是切镜，而亮度类检测器全部误报；真切镜 TANKCHAIR 109、62 与 Ramparts 29
的 HOG 分别为 0.171 / 0.221 / 0.168。**结论：真实素材上亮度 MAE 不能当真值用。**

## k7 侧补帧脚本的默认值（`k7sfunc\mpv\portable_config\vs\`）

| 脚本 | 补帧选项 | 切镜 | 重复帧 |
| --- | --- | --- | --- |
| `MEMC_DLSSG_NV.vpy` | `Multi=3, Preset="a"` | `Scene=0.12` | `Dup=0.005` |
| `MEMC_VFG_NV.vpy` | `Multi=2, Mode=1` | `Scene=0.12` | `Dup=0.005` |
| `MEMC_XESSFG_NV.vpy` | `Multi=2` | `Scene=0.12` | `Dup=0.005` |
| `MEMC_FSR4.vpy` | `Multi=2, Camera=0` | `Scene=0.12` | `Dup=0.005` |
| `MEMC_RIFE_AJI.vpy` | `Model="4.14", Fps_Num=2` | `Sc_Mode=1`（用上游 `_SceneChange*`） | 无 |

控制链：脚本字面量 → `k7f.FRAMEGEN_VPY(scene=, duplicate=)` → 插件 `scene_threshold` /
`duplicate_threshold`；优先级 字面量 < 环境变量 `<NS>_<KEY>` < user-data，OSD 由
`scripts\k7f_vpypar.lua` 提供 `scene`（关/0.08/0.12/0.15/0.2）与 `duplicate`（关/0.003/0.005/0.01）两项。

## 性能

- 4K（3840x2160）60 源帧：128.0 ms/源帧（检测开）vs 128.4 ms/源帧（`--off`）→ 被引擎耗时淹没
- 1080p：检测开销同样在 ms 级以下（1/4 采样 + 9 bin 直方图）

## k7f 侧的同一套算法

用户提出"多个插件共用一套 k7 检测"。四个插件 DLL 由同一份 `frame_gen.cpp` 编译，插件侧天然是一套
（`scene_threshold` / `duplicate_threshold`）；k7f 侧补了同口径实现，供纯 VS 脚本与自定义上游使用：

- `k7sfunc.PROP_HOGDIFF(input, sample_step=4, bins=9, min_mag=0.02, min_coverage=0.05)` → 写 `SCPlaneStatsDiff`
- `k7sfunc.SCDetect2(..., algo=5, threshold=0.12, hog_sstep=4)` → 写 `_SceneChangePrev/_SceneChangeNext`

两条路径判定一致（实测）：合成片源 240 帧都判 `[29, 179]`；番剧 TANKCHAIR 120 帧都判 `[62, 109]`，
差异 0。差别只在开销：k7f 走 Python 逐帧回调（TANKCHAIR 120 帧 61.8 s），插件在 C++/GPU 流水线里
（4.5 s）。所以默认仍走 C++ 内置检测，k7f 的 `SCDETECT2(algo=5)` 作为可选上游（写入 `_SceneChange*`
后插件内置检测自动让位）。

## 复现与脚本

- `bench\src\scene_algos.py` — 各检测器的 numpy 复刻（HOG / 内容 / 边缘 / 运动补偿 / 自适应 / dHash / 块最大差）
- `bench\src\scene_detect_bench.py` — 合成带标注片源 + 全算法扫描（`--no-vs` 跳过 vspipe）
- `bench\src\real_data_bench.py` — 8 个真实样本，NVDEC 解码缓存（`--hw cuda|off`）
- `bench\src\verify_real_plugin.py` — 直接跑真插件统计 `_DLSSGFallback` 密度（`[样本索引] [帧数] [--off]`）
- `bench\src\verify_cut_detect.py` — 合成片源跑真插件，逐边界对照标注
- `bench\src\density_stats.py` / `probe_gate.py` / `inspect_boundary.py` / `dump_grid.py` — 密度统计、门限校准、单边界探针、目视联系表
- `bench\src\mpv_smoke.py` — 部署后 mpv 冒烟（`python bench\src\mpv_smoke.py [脚本名] [源] [--frames=N]`）

## 坑

- **mpv 侧 k7sfunc 版本**：`测试环境\mpv-lazy-nv\Lib\site-packages\k7sfunc` 是 PyPI 1.8.1，不含
  `mod_vpy`/`FRAMEGEN_VPY`。部署必须整包覆盖为仓库 `k7sfunc\src\k7sfunc`（旧版已备份为
  `k7sfunc-1.8.1.bak`）。只丢 `mod_vpy.py` 会报 `AttributeError: module 'k7sfunc' has no attribute 'FRAMEGEN_VPY'`。
- **mpv `--vf=vapoursynth=` 的路径必须整体加引号**，否则 `Error parsing option vf`。
- `测试环境\mpv-lazy` 是另一份独立便携包（k7sfunc 1.8.1、无 vdlssg 插件、`portable_config\vs` 为空），
  不要把它当成测试环境；真正部署的是 `mpv-lazy-nv`。
- MSVC + Windows.h 的 `min` 宏会让 `std::min(...)` 报 `error C2589`，写成 `(std::min)(...)`。
- BestSource 会同时写 `_Range=0`（limited）与 `_ColorRange=1`（mpv 枚举里 1 也是 limited），两个键含义相反，
  读取顺序必须固定。
