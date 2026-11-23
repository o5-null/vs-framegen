# D3D12 NVOF + DLSS-G 共享多帧测试

日期：2026-11-23

## 目的

验证在 `vs-dlssg` 的 D3D12 层接入 NVIDIA Optical Flow（NVOF），把真实光流写入
DLSS-G 的 `pMVecs`，能否改善 mpv `vf_playkit` 6× 共享纹理路径的中间帧质量。

## 实现

- `vs-dlssg/src/nvof_motion.{h,cpp}`：NVOF 5.0 D3D12；P010→`R8_UNORM` luma，
  `R16G16_SINT` flow，S10.5 展开为 `int16/32.0f`，再写 `R16G16_FLOAT` motion。
- `d3d12_ctx.{h,cpp}`：新增 `recordLuma(slot,dst)` 和 `shareLumaPso_`。
- `ngx_session.cpp`：`generateTexturesMulti` 在 `fillConstantInputs` 后执行 NVOF，
  输出循环使用 `copyMotion=true`。
- `bridge.cpp`：`VDLSSG_NVOF=1` 强制启用，`2` 为 auto；默认关闭。
- 构建：`vs-dlssg/src/build.bat`；产物 `vs-dlssg/bin/vdlssg.dll`（186,880 B），
  复制到 `mpv-src/build-msys/dlss45/vdlssg.dll`。

## 结果

### 合成 spike

`nvof_spike.exe` 用 640×360、方块右移 8 px、`input=cur`、`reference=prev`，
grid=4，测得 mean flow `(-8.033, -0.014) px`，方向/尺度正确。

### 真实片段

命令（不要加 `--o`，否则 mpv 编码 profile 强制 `vo=lavc` 并失败）：

```bash
./mpv.exe --no-config --hwdec=d3d11va --d3d11-adapter=#1 \
  --vo=gpu-next --gpu-context=d3d11 \
  --vf=playkit=multi=6:log=yes:dll=dlss45/vdlssg.dll:proxy_dir=dlss45 \
  --frames=2 --start=4.0 "测试数据/ah_1080p10.mkv"
```

`VDLSSG_NVOF=1` 时：

```text
[vdlssg] NVOF enabled (grid 4)
NVOF flow stats mean=(-48.085,-14.326)px range=[-5280,456]x[-2592,400] nonzero=100.0%
NVOF flow 480x204 -> motion 1920x816
```

flow 非零，但含大量离群值（原始 S10.5，min=-5280 即 -165 px）。

### 输出对比

`pk_dump_rgba` 导出 `out0..out4.ppm`（1920×816）：

| 对比 | MAE |
|---|---:|
| out0→out1 | 0.089 |
| out1→out2 | 0.035 |
| out2→out3 | 0.035 |
| out3→out4 | 0.015 |
| out0→out4 | 0.144 |
| NVOF on vs off（同索引） | 0.002–0.007 |

5 张输出几乎相同。源图 `s1→s2` 的 MAE 为 4.81，说明源片段有真实运动；输出与源图
的 MAE 约 39，主要是 P010→RGBA 色偏。

### 对照实验

1. 把 `fillConstantInputs` 的 motion 改为每像素 half `0x4000`（常量 2.0 px 平移），
   depth 仍 0.5：输出 MAE 与 NVOF 路径几乎完全一致，仍无真实插帧。
2. 再把 depth 改为 `0.5..0.7` 横向渐变，motion 仍 2.0：输出与常量 depth 版本
   MAE 0.001，仍无变化。

## 结论

- NVOF 本身可用：合成测试方向/尺度正确，真实片段 flow 非零。
- 当前 DLSS-G **共享多帧路径**没有消费外部 `pMVecs`/`pDepth`，或运行库直接抑制了
  所有中间帧（5 张近似等于 cur）。
- 因此 **NVOF 不能提升这条 6× 链路的质量**；继续优化 NVOF flow 没有意义。
- 若要质量，应改回 RIFE/VS 路线，或继续查 DLSS-G 共享路径为何抑制中间帧
  （需要真实 depth/camera，或改用 `generateTextures` 单帧路径对照）。
- `nvOFDestroy` 在 5.0 驱动上退出时崩溃；`NvOfMotion::shutdown()` 默认只 unregister，
  不调用 destroy，也不 `FreeLibrary`，避免进程退出崩溃。

## 代码状态

- `VDLSSG_NVOF` 默认关闭；未启用时原 6× 路径不受影响。
- `fillConstantInputs` 的诊断常量已还原为 depth 0.5、motion 0。
- `nvof_motion.cpp` 的 flow stats 仍保留用于调试。
- 所有改动未提交、未 push。
