# vdlssg — DLSS-G 帧生成 VapourSynth 插件

`core.vdlssg.FrameGen(clip, multi=2, proxy_dir, gpu=0, scene_threshold=0.12, duplicate_threshold=0.005, preset=None, log=0)`
输入：平面 YUV 8..16bit 或 RGBS；输出 N→multi·N（`multi` = 2..6，即每对源帧插 `multi-1` 帧；6× 见
`docs\vdlssg45-engine.md`）。
每组 `multi` 帧里槽 0 原样返回源帧，槽 1..multi-1 由 DLSS-G 合成；末组不足时 hold，切镜吐源帧，
重复帧整组复制前一帧。
属性：`_DLSSGGenerated` / `_DLSSGFallback`(0 正常 / 1 切镜 / 2 运行库抑制 / 3 末帧 hold / 4 重复帧) /
`_DLSSGSceneCut` / `_DLSSGDuplicate` / `_DLSSGMultiFrameIndex`(0..multi-1)。

多帧的插值位置**只由 `multiFrameIndex` 决定**，与求值顺序无关（见 `vdlssg-multiframe.md`）。
同一 reset 后连续求值多个 index 时，运行库只对第一个写 `disable=0`，其余置 1（"别呈现"提示），
但纹理内容照常正确 —— 所以插件在 `multi > 2` 时忽略该标志；`multi == 2` 仍按抑制处理。

## 可选运动矢量输入 `mv`

`core.vdlssg.FrameGen(clip, multi=2, mv=mvclip, ...)`，`mvclip` 与 `clip` 同帧数、同帧率，RGBS。
运行库要的是 `mvec_curr2prev`：**平面 0 = 水平、平面 1 = 垂直，单位像素，定义在该帧网格上、
指向它的上一帧**；第 k 帧的场属于源帧 k（第 0 帧没有前帧，填零即可）。不给 `mv` 时插件沿用一个
常量零场（内容对输出影响极小，见下）。

- 传入时每趟都会重新上传运动纹理（`Gpu::recordPreEvaluate(copyColor, copyMotion)`），
  且在两趟 Evaluate 里分别用 `srcA` / `srcA+1` 帧的场。
- **尺寸只能是整幅**。插件自己支持"比帧小"的 motion 纹理（`Gpu::init(..., motionWidth,
  motionHeight, ...)` 独立建纹理与上传缓冲），`mvecScale` 仍取 `1/w, 1/h`（矢量单位是帧像素，
  与纹理分辨率无关）；但 NGX 在 `EvaluateFeature` 阶段硬拒半幅纹理：
  `EvaluateDlssg failed (NGX 0xbad00005)`，`0xbad00005 = NVSDK_NGX_Result_FAIL_InvalidParameter`
  （`ngxsdk\include\nvsdk_ngx_defs.h:125`）。`CreateFeature` 通过、只有 Evaluate 报错，代理日志里
  只留一条 E 级 `event":"evaluate"`，没有具体原因说明。所以纹理尺寸是 NGX 的参数面硬校验，
  与 `pDepth`/`pMVecs` 传 nullptr 报 `0xbad0000a` 同一类。每 2×2 像素一个矢量的格式只能**展开成
  整幅**再喂（每个 2×2 块填同一个矢量），代价是 4 倍上传流量、换不来任何画质。
- 实测结论（`vdlssg-vs-rife422-flow.md`）：喂 DIS 顶配光流后输出 19/19 帧都变了（平均 5.7/65535 code），
  但 VMAF/PSNR/SSIM/锐度全部不动 → **运行的瓶颈在运行库内部融合，不在运动输入**。
  按 Encore 自己的数值格式（2×2 块共享 + 1/32 px 定点）复现后同样只到 58.137（基线 58.123）。
  这个参数保留下来是给"想验证 MV 是否被读"和以后换运行库用的，不是画质开关。
- 编码约定：16-bit PNG，`code = v*128 + 32768`（±256 px，1/128 px），u 在 R、v 在 G、B 置 0。
  三道脚本：`test\vd_make_mv.py`（DIS 顶配算场 → `results\dlssg_mv\mv`）、`test\vd_make_mv_half.py`
  （2×2 块均值降采样 + 量化 1/32 px → `results\dlssg_mv_half\mv`，960×540）、`test\vd_make_mv_enc.py`
  （上一份 2×2 展开回 1920×1080 → `results\dlssg_mv_enc\mv`）；生成脚本 `vd_gen_dlssg_mv.py` /
  `vd_gen_dlssg_mvenc.py`。因为 `imwri` 会归一化到 [0,1]，读回后要
  `core.std.Expr([mv], ["x 65535 * 32768 - 128 /", "x 65535 * 32768 - 128 /", "0"])` 还原像素值
  （单个 clip 时 Expr 的 `x` 就是当前平面，写成 `y` 会报 `reference to undefined clip: y`）。
  注意 `cv2.imwrite` 在含中文的路径上**静默返回 False**，写 PNG 必须走 `cv2.imencode` + `tofile`。

