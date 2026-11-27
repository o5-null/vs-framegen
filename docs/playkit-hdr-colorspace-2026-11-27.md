# vf_playkit：生成帧与源帧的色彩/HDR 不一致（2026-11-27）

## 现象

`vf=playkit` 生成的帧和源帧交替显示时画面闪烁、明暗与色偏跳变。SDR BT.709 素材
基本正常，**BT.2020 / HDR10 素材闪烁明显**：生成帧更亮（范围被拉开）、发绿。

## 定位过程与结论

### 1. 生成帧的 Y 范围被拉成 full range

| 素材（640x360） | 源帧 Y (min/max) | 生成帧 Y（修复前） |
|---|---|---|
| sdr709_ramp（bt709/tv） | 115.02 (16/214) | 115.15 (16/214) ✅ |
| sdr2020_ramp（bt2020nc/tv） | 115.02 (16/215) | 115.39 (**0/231**) ❌ |
| hdr10_ramp（pq/bt2020/tv） | 114.82 (17/213) | 115.09 (**1/230**) ❌ |

`PLAYKIT_OUTRANGE=full` 时两者都原样给出 0..231，说明引擎输出的是 0..255 的 RGBA8，
`16..215` 被线性拉成 `0..231`（`(v-16)*255/219`）。`out=STUDIO` 时驱动会把 BT.709
压回 `16..214`，但 **BT.2020 完全不压**。

### 2. 这台机器的驱动 BT.2020 YCbCr 输出级是坏的

用 `PLAYKIT_INCS`/`PLAYKIT_OUTCS` 直接指定 DXGI 色彩空间枚举（`dxgicommon.h:35-62`），
对同一 HDR 素材逐组合测 `dst` 的 Y 与 Cb/Cr：

```
in=0x0  out=0x8 :: y=115.07 (16..214) cb=128.00 cr=128.00   ✅ 709 studio
in=0x11 out=0x2 :: y=115.07 (16..214) cb=128.00 cr=128.00   ✅ RGB studio 709
in=0x11 out=0xa :: y=115.09 ( 1..230) cb=115.37 cr=115.02   ❌ YCbCr studio 2020
in=0x11 out=0xd :: y=115.09 ( 1..230) cb=115.37 cr=115.02   ❌ YCbCr studio 2084 2020
in=0x11 out=0x3 :: y=115.09 ( 1..230) cb=115.37 cr=115.02   ❌ RGB studio 2020
in=0x0  out=0xb :: y=115.09 ( 1..230) cb=115.37 cr=115.02   ❌ YCbCr full 2020
```

即凡 `out_cs` 属于 P2020 系列：**既不做 studio 范围压缩，又把中性灰写成 Cb/Cr≈115**
（应为 128）—— 中性灰偏绿/偏暗，这正是显示路径 `deltas=0.1657` 的来源。`0xe/0xf`
的数值与 `0x9`（709 full）一致，是驱动对不支持组合的静默回退。

驱动环境：RTX 3080，DriverVersion `32.0.16.1742`（2026-09-30）。

### 3. 修法

**mpv 侧（`mpv-src/video/filter/vf_playkit.c`）**

- `pk_update_colorspace()` 按 `matrix`/`pq`/`full` 选输入输出色彩空间，HDR 用
  `RGB_FULL_G2084_NONE_P2020` / `YCBCR_STUDIO_G2084_LEFT_P2020`，SDR 2020 用
  `RGB_FULL_G22_NONE_P2020` / `YCBCR_STUDIO_G22_LEFT_P2020`，709 走原路径。
- **两段式 Blt**（`PLAYKIT_1PASS` 可关）：先 `RGB_FULL → RGB_STUDIO` 把 full 的
  引擎输出压成 limited 码值（驱动对 RGB studio 输出的压缩是正确的），再
  `RGB_FULL(P2020) → YCbCr STUDIO(P2020)` 出帧。这样 Y 范围正确，但色度仍受 3 中缺陷影响。
- **BT.2020 改由引擎直出 YCbCr**（默认开启，`PLAYKIT_NOYUV` 可回退）：out 纹理直接
  用 `DXGI_FORMAT_P010/NV12`，`pk_frame()` 只做 `CopySubresourceRegion`，完全绕开
  驱动的 P2020 YCbCr 输出级，同时省掉每帧全分辨率 Blt。
- `pk_luma_stat()` 增加 Cb/Cr 回读（P010 的 plane1 在同一 subresource 内，偏移
  `RowPitch*Height`，U 低位 / V 高位 `>>8`），让缺陷在测试里可量化。

**引擎侧（`vs-dlssg/src/d3d12_ctx.{h,cpp}`）**

- 新增 `kRgbToYuv420Shader`：`Texture2D<float4> Src:t0` + `RWTexture2D<float> DstY:u0`
  + `RWTexture2D<float2> DstUV:u1`，8x8 线程每 2x2 块写 4 个 luma 与 1 个 chroma，
  系数/量化取 `ShareParams`（BT.2020 的 kr/kg/kb、studio 偏移与缩放、maxValue）。
- `shareYuvRoot_` + `shareOutYuvPso_`；`openSharedOutput()` 按打开的纹理格式置
  `shareOutIsYuv_`；`recordConvertOutput()` 走 YUV 分支：写本地 R16/R16G16 平面纹理，
  再 `CopyTextureRegion` 到共享 P010/NV12 的 subresource 0/1。
