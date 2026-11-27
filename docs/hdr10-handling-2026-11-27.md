# HDR10 处理现状：vsdlssg 与 vfg（2026-11-27）

被测样片：`测试数据\hdr10.mkv`
`ffprobe`：vp9 Profile 2 / 3840x2160 / yuv420p10le / color_range=tv / bt2020nc / smpte2084(PQ) /
bt2020 / 30000/1001 fps / 4:19.659。

引擎构建：`vs-dlssg\bin\vdlssg.dll`（2026-11-26）、`vs-dlssg\bin\vvfg.dll`（2026-11-26）。

## 结论

1. **两条链路都不做 HDR 感知处理**。源码里没有任何 `_Transfer` / `_Primaries` 读取，没有 PQ/HLG
   的 EOTF/OETF，没有 primaries 变换，也没有色调映射。
2. **补帧在“编码码值域”完成**：YUV→RGB 只做矩阵 + 量化范围换算，引擎（DLSS-G / NVVFG）对
   码值做插值，结果再按同一矩阵转回 YUV420P10。实测中间帧亮度落在码值算术中点附近，而不是
   线性光中点（见下表），因此只对 SDR 数值上正确。
3. **HDR 标签原样透传**：输出帧由 `newVideoFrame(..., a, core)` 以源帧作 propSrc，继承
   `_Matrix/_Transfer/_Primaries/_Range`；生成帧只多出 `_DLSSG*` 时间轴/标记属性。
4. **位深与帧率**：10bit 源输出仍为 10bit `YUV420P10`，帧率 ×2（59.94 fps）。
5. 因此 HDR10 片源补出的中间帧**绝对亮度偏低**（PQ 码值中点对应的 nit 远低于线性光中点），
   亮度渐变/闪光处会有明暗跳变；色相与色域不错乱（矩阵一致），也不会丢 HDR 标签。
6. 文档层面已经声明此限制：`vs-dlssg\README.md:8,130`、`vs-dlssg\docs\mpv-dlssg.md:45`
   （“只做补帧，不做色调映射”）。

## 代码路径

| 位置 | 行为 |
| --- | --- |
| `vs-dlssg\src\frame_gen.cpp:397-406` | 从源帧读 `_Matrix`（缺则用 `_ColorSpace`）、`_Range`（==1 为 full）、`_ChromaLocation`；**不读 `_Transfer`/`_Primaries`** |
| `vs-dlssg\src\frame_gen.cpp:365` | `newVideoFrame(..., a, core)`，a=源帧 → 输出继承全部颜色属性 |
| `vs-dlssg\src\frame_gen.cpp:160-186` | `applyTimelineProps()` 只改 `_Duration*`/`_AbsoluteTime`/`_SceneChange*` |
| `vs-dlssg\src\yuv.h` | `yuvCoefficients()` 支持 1/4/5/6/7/9/10（9/10=BT.2020：kr 0.2627/kg 0.6780/kb 0.0593）；YUV↔RGB 只有矩阵+量化范围，**无传递函数** |
| `vs-dlssg\src\ngx_session.cpp:60` | 引擎工作缓冲：DLSS-G 用 `R16G16B16A16_FLOAT`，XeSS-FG 用 `R10G10B10A2_UNORM`；值仍是码值域浮点 |
| `vs-dlssg\src\ngx_session.cpp:932-960` | `yuvRowToRgb()` 逐行按矩阵转 RGB 后送引擎 |
| `vs-dlssg\src\ngx_session.cpp:1137-1145` | 回读 `R10G10B10A2`，解包三通道（alpha 丢弃）再转回 YUV |
| `vs-dlssg\src\ngx_session.cpp:1451-1452` | GPU YUV 回读打包位深 `(force8Bit \|\| bits<=8) ? 8 : 10` |
| `k7sfunc\src\k7sfunc\mod_vpy.py:409-515` | `FRAMEGEN_VPY()`：YUV 源直通插件；仅 RGB 源 `core.resize.Bilinear(..., format=vs.YUV420P10, matrix_s="709")`（492 行，硬编码 709） |

## 实测证据

### 属性透传（6 帧片段，`multi=2`）

vdlssg 链与 vvfg 链输出均为 12 帧、3840x2160、59.940 fps、`YUV420P10`；每张生成帧
（`_DLSSGGenerated=1`）都携带 `_Matrix=9,_Transfer=16,_Primaries=9,_Range=0`，与源帧一致。
部署在 `测试环境\mpv-lazy-nv\vs-plugins\vdlssg.dll`（2026-11-22 构建）行为相同。

### 插值域探针（合成纹理帧，避免 fallback）

两帧纹理相同、整体亮度不同，`_Matrix=9,_Range=0,_Transfer=16,_Primaries=9`，
`scene_threshold=1.0` 关闭切镜检测，取中间帧 Y 平面均值：

| 亮度对 | 码值 | 码值中点 | 线性光中点 | vdlssg 实测 Yavg | vvfg 实测 Yavg |
| --- | --- | --- | --- | --- | --- |
| 100 → 1000 nit | 509 → 723 | 616 | 666（552 nit） | 605.7（290 nit） | 624.7（356 nit） |
| 100 → 4000 nit | 509 → 855 | 682 | 792（2057 nit） | 628.9（372 nit） | 686.5（685 nit） |

vvfg 基本等于码值算术中点；vdlssg 更靠近前一帧（自身运动/混合权重），但两者都远离线性光中点。
→ 插值发生在 PQ 码值域，没有解码到线性光。

### 纯色帧

无纹理的纯色帧对会被判无效（`_DLSSGFallback=1`，保持前一帧），两条链路一致；这解释了一些
亮度骤变场景下的“重复帧”。

