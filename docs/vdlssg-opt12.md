# vdlssg 优化 1/2 实测：回读融合 + F16C/线程池

日期：2026-11-22
目标：1080p30 源、6× 输出（180fps）。旧基线：m6 RGBS 89.1fps，m6 YUV420P10 93.7fps。

## 改动

- `ngx_session.cpp`：新增持久 `RowPool`（16 线程，caller 参与，按 band 分活，代次检查），替换每次 `parallelRows` 新建 15 个 `std::thread` 的实现。
- `d3d12_ctx.h`：`floatToHalfFast`/`halfToFloatFast` 改用 F16C 标量内建；新增 AVX2 `half8ToFloat8`/`float8ToHalf8`。
- `ngx_session.cpp`：`uploadColor` 的 RGBS 路径按 4 像素 `packRgba4`；`readbackColor` 的 RGBS 路径按 4 像素 `unpackRgba4`；DLSS-G 的 YUV 分支按 chroma 块直接读 RGBA half、写 Y/Cb/Cr，去掉每 band 的 float RGB 中间缓冲（仍保留 `yuvQuant`/`yuvCoefficients`/`yuvStoreSample` 语义）。
- `generate()` 增加 `uploadMs/readbackMs/wallMs/gpuMs/copyMs`；日志格式 `... ms (up .. rb .. gpu .. cp .. wall ..)`。

## 测试口径

`media\sintel_trailer_1080p.mp4`，Trim 0..119（120 源帧），`vspipe -p ... NUL`，`proxy_dir=vs-dlssg\bin\dlss45`，第二轮 build（含 cp 计时）。

## 结果（1080p，YUV420P10，输出 fps / ms 每输出帧）

| multi | 输出帧 | 时间 | 输出 fps | ms/输出帧 | 30fps 源可支持 |
|---|---|---|---|---|---|
| 2 | 240 | 1.72s | 139.61 | 7.16 | 69.8 源fps |
| 3 | 360 | 2.49s | 144.45 | 6.92 | 48.2 |
| 4 | 480 | 3.71s | 129.23 | 7.74 | 32.3 |
| 6 | 720 | 5.60s | 128.57 | 7.78 | 21.4 |

对照旧 build：m6 RGBS 89.1 → 104.5fps（+17%）；m6 YUV 93.7 → 123.6–128.6fps（+32~37%）。

逐帧拆解（m6 slot 2/5）：

```
0.55 ms (up 0.00 rb 1.69 gpu 3.05 cp 2.50 wall 5.30)
slot 1（含 reset+上传）:
12.95 ms (up 3.66 rb 1.80 gpu 6.17 cp 2.50 wall 12.95)
```

- 生成帧 wall ≈ 5.3ms；CPU 回读 ≈ 1.7ms；GPU span 3.05ms，其中 `cp`（output→readback）2.50ms，evaluate 仅 0.55ms。
- `readback_` 是 `D3D12_HEAP_TYPE_READBACK`（`d3d12_ctx.cpp:206-208`），每生成帧 16.6MB RGBA16F 走 PCIe，实测 2.50ms ≈ 6.6GB/s。
- 1080p30 的预算：3×=90fps→11.1ms/帧；4×=120fps→8.33ms；6×=180fps→5.56ms。当前 m3/m4/m6 为 6.92/7.74/7.78ms。

## 第三轮：GPU compute 直接转 YUV420P10

动机：`cp` 的 2.50ms 是 16.6MB RGBA16F 走 READBACK heap（PCIe，约 6.6GB/s）。把 RGBA16F→YUV420P10 放到 GPU，只回读 6.22MB，可同时压掉 `cp` 与 CPU 侧 `rb`。

实现（`vs-dlssg/src/d3d12_ctx.{h,cpp}`）：

