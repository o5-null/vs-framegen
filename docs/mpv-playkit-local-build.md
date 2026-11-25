# mpv fork：vf_playkit 本地构建与 pass-through 验证

## 结论

已在本地 MSYS2 CLANG64 环境构建 `o5-null/mpv` fork，`vf_playkit` 以 D3D11 硬件帧完成 6× 复制，PTS 间隔为 `1/180 s`，不依赖 GitHub Actions。

- mpv 源码：`E:\code\dlssg-vfi\repos\mpv-src`
- 本地提交：`19694d5 feat(vf): add optional software output format conversion`（在 `4549b1d` 之上，未 push）
- 二进制：`E:\code\dlssg-vfi\repos\mpv-src\build-msys\mpv.exe`、`mpv.com`、`libmpv-2.dll`
- 构建方式：MSYS2（`C:\msys64`，CLANG64）+ Meson/Ninja；脚本 `E:\code\dlssg-vfi\build-mpv.sh`

## 滤镜行为

`vf_playkit` 目前是 **pass-through + 6× 复制**，并带一个可选的软件格式转换，尚未接 DLSSG：

- 接受任意 `mp_image`：软件 `yuv420p`/`yuv420p10`/`rgb`，硬件 `d3d11[nv12]`/`d3d11[p010]`；复制时只增加引用。
- `outfmt=<imgfmt>` 可要求软件帧在复制前转成目标格式（`mp_sws_scale`）；D3D11 硬件帧不做转换，会报错并提示在 `playkit` 后接 `format` 滤镜。
- 保存 `prev` 与最多 6 帧队列；按 `prev->pts + k*(cur->pts-prev->pts)/multi` 生成中间帧，`multi` 取 2..6。
- 首帧直通，因此 N 帧输入输出约 `6N-5` 帧（5.83×），不是严格 6N。
- EOF 时先排空队列，再转发 `MP_EOF_FRAME`。

## 构建

```bash
C:\msys64\usr\bin\bash.exe /e/code/dlssg-vfi/build-mpv.sh
```

脚本内容：

```bash
export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH
cd /e/code/dlssg-vfi/mpv-src
meson setup build-msys --prefix=/clang64 -Dlibmpv=true -Dd3d11=enabled -Dvulkan=enabled -Dshaderc=enabled
meson compile -C build-msys
```

依赖已通过 pacman 安装（ffmpeg 9.1、libplacebo 7.387、libass、luajit、vulkan-headers、shaderc 等）。pacman 镜像改为清华源，否则 `mirror.msys2.org` 会报 TLS EOF。

## 验证

```bash
E:\code\dlssg-vfi\run-playkit.sh
```

关键输出：

```text
[playkit] playkit: 1920x816 imgfmt=d3d11 hw_subfmt=nv12 pts=0.000000 multi=6
[playkit] playkit: out #1 pts=0.000000
[playkit] playkit: out #2 pts=0.005500
...
[playkit] playkit: out #7 pts=0.033000
```

10 秒吞吐（`bench-playkit.sh`，`--frames=1800 --untimed`，含 D3D11 VO）：

```text
real 0m10.797s  → 1800 输出帧，约 166.7 fps
```

格式矩阵（`work/test_outfmt.sh`，`multi=3`，`--frames=12`）：

| 输入 | `outfmt` | 结果 |
|---|---|---|
| 软件 `yuv420p` | `p010` | `VO: [gpu] 1920x816 p010` |
| 软件 `yuv420p10` | `nv12` | `VO: [gpu] 1920x816 nv12` |
| D3D11 `p010` | 无 | `VO: [gpu] 1920x816 d3d11[p010]`，PTS 间隔 `1/180 s` |
| D3D11 `p010` | `nv12` | 报错 `cannot convert hardware p010; put a format filter after playkit instead`，随后停止 |

P010 硬件直通是后续 DLSSG 零拷贝的主路径；软件 `outfmt` 只用于测试/兼容，不进入零拷贝链路。

## 下一步

1. 在 `vf_playkit` 内把 D3D11 纹理经 `SHARED_NTHANDLE` 共享给 D3D12，接 `NgxSession` 的 RGBA16F 输入/输出。
2. 需要 motion vector/depth 时先补零 MV 或从解码侧取；当前 mpv 链没有。
3. 发布版从 mpv 仓库出，只在用户明确要求时触发构建/发布。