前提（缺一不可，详见 `dlssg-on-3080-spike.md`）：链动态 `nvsdk_ngx_d.lib`、`PathListInfo` 含
`<proxy>\dlssg_sm86\runtime`、代理需 `LoadLibraryW(L"version.dll")` 且 `<proxy>\dlssg_sm86\logs\` 预先存在。

## 相机描述 `camera`

`core.vdlssg.FrameGen(clip, multi=2, ..., camera=0)`。运行库的 `Opt_Eval_Params` 里有一整套相机
描述（`cameraViewToClip`/`clipToCameraView`/`clipToLensClip`/`clipToPrevClip`/`prevClipToClip`、
`orthoProjection`、`cameraMotionIncluded`、`cameraNear/Far/FOV/Aspect`）。插件默认给"没有相机"的
调用者会给出的东西：五个单位阵 + `orthoProjection=true` + `cameraMotionIncluded=true`。

| `camera` | 写入内容 |
|---|---|
| 0 | 单位阵 + 正交投影（默认，与早期版本逐位一致） |
| 1 | 真实透视投影 `perspectiveRH(FOV 60°, near 0.1, far 100)`，`clipToCameraView = P⁻¹`，`orthoProjection=false` |
| 2 | 同 1，且把该 pair 的运动场最小二乘拟合成全局仿射（每 16 px 采样、两趟、剔残差 >3 px），嵌进 `clipToPrevClip`/`prevClipToClip` |
| 3 | 同 2，且 `cameraMotionIncluded=false`（矢量只当物体运动） |

仿射拟合在 `ngx_session.cpp` 的 `fitGlobalAffine()`（`p_prev = A·p_cur`），`clipToPrevClip = S⁻¹·A·S`，
`S` 是 clip→像素的缩放翻转阵。拟合失败（有效样本 <64）回退成恒等，绝不沿用上一 pair 的数。

实测（`test\vd_gen_camera.py` + `metrics_camera.json`，见 `vdlssg-vs-rife422-flow.md`）：
`camera=1` 输出与 0 **逐位相同**（不声明帧间运动，投影矩阵走不到）；`camera=2` 让 19/19 帧都变
（平均 27.40 code，比喂 MV 的 5.69 大近五倍）但 VMAF 58.126→57.837、锐度 0.6134→0.6010；
`camera=3` 与 2 逐位相同。结论：这条通路确实被消费，但消费方向是把更多像素交给 warp 候选，
画质更差。**默认保持 0**，这个参数只作为取证入口。

## 模型预设 `preset`

`core.vdlssg.FrameGen(clip, multi=2, ..., preset="c")`，取值 `a` 或 `c`；不给就沿用引擎包里的值
（出厂包是 `a`）。

DLSS-G 的 preset 不是 NGX 参数：sm86 代理在**装载 `version.dll` 时**读引擎包自己的
`<proxy_dir>\dlssg_sm86.ini` 的 `[DLSSG] Preset`，把 id 钉死在整个进程上（日志 `event:preset_pinned`、
`hooks.preset_select`），之后任何 API 都改不了。插件因此在 `Impl::init` 里、`loadProxy()` 之前把
INI 的这一行改写成 `A` 或 `C`，并用命名互斥 `Local\vs-dlssg-preset-ini` 把"改写 + 装载 + 建 feature"
串起来，避免两个进程互相读到对方的取值。同一进程里后来的 `FrameGen` 只能沿用同一个值，换值直接报错。

| `preset` | 代理日志 | 含义 |
|---|---|---|
| `a`（缺省） | `preset_id:1, effective:0, overrode_runtime:false` | 历史模型，与旧版本逐位一致 |
| `c` | `preset_id:3, lite_effective:1, overrode_runtime:true` | FG Lite：每生成帧 GPU 时间约少 13%，画质略低 |

实测（RTX 3080、1080p、multi=6、sintel 90..113 源帧、各两轮）：`a` wall 6.35 ms/输出帧、GPU 均值
3.89、p50 3.08；`c` wall 6.05、均值 3.55、p50 2.66。画质：静态段（帧 2/5/8/11/14/17）与 `a`
**逐像素相同**；运动段 PSNR 45.64–47.95 dB、max|Δ| 0.14–0.18（线性 0..1，约 68% 像素差 >1e-4），
差异集中在 HUD 与遮挡边缘，与 FG Lite 的设计一致。

`preset="b"`（UI 重组）直接报错，而不是接受后静默降级：它要运行库同时拿到 `pHudless`（无 UI 画面）
与 `pUI`/`pUIAlpha`（UI 颜色与遮罩），本插件在 `runPass` 里把后缓冲直接当 hudless 交出去、UI 平面
留空，代理会判 `preset_ineffective`（`"missing":["DLSSG.UI"]`）并按 `a` 跑。成片视频也不存在可分离
的 UI 层，这条通路要新增三路输入才谈得上，不是预设开关。

**改写发生在引擎包文件里**：跑过一次 `preset="c"` 之后，包里的 INI 就停在 `C`，下次不写 `preset`
的脚本也会跑 C；恢复默认用 `preset="a"` 跑一次即可。

本机验证（2026-11-23，`test\mf6.vpy` 用环境变量 `VDLSSG_PRESET` 选档，缺省 `a`）：`preset="c"` 跑完
288 帧 1920x1080（6.61 ms/输出帧），stderr 有 `[vdlssg] DLSS-G preset c (written to the engine pack's
dlssg_sm86.ini)`，代理日志对应进程 `preset_id:3, lite_effective:1`；`b` 与非法值都以
`vapoursynth.Error` 退出，INI 不动；再跑一次 `a` 后 INI 回到 `Preset=A`。
测这套东西时 `.venv\Scripts\vspipe.exe` 在本机报 `VSScript reported error: Python executable and
library path couldn't be determined`，改用 venv 的 python 直接 `exec` 那个 `.vpy`、取 `out.get_frame(i)`
循环即可（`vspipe` 只在源脚本读不进来时才需要）。

