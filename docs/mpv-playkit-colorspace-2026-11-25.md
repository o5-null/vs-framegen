# mpv playkit：生成帧亮度/色彩闪动的根因与修复（2026-11-25）

现象（用户）：6× 补帧播放时"画面亮度闪动，原生帧和生成帧亮度不一致"，限定色彩空间的片源
也曾报告整帧压暗/局部过曝交替。

本文记录 2026-11-25 的定位过程、两个真实缺陷、修复与实测数据，以及对打包/发版的遗留影响。

## 结论先行

生成帧的亮度/色彩与真实帧不一致有三个独立原因，前两个此前已修（`659d269`、`aa03ade`），
本次修掉的是第三个，它同时表现为：

| # | 缺陷 | 表现 | 状态 |
|---|------|------|------|
| 1 | 生成帧被强制成源帧的 `mp_image_params`，VO 每个源帧重新配置 | 每源帧一次重配（闪） | 已修 `aa03ade` |
| 2 | 视频处理器未被告知输入/输出色彩空间，默认 RGB full → BT.601 full | limited 源每生成帧压暗/过曝 | 已修 `659d269` |
| 3a | `D3D11_VIDEO_PROCESSOR_COLOR_SPACE.Nominal_Range` 在 NVIDIA 驱动输出端无效 | full-range 源的生成帧被压回 16-235 | 本次 `8f58700` |
| 3b | `vs-dlssg` 的 `yuvQuant(full)` 少掉色度中点偏移 | full-range 源全链路偏品红（R/G/B ≈ 236/5/253） | 本次 `2ea84dd` |
| 3c | bridge 的 matrix/range 只在 create 时取滤镜选项（默认 709 limited） | untagged/BT.601/BT.2020 源的生成内容系数错误 | 本次 `8f58700` |

## 复现与量测方法

滤镜内回读，不看最终输出：

```
mpv.exe --no-config --vo=gpu-next --gpu-api=d3d11 --hwdec=d3d11va --no-audio \
        --untimed --length=N \
        --vf=playkit:multi=6:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45 \
        <clip.mkv>
```

- `PLAYKIT_DSTSTAT=1`：`pk_luma_stat()` 把源纹理与每张输出纹理的 Y 平面拷到 staging 纹理回读，
  打印 `playkit: src[i]/dst[k] sub=N y=… min=… max=…`（10-bit 按 16-bit 采样读、右移 8 位归到 0-255）。
- `work/bench_npk/dst_linear.py`：以 src0→src1 的线性插值为基线，算 k=0..4 的 `mean_dev`、
  标准差、超阈值计数，以及**相邻步长**和**跨对边界步长**——后者正是"源帧率亮度脉动"的度量。
- `playkit:log=yes`：`pk_dump_rgba()` 把引擎输出的 RGBA 共享纹理写成 `work/out{0..4}.ppm`，
  可直接与 ffmpeg 解码出的参考帧做逐像素比较（绕过视频处理器，单看引擎产物）。
- `PLAYKIT_LEGACY_CS` / `PLAYKIT_OUTRANGE=full|limited`：强制走旧路径 / 覆盖输出范围，用于对照实验。

素材（`work/bench_analysis/`）：

| 文件 | 规格 | 用途 |
|------|------|------|
| `ah_1080p.mkv` | 1920x816 30fps hevc 8-bit，BT.709 limited | 基线 |
| `ah_1080p_pc.mkv` | 同上，`scale=in_range=limited:out_range=full`，tag full | full-range 8-bit |
| `ah_1080p_pc10.mkv` | 同上，P010 10-bit，tag full | full-range 10-bit |
| `ah_480p601b.mkv` | 720x480，`colorprim/colormatrix=smpte170m`、`range=limited` | BT.601 |
| `ah_720p_untagged.mkv` | 1280x720，无色彩标签 | guess 路径 |
| `bars601.mkv` / `bars709_pc.mkv` | `smptebars` 静止图，601 limited / 709 full | 静止内容逐像素比对 |

注意：造 clip 时 `zscale` 的 `rangein/rangeout` 在该 ffmpeg 里不存在，要用
`scale=in_range=limited:out_range=full`；x264 编出的这类素材在 `d3d11va` 初始化会失败
（`h264: Failed setup for format d3d11`），必须用 hevc。

## 定位过程要点

1. **limited 源本来就是好的**：`ah_1080p.mkv` 的 `mean_dev` 为 -0.24…-0.83、跨对边界 0.90，
   说明 `659d269` 的色彩空间修复确实生效，问题只出现在别的色彩空间组合上。
2. **full-range 源复现**：`ah_1080p_pc.mkv` 的源 `min=0 max=247`，生成帧却恒为
   `min=16 max=235`，`mean_dev=+6.7…+6.0`、跨对边界 5.02——生成帧被压进 limited。
3. **legacy 结构体失效的证据**：用 `PLAYKIT_OUTRANGE` 在同一次播放里强制 full/limited，
   两次输出的 dst Y 值**逐字节相同**，`Nominal_Range` 在输出端根本没生效。
   换 `ID3D11VideoContext1` + `DXGI_COLOR_SPACE_YCBCR_{STUDIO,FULL}_G22_LEFT_P*` 后，
   dst 立刻变成 `min=0 max=255`。
4. **引擎产物是好的**：把 `out_tex` dump 出来看，limited 源正常；full-range 源却是
   `R 236 / G 5 / B 253` 的品红——不是未初始化显存，是图像内容（能看到画面结构），
   说明 NV12→RGB 的色度差了半个单位。