- 坑：`cbuffer` 字段顺序必须与 `struct ShareParams` 完全一致（`width..chromaHeight`
  在前，量化参数次之，`kr/kg/kb/maxValue` 在后）。写反时 dispatch 链本身是通的
  （`VDLSSG_YUV_CLEAR=1` 清 0.5 能出 `y=127 cb=cr=127`），但 Y 恒 0、色度错乱 ——
  常量错位很容易误判成"共享纹理没打开"。

### 4. 验证

| 项 | 源帧 | 生成帧 |
|---|---|---|
| hdr10_ramp Y | 114.82 (17/213) | 114.67 (17/213) |
| hdr10_ramp Cb/Cr | 128.00 / 128.00 | 127.41 / 127.85 |
| hdr10_red Y/Cb/Cr | 81 / 90 / 240 | 79 / 93 / 236（用 BT.2020 系数，彩色不偏） |
| sdr709_ramp | 115.02 (16/214) | 115.15 (16/214) |
| sdr2020_ramp | 115.02 (16/215) | 115.10 (16/214) |
| 显示路径（4 帧交替） | — | `deltas` 全 < 0.004 |

## 测试

`test/suite/lib/playkit.py`（解析 `playkit:` 日志的 luma/色度/色彩空间/矩阵，及
`vf_chain()`、`make_clip()`）+ `test/suite/cases/test_75_playkit.py` 五条用例：

1. BT.2020 SDR 生成帧保持 limited 范围；
2. HDR10 生成帧用 PQ + limited 范围；
3. 生成帧保持中性色度（BT.709 对照）；
4. 生成帧保持中性色度（HDR）；
5. 显示路径上源帧/生成帧交替不闪烁（截图 → `metrics.deltas`）。

第 3、4、5 条在修复前精确失败（`cb=115.13 cr=114.83`、`deltas=[0.1657,...]`），
修复后全绿。跑法：`.venv\Scripts\python.exe -m pytest test\suite\cases\test_75_playkit.py -q`。

顺带修的两处测试设施问题：

- `test/suite/lib/display.py`：`frame-step` 可能在文件加载完成前发出（`[ipc]
  ('frame-step',): error running command`），新增 `_wait_loaded()` 轮询
  `video-params/w` 再开始逐帧。
- `test/suite/cases/test_70_vdlssg.py`：插件的 `-j`/`vs.log` 属性采集在重跑时是追加的，
  重试前需清 `vs.log`/`props.json`。

## 未解决：DLSS-G 后端在进程收尾随机崩溃

全量套件曾出现 `test_70_vdlssg.py::test_frame_gen_writes_duration_props` 随机失败，
`rc=4294967295`（-1）。直连 `subprocess.run` 为 0，走套件（`monitor+gpu`）约 1/3–1/2
概率出现，失败耗时 5.5–10s（正常 2.1s），stderr 只有那条 `_SceneChangePrev` 警告和
`Output 4 frames`。Windows 事件日志给出根因：

```
出错应用程序名称： vspipe.exe ... 出错模块名称： sm86_backend.dll
异常代码： 0xc0000005   错误偏移： 0x000000000002b042
```

即 **NVIDIA DLSS-G 后端在帧全部输出之后的收尾阶段随机访问违例**。用 `git stash` 生成
不含本轮改动的基线 DLL（224768 字节 vs 当前 232448 字节）跑 6 次，同样出现
`rc=-1 9.60s`，证明与本次色彩改动无关；`nvof_motion.cpp:88-90` 已有同类注释
（驱动 faulty nvOFDestroy 在进程收尾崩溃）。

处理方式：`test_70_vdlssg.py` 先重试，仍出现"帧已输出但退出码非 0 且无插件错误字样"
时，记一条 `warnings.warn` 并继续校验产物（不再判失败）。这是第三方缺陷，插件侧没有
可靠修法；试过跳过 `NVSDK_NGX_D3D12_Shutdown1`，反而更糟（3/6 崩溃），已回退。

## 改动文件

- `mpv-src/video/filter/vf_playkit.c`（两段式 Blt、色彩空间选择、`yuv_out` 直拷、
  Cb/Cr 统计、`PLAYKIT_1PASS`/`PLAYKIT_NOYUV`/`PLAYKIT_INCS`/`PLAYKIT_OUTCS` 诊断开关）
- `vs-dlssg/src/d3d12_ctx.h`、`vs-dlssg/src/d3d12_ctx.cpp`（P010/NV12 直出）
- `test/suite/lib/playkit.py`、`test/suite/cases/test_75_playkit.py`（新增）
- `test/suite/lib/display.py`（IPC 加载竞态）、`test/suite/lib/paths.py`（`dev_mpv()`）
- `test/suite/cases/test_70_vdlssg.py`（重试 + 第三方崩溃容忍）

## 复现命令

```
powershell -File work\build_mpv.ps1          # 只重编 vf_playkit.c
powershell -File work\sync_dev_mpv.ps1       # 拷到 work\mpvdev
cmd /c "cd vs-dlssg\src && build.bat"        # 四个引擎 DLL
.venv\Scripts\python.exe -m pytest test\suite -q
```

结果：`53 passed`（约 41s）。