- `Gpu::enableYuv420(format, error)`：仅对 DLSS-G 的 4:2:0、8/10-bit、RGBA16F、偶数宽高启用；建 DEFAULT UAV buffer `yuv_`、READBACK buffer `yuvReadback_`、2 描述符堆（SRV=`output_`，UAV=`yuv_`）、根签名（两张 descriptor table + 12 个 32-bit 常量）、运行时 `D3DCompile` 的 cs_5_0 PSO。
- 打包布局：每两个 Y 样本一个 `uint`（`Y0 | Y1<<16`），每个 chroma 样本一个 `uint`（`U | V<<16`）；1080p 共 `(1920*1080/2 + 960*540)*4 = 6,220,800` 字节。
- `recordYuv420Convert()`：`output_`→NON_PIXEL_SHADER_RESOURCE，`yuv_`→UNORDERED_ACCESS，Dispatch `((CW+7)/8,(CH+7)/8,1)`（8×8 线程，每线程一个 2×2 chroma 块），UAV barrier，`yuv_`→COPY_SOURCE，`CopyBufferRegion` 到 `yuvReadback_`。
- `readbackColor` 的快速路径：Map 后 Y 逐行 `memcpy` 到 sink（10-bit 每行 `width*2` 字节，uint 小端正好两个 uint16），UV 逐样本拆低/高 16 位写 U/V（当时 8-bit 仍复用 16-bit 槽；紧凑 8-bit 见第四轮）。
- 注意：`YuvParams` 是 4 个 `uint32` + 8 个 `float` = 12 个 32-bit 常量，root signature 与 `SetComputeRoot32BitConstants` 必须写 12（曾误写 14，越界读 8 字节，已修）。

### 结果

| 路径 | m2 | m3 | m4 | m6 冷跑 | m6 热跑 |
|---|---|---|---|---|---|
| 旧 CPU 融合（opt12） | 139.61 | 144.45 | 129.23 | 128.57 | — |
| GPU compute YUV | — | 147.79 | 153.43 | **180.05** | **223.25** |
| RGBS（旧回读） | — | — | — | 141.39 | — |

- m6 冷跑 720 帧 / 4.00s = 180.05fps，正好 5.56ms/输出帧；热跑 720 帧 / 3.23s = 223.25fps，4.49ms/输出帧。冷/热差约 0.8s，来自 NGX/PSO 初始化与 GPU 时钟爬升。
- m6 稳态逐帧：`0.73 ms (up 0.00 rb 0.55–0.89 gpu 1.77 cp 1.03 wall 3.10–3.43)`；`cp` 2.50→1.03ms，`rb` 1.69→约 0.6ms，生成帧 wall 5.30→3.2ms。
- 反直觉但符合模型：m3（147.8）低于 m6（180–223），因为 reset pass、上传、fence、VS 帧等每对固定成本被 5 张生成帧摊薄；6× 是这条管线效率最高的工作点。
- RGBS 仍走 `CopyTextureRegion`，m6 141.39fps；比 opt12 的 104.47 高，是 F16C/RowPool 的收益，但仍受 16.6MB PCIe 回读限制。

### 正确性

- `work\bench_analysis\check_rgbs.vpy` 走 RGBS，`check_yuv.raw` 走 YUV，同取输出第 300 帧；用 BT.709 limited 10-bit 从 RGBS 反算 YUV 与 shader 输出比较：
  - Y PSNR 53.22 dB（maxabs 16，meanabs 0.736）
  - U PSNR 47.34 dB（maxabs 24，meanabs 2.265）
  - V PSNR 51.20 dB（maxabs 15，meanabs 1.672）
- 范围与对齐正常（Y 40–292，U 508–524，V 500–516），说明打包、10-bit 量化、chroma 块平均正确。非逐位一致的主因是 RGBS 输入的 chroma 由 Bicubic 上采样，而 YUV 输入走 `yuvRowToRgb` 的 bilinear，两者喂给引擎的 RGB 不同；严格逐位 A/B 需临时开关强制旧 CPU 路径。

## 第四轮：force8bit 与 8-bit 紧凑回读

动机：10-bit 布局每样本占 16-bit 槽，8-bit 源有一半是填充；1080p 回读 6.22MB。增加开关让 8-bit 源（或主动降精度的 10-bit 源）走 3.11MB 的紧凑布局。

行为：

- `force8bit=0`（默认）：按源格式自动选择。`format.bits<=8` 用 8-bit 紧凑，`>8` 用 10-bit 布局。
- `force8bit=1`：即使源是 P10，也把 GPU 转换结果量化到 8-bit 并紧凑打包；输出帧仍是源格式（P10 输出），CPU 侧把 8-bit 样本扩展回 10/12/16-bit。这样不改 VapourSynth 输出格式，也不影响 suppressed 帧。
- 参数在 `frame_gen.cpp` 注册为 `force8bit:int:opt`，日志会打印 `force8bit=` 和 `GPU YUV readback: 8-bit packed, 3110400 bytes/frame (forced)`。

实现：