## 2026-11-16 修复与验证

三个 bug 已修并验证（`vdlssg.dll` 重编译后跑 `test\smoke.py`）：

1. **计时下溢**（`[vdlssg] frame N: 18446744073683.83 ms`）：`evaluateMs` 的 uint64 时间戳差在
   query 未解析时下溢。修复后 1080p 每生成帧 1.26–2.83 ms（偶发 18.6 ms 为桌面占用）。
2. **帧引用泄漏**（`Core freed but 522805248 bytes still allocated in framebuffers`）：
   `getFrameFilter` 拿到的 a/b 未 `freeFrame`、切镜分支前预分配的 `out` 被丢弃。修复后该警告消失。
3. **同进程多 clip 输出陈旧帧**：帧复用缓存（`lastPairIndex`/`pairSeeded`/`seededPairIndex`）只按
   `pairIndex` 判定，而 session 是进程级单例；脚本里连着建多个 `FrameGen` 时，第二个 clip 的首对
   `pairIndex == 0` 命中上一个 clip 的状态，跳过 pass1 的 reset 与上传，输出一张陈旧画面
   （RGBS 均值恒 0.488）。修法是每个过滤器实例取一个 `NgxSession::newStream()` 代次，
   `generate()` 以 `streamId` 为第三参；代次变化时复位四个缓存字段。
   回归入口：`test\diag_fp.py t01 t05 t07` 的三个 slot1 md5（`1c68edd7ce55` / `a24b13cd3914` /
   `8790d6397f70`）必须与单跑一致。

**质量对标**（`media\parity2.py`，10 组 Sintel 1080p 三帧，GT=真中间帧）：

- 透传帧 vs ffmpeg 抽帧：MAE **0.000000**（输入通路逐位无损）
- 插件输出 vs spike 输出：MAE **0.000000**（10/10 逐位相同）
- 插件输出 vs GT：mean MAE **0.004109**，与 spike 的 0.00411 一致

→ 插件复刻了 spike 的像素输出，spike 的 VMAF 结论（dlssg 77.11 / rife 80.21 / minterp 74.35 /
blend 57.69）可直接沿用，不需要重跑 VMAF。

## 环境坑（不要再踩）

`core.resize.Bicubic(..., format=vs.RGB24)` 在本机 venv 的 VS R74 里产出**行 stride = 宽 1920
而非 5760**的打包帧：vspipe 写出的 rgb24 是 3×3 平铺的碎图，任何基于它的 PNG/MAE 全部作废
（曾误判为 DLSS-G 输出损坏）。要预览或比对：

- 留在 `vs.RGBS`（三个 float 平面），手动 `np.stack([r,g,b], -1)` 交错成 rgb24；或
- 直接看 `_DLSSG*` 属性判断是否走生成路径。

`core.resize.Bicubic(clip, format=vs.RGBS, matrix_in_s="709")` 这条 YUV→RGBS 通路是好的
（源帧 `YUV420P8`，props `_Matrix=2` 未指定，必须 `matrix_in_s`；用 `matrix_s` 会报
`RGB color family cannot have YUV matrix coefficients`）。

## 脚本

- `test\smoke.py` — 整段跑的冒烟脚本（`--start 100 --end 140`）
- `media\parity2.py` — 权威对标：透传/生成帧 vs 抽帧与 spike，全部在 RGBS 平面里算
- `media\trip_one.py` / `trip_props.py` / `trip_dump.py` / `trip_warmup.py` — 单组三帧诊断（属性、MAE、冷热历史）
- `media\probe_frame.py` / `probe_rgb.py` / `probe_src.py` — 定位 stride 坑的探针
- `media\vmaf.py` — VMAF 打分（读 spike 的 ppm）

运行方式（不要用 PowerShell 的 `&` 串联，会被拆成 BackgroundJob）：

```
cmd /c E:\code\VFI\.venv\Scripts\vspipe.exe --start 100 --end 140 E:\code\dlssg-vfi\test\smoke.py - > NUL 2> smoke.err
```

把帧数据写进日志会得到 900 KB 二进制垃圾，stderr 单独重定向再看。