## 路径差异与坑

- **mpv 播放路径的帧属性更少**：`vf_vapoursynth` 只写 `_ColorSpace=9`、`_ColorRange=1`、
  `_ChromaLocation`，没有 `_Matrix/_Range/_Transfer/_Primaries`。插件靠 `_ColorSpace` 回退拿到
  矩阵；`_Range` 缺失原先一律按 limited（本片正确，full-range 源会误判）。
  **已修**（2026-11-27）：`_Range` 缺失且没有 `_Matrix` 时改读 `_ColorRange`，按 mpv/libavcodec
  枚举（1=limited、0=full）解释。
- mpv 的 `_ColorRange` 语义与 VS 相反（`mpv-src\video\filter\vf_vapoursynth.c:175`：
  limited 写 1）；正因为相反，别名只在「无 `_Range` 且无 `_Matrix`」的 mpv 签名上生效。
- **场景检测**：原先插件默认 `scene_threshold=0.15`（亮度减均值后的平均绝对差），k7sfunc 的
  `FRAMEGEN_VPY`/vpy 默认 0.25（k7f 只传值，判定在插件内置检测器）。亮度类判据在 HDR 里两头都
  不对：爆闪/闪白会误判切镜（hold 成重复帧），而暗部大范围渐变又可能漏判。**已重做**（2026-11-27）：
  判据换成两帧亮度梯度方向直方图（HOG，9 bin 无符号、4 像素采样）的 L1/2 距离，默认 **0.12**；
  有效梯度覆盖 <5% 的帧（黑场/渐隐/纯色）直接判非切镜。HOG 不含亮度，HDR 爆闪（实测 0.035–0.069）
  与真切镜（0.17+）之间有 2.5 倍以上间隔。另加 `duplicate_threshold=0.005`（32px 块内亮度最大差）
  做重复帧判定，命中即复制前一帧、跳过引擎。
- **部署版本落后**：`测试环境\mpv-lazy-nv\vs-plugins` 里的 vdlssg/vvfg 是 11-22 构建，
  repo `bin` 是 11-26；本次行为结论对两者都成立。
- **RGB 输入硬编码 709**：`mod_vpy.py` 的 RGB→YUV 出口**已修**（2026-11-27）：按源 `_Matrix`
  →`_ColorSpace`→`_Primaries`（BT.2020 → 2020ncl）选矩阵，不再硬套 709。注意 `mod_memc.py` 等
  其它 MEMC 模块仍写死 709，不在本次范围。
- **>10bit**：GPU YUV 路径只接受 8/10bit（`d3d12_ctx.cpp:459,467-475`），12/16bit 源走 CPU 路径，
  输出位深仍随源。

## 本次修复（2026-11-27）

| 文件 | 改动 |
| --- | --- |
| `vs-dlssg\src\frame_gen.cpp` | `_Range` 缺失且无 `_Matrix` 时读 `_ColorRange`（mpv 枚举 1=limited/0=full）；`isSceneCut()` 换成 HOG（`measureScene()`，9 bin 无符号、step 4、幅值门限 0.02、覆盖门限 5%），`scene_threshold` 默认 **0.12**；新增 `duplicate_threshold` 默认 0.005、`_DLSSGFallback=4`（重复帧）与 `_DLSSGDuplicate` |
| `k7sfunc\src\k7sfunc\mod_vpy.py` | RGB→YUV 出口不再写死 `matrix_s="709"`，新增 `_framegen_matrix_s()`：`_Matrix` → `_ColorSpace` → `_Primaries`(BT.2020→2020ncl) → 709；`FRAMEGEN_VPY` 增加 `duplicate` 参数 |
| 四个 `vs-dlssg\mpv\MEMC_*.vpy` | `Scene = 0.25 → 0.12`，新增 `Dup = 0.005` |
| `vs-dlssg\README.md`、`vs-dlssg\docs\engines.md`、`docs\install.md`、`docs\vdlssg-plugin.md`、`docs\vdlssg-presets.md`、`docs\vsdlssg-vs-vdlssg.md` | 同步新的阈值默认值与检测口径、`_Range`/`_ColorRange` 回退说明 |

验证状态：pytest 套件 48 passed（Python 侧）；四个 DLL 已用 `vs-dlssg\src\build.bat` 重新编译，
合成片源与真实素材（TANKCHAIR / Ramparts / PV / HFR / HDR10-4K）上验证通过，4K 检测开销可忽略
（128.0 vs 128.4 ms/源帧）。

## 若要真 HDR 正确（未实施，仅建议）

1. 补帧前把 PQ 解码到线性光、必要时做 BT.2020→BT.2020（同色域）矩阵规范化，补完再编码回 PQ；
   这样引擎的插值才是物理正确的。
2. 4:2:0 色度在 PQ 域采样会放大误差，正确顺序是 YUV→RGB（线性）→补帧→降采样回 4:2:0。
3. HDR10 静态元数据（mastering display / MaxCLL）在容器里，不经过 VapourSynth，无需改动。
4. 退一步的近似：插值前对 PQ 码值做 γ≈2.2 的线性化，代价小、可减少亮度跳变，但不是严格正确。

## 复现

- 探针脚本：`C:\Users\benben\AppData\Local\Temp\opencode\hdr10probe\pq_textured.py`、
  `flags_real.py`、`old_plugin.py`、`pq_domain.py`
- 测试套件：`.venv\Scripts\python.exe -m pytest test\suite` → 48 passed（2026-11-27）；
  其中 `test\suite\cases\test_30_color.py:102-121` 只验证 10bit→16bit PNG 不丢精度，
  **没有覆盖 vdlssg/vvfg 的 HDR10 用例**。