- `d3d12_ctx.cpp` 新增 `kYuv420Shader8`：`[numthreads(8,8,1)]`，每线程 4×2 块；Y 每 uint 4 个样本（`Y0|Y1<<8|Y2<<16|Y3<<24`），UV 每 uint 两对（`U0|V0<<8|U1<<16|V1<<24`）。
- 8-bit 缓冲区 `W*H + CW*CH*2 = 3,110,400` 字节；Dispatch 用 `(width/4+7)/8, (CH+7)/8`。
- `Gpu::setYuvColor` 每帧刷新矩阵/范围；`enableYuv420` 仍负责建资源与初始常量。
- `readbackColor` 的 Y 扩展改为 SSE2 `expand8RowTo16`（每步 8 样本，`unpacklo_epi8` + 左移/右移 + `or`），UV 用 256 项 LUT 去交错；8-bit 输出走 `memcpy`，不扩展。

### 结果（1080p，720 输出帧，m6）

| 路径 | 时间 | fps | 稳态 rb | cp | wall |
|---|---|---|---|---|---|
| 8-bit 源，自动 | 2.17s | **331.12** | 0.48 | 0.49 | 1.94 |
| 10-bit 源，force8bit=1 | 2.38s | **302.30** | 0.70 | 0.49 | 2.19 |
| 10-bit 源，正常 10-bit | 3.06s | 234.94 | 1.30 | 0.97 | 3.31 |

- SIMD 前 force8bit 只有 158–164fps，`rb` 3.05ms；瓶颈是逐样本 `expand8To` 的 3.1M 次标量展开，不是 PCIe。SSE2 后 `rb` 0.70ms，force8bit 反超 10-bit 路径。
- 8-bit 源的 `cp` 0.49ms 已是 3.11MB 的 PCIe 下限附近；剩余 `wall` 主要是 evaluate 与宿主帧开销。

### 正确性

- 同取输出第 400 帧，force8bit=1 vs 正常 10-bit：Y PSNR 57.78dB、U 53.90dB、V 55.55dB，maxabs 3/4/4；第 300 帧为 8-bit 对齐内容，逐位一致。
- 8-bit 量化对 10-bit 源有约 54–58dB 的损失；追求画质时保持 `force8bit=0`，它只在源本身 ≤8-bit 时启用紧凑路径。

## 结论

- 1080p30 的 6×（180fps）已在 YUV420P10 + GPU compute 路径上达标：冷跑 180.05fps，热跑 223.25fps；3×/4× 分别 147.8/153.4fps，余量充足。
- 8-bit 紧凑路径进一步把 m6 推到 302–331fps（`force8bit=1` / 8-bit 源），回读 3.11MB、`rb` 0.5–0.7ms；默认按源位深自动选择，10-bit 源仍走 6.22MB 的 10-bit 布局。
- 当前生成帧成本 3.2ms：`cp` 1.03ms（6.22MB）、`rb` 0.6ms、GPU span 1.77ms（evaluate 0.73ms）；已从“PCIe 搬运瓶颈”转为“宿主每输出帧开销”。
- 要进一步留余量或上 4K，需要 GPU 驻留输出（原生 D3D11/D3D12 hw-frame 滤镜，参考 `vf_animejanai.c`），避免每个生成帧变成 VS CPU 帧；1080p 当前已可实时。

## mpv 参考插件

用户说的 “database” 是 `the-database/mpv` fork，不是数据库文件。核心是 `video/filter/vf_animejanai.c`：

- 头注释（1-36 行）：帧全程留在 GPU；输入 `IMGFMT_CUDA/D3D11/VULKAN`，输出自有 `AVHWFramesContext` 池，同格式。
- 输出/源/工作三个池都用 `AV_PIX_FMT_D3D11`，`D3D11_RESOURCE_MISC_SHARED | SHARED_NTHANDLE`，`BindFlags = RENDER_TARGET | SHADER_RESOURCE`（1431-1523 行）。
- 管线 queue depth 3（默认），`aji_infer` 只提交，`aji_flush`/`aji_wait` 按 ticket 等完成（26-30 行；1174、2655 行）。
- D3D11 解码纹理不可共享/不同步，先用 GPU copy 到自有 SHARED|NTHANDLE 纹理；只有采样/分析用 staging 回读（`ajn_sample_gpu.c:16,74-77,144`）。
- RIFE 链仍同步，因为场景切换需要每对一次 CPU 回读（30 行）。

这正是 3/4 的模板：输入输出都用 hw frame，NGX 的 D3D12 纹理通过 shared NT handle 与宿主互操作，避免 CPU 回读。