5. **根因在 `yuvQuant`**：full 分支只设了 `lumaScale/chromaScale`，`chromaOffset` 留在 0。
   有限范围是 `(code-128)/224`，全范围却成了 `code/255`，等于色度整体 +0.5：
   `Cr≈+0.5` 抬高 R、压掉 G，`Cb≈+0.5` 抬高 B —— 正好是观察到的品红。
   写回方向（`chromaOutOffset`）同样缺失。改为 `maxValue/2` 后，引擎产物恢复正常。
6. **bridge 系数也要跟帧走**：此前 `vdlssg_bridge_create()` 收到的是滤镜选项（默认
   `matrix=0`→709、`full_range=0`），与片源无关。改为在 `pk_bridge_init()` 里按
   `mp_image_params_guess_csp()` 之后的帧参数推导（601→5/6 按分辨率、240M→7、2020→9/10、709→1）。
7. **视频处理器侧同样先 guess 再取色**：`vf_d3d11vpp.c:430-446` 就是
   `mp_image_params_guess_csp()` + `mp_params_to_dxgi_colorspace()` 的做法，照此对齐，
   untagged 片源下滤镜、bridge、VO 三者对 matrix/range 的判断才一致。

## 实测结果

`mean_dev` 为各 k 相对线性基线的平均偏差（越小越好），`跨对边界` 是真实帧→首个生成帧的步长。

| 素材 | 修复前 | 修复后 |
|------|--------|--------|
| 1080p 709 limited | mean_dev -0.24…-0.83，边界 0.90 | -0.24…-0.79，边界 0.90（无变化，基线） |
| 1080p 709 **full** 8-bit | mean_dev **+6.7…+6.0**，边界 **5.02** | -0.09…-0.66，边界 0.68 |
| 1080p 709 **full** 10-bit | —（旧读取方式无法量测） | -0.20…-0.29，sd 0.21-0.27，边界 0.24 |
| 480p **BT.601** limited | — | -0.10…-0.18，边界 0.17 |
| 720p **untagged** | — | -0.09…-0.16，边界 0.17 |

静止色条逐像素（引擎 RGBA 对比 ffmpeg 解码参考帧，3563 个采样点）：

| 素材 | 平均绝对差 | p99 | 最大 |
|------|-----------|-----|------|
| 601 limited | 0.26 | 1 | 3 |
| 709 full | 0.71 | 2 | 3 |

即整条链路（解码 → NV12→RGB → 生成 → RGB→NV12 → VO）在 8/10-bit、limited/full、601/709
下都与参考实现一致到量化误差级别。

## 需要知道的遗留

- **打包仍不含这些修复**：`mpv-playkit/sources.lock.json` 仍 pin
  `mpv-x86_64-playkit-2026.11.23-2`（发布于两个修复之前），`dist/stage/mpv.exe` 也是旧构建。
  用户若装的是分发包，看到的仍是缺陷 1/2 的行为。mpv 侧修复在 `mpv-src` 本地提交
  （`8f58700`），需要推上游并重新出 mpv 构建 artifact 才能进包。
- `vs-dlssg` 侧 `2ea84dd` 是共享代码，VS 直连路径（`frame_gen.cpp` 读 `_Range`）同样受益，
  但四个引擎 DLL 需要一起重编才会一致（`vs-dlssg/src/build.bat`）。
- BT.2020 / HDR(PQ) 路径代码上对齐了 DXGI 枚举，但没有 HDR 素材实测。
- 生成帧在中段仍有几何抖动（源片段 30fps、6× 大幅运动时的光流/内容级问题），
  与色彩无关，未在本次范围内。

## 本地打包验证

修复落到可分发的目录（不压缩、不做 7z）：

```
# 1. 把本次 mpv 构建打成发布用的平铺 zip（DLL 闭包按 objdump 导入表收齐）
python mpv-src\scripts\package_windows.py --build-dir mpv-src\build-msys \
    --msys-bin C:\msys64\clang64\bin \
    --out work\bench_npk\dist\mpv-x86_64-playkit-2026.11.25.zip --version 2026.11.25

# 2. 用该 zip 覆盖 mpv 组件，装配 vsdlssg 全量包 stage（不打包）
set PLAYKIT_MPV_ARCHIVE=<上一步的 zip>
python work\bench_npk\assemble_pack.py vsdlssg 2026.11.25-local

# 3. 用本地四份引擎 DLL 覆盖组件里发行的那四份（yuv.h 是共享代码，必须同源）
copy vs-dlssg\bin\vdlssg.dll vs-dlssg\bin\vxessfg.dll vs-dlssg\bin\vvfg.dll vs-dlssg\bin\vfsr4.dll ^
     mpv-playkit\dist\stage\...-vsdlssg\vs-plugins\

# 4. 冒烟 + 结构校验
python work\bench_npk\smoke_stage.py <stage 目录> vsdlssg
```

产物：`mpv-playkit/dist/stage/mpv-playkit-pre-2026.11.25-local-windows-x64-vsdlssg`
（793 个文件，939 MB）。`BUILD_INFO.json` 里 mpv 组件标了 `overridden: true` 并记录本地 zip 的
sha256/size，不会把这份测试包误认成锁定版本。

装配脚本需要 PyYAML（`.venv` 里没有 pip，用系统 `C:\software\Python312\python.exe`）。
