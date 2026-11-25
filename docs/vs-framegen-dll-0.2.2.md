# vs-framegen DLL 0.2.2：VVFG 换档与吞吐、桥内 seed 复用、YUV 全范围偏色修复

2026-11-26

本版是 `v0.2.1`（`0d152c8`）之后的累积发行，源码提交范围 `0d152c8..e746fd7`。

## 本版改动

按引擎归类的完整清单写进 release 正文（草稿在 `work\release-0.2.2.md`），要点：

- **VVFG**：新增 `mode=0|1|2`、默认改 medium（`4fc926a`）；源帧一份 `cuMemAllocManaged`
  跨 lane 共享（`4fc926a`）；同对内 in-between 改成按 lane 流水（`d5822b0`，1080p medium
  multi=6 5.64→4.18 ms/输出帧，输出逐字节相同）；打包/解包 AVX2（`2156189`）。
- **vdlssg 桥**：跨对复用 seed 省一次 evaluate、`VDLSSG_TIMING` 补 `rec/wait/gpu_eval/gpu_total`、
  转换路径实验开关 `VDLSSG_CONV_LOCAL` / `VDLSSG_CONVPROBE` / `VDLSSG_SKIP_CONVERT`（`2d2090a`）。
- **YUV**：full range 色度缺 127.5 中点偏移，全范围源整体偏品红（R/G/B 236/5/253），
  已在 `yuv` 读写两侧按 `maxValue/2` 居中（`2ea84dd`）。
- **mpv 宿主**：四个 `MEMC_*.vpy` 只剩用户选项 + 一次 `FRAMEGEN_VPY`，逻辑进 K7sfunc 包
  （`55d3811`），`k7f_vpypar.lua` 同步叠滤镜修复（`15263a9`）。
- **构建规则**：Python 环境与包管理统一走 uv，`.venv*` 一并忽略（`4ebc902`，见 `AGENT.md`）。

同时补了两份调研：`docs\vs-vulkan-gpu-path.md`（VS R80+ `vnode:gpu` 驻留模型与实测）与
`docs\vs-fg-vulkan-feasibility.md`（四引擎搬到 Vulkan 的拷贝代价，结论：vvfg 可真零拷贝、
vfsr4 一次显存内拷贝、vdlssg/vxessfg 受 D3D12 无反向导入限制）。

## 构建与打包

- 构建：`vs-dlssg\src\build.bat`，`exit=0`，四个 DLL 全部产出，只剩既有的 C4996 `getenv` 警告；
  日志 `work\build-0.2.2.log`。
- DLL 时间戳 2026-11-26 00:24:41–00:24:53，晚于最后一次提交（00:24:19），确认打进的是 HEAD 源码。
- 打包脚本 `work\pack-0.2.2.ps1`：取 `vs-dlssg\bin` 的四个 DLL + 四棵引擎，剔除
  `*/dlssg_sm86/logs/*.log` 与 `fsr4\shader_cache`，`tar -a` 压成 `vs-plugins\` 布局。
- 资产 `work\vs-framegen-0.2.2.zip`：**253,832,410 B**，
  `sha256 bfa079362644a532b15be7a1c8796c653f9c7edbfd5d53f0db5e341c8038d289`
  （0.2.1 为 253,831,573 B / `578ccafe…`，差 837 B，只有四个 DLL 变了）。

## 验证（本机，RTX 3080 10GB）

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| 四插件加载 | `work\smoke-0.2.2.py`（`.venv`，R74 / API 4.2） | 四个都 `ok ['FrameGen']` |
| 真实出帧 | `.venv\Scripts\vspipe.exe -p test\mf6.vpy NUL` | `exit=0`，288 帧 1.67 s（171.96 fps） |
| 转换路径开关 | 同上 + `VDLSSG_CONV_LOCAL=1 VDLSSG_CONVPROBE=1`（`work\smoke-local.ps1`） | `exit=0`，288 帧 1.62 s（177.42 fps） |

两个 run 的 stderr 都有相同的既有噪声，与本次改动无关（0.2.1 也有）：
`[NGXLoadLibrary:332] error: failed to load NGXCore: 126 (…\.venv\…\nvngx.dll)` 与
`[NGXSecureLoadFeature:1384] Feature dlssg failed to load (cmsid …) from cache`。

## 桥路径补验（vf_playkit，2026-11-26）

上面两个 `vspipe` 冒烟走的是 CPU 路径 `generate()`，覆盖不到本次两处改动——它们都在共享纹理桥
`generateTexturesMulti()` 里（`captureTexture` 为真时才调 `recordConvertOutput()`）。补验改用带
`vf_playkit` 的 mpv（`work\mpv-x86_64-playkit-20261123.zip` 解到 `work\pk-0226`，把新编的
`vdlssg.dll` + 四棵引擎放进其 `vs-plugins`），命令：

```
mpv.exe --no-config --hwdec=d3d11va --vo=gpu-next --gpu-context=d3d11 \
  --d3d11-flip=no --d3d11-sync-interval=0 --autofit=960x540 \
  --vf=playkit=multi=6:log=no:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45 \
  --start=2.0 --end=4.0 --untimed media\sintel_trailer_1080p.mp4
```

日志里确认 `playkit: vdlssg bridge ready 1920x1080 nv12 bits=8 multi=6`，即真的进了桥。

**1. 跨对 seed 复用（`VDLSSG_TIMING=1`，83 对）**

| 对 | conv_prev | seed |
| --- | ---: | ---: |
| 第 1 对 | 1.15 ms | 8.36 ms |
| 第 2 对起（82 对） | 0.00 ms | 0.00 ms |

即判据按预期生效：首对补 seed，之后每对省掉 `conv_prev` 转换 + 整次 seed evaluate ≈ **9.5 ms/对**，
相对 1080p multi=6 约 38.6 ms/对 的桥内耗时约 **1/4**。

**2. 转换路径开关（`VDLSSG_CONV_LOCAL=1`）**

`convertOutput` 行 415 条：clean 全部 `local=0`，`VDLSSG_CONV_LOCAL=1` 全部 `local=1`，
无 `outLocal create failed`，`exit=0`。带 `PLAYKIT_TIMING=1` 各跑一遍（83 对）：

| 配置 | process 中位 | bridge wall 中位 | copy_wait 中位 |
| --- | ---: | ---: | ---: |
| clean（直接 UAV 写共享纹理） | 38.57 ms | 36.80 ms | 0.44 ms |
| `VDLSSG_CONV_LOCAL=1`（本地纹理 + CopyResource） | 37.88 ms | 36.04 ms | 0.41 ms |

**结论：差别在噪声内（≈2%），"共享纹理上直接开 UAV 是那 5 ms 的根因"这个假设被否掉**——否则
绕开它的本地纹理路径应当明显更快。开关保留为诊断用，默认仍是直接写共享纹理。

**3. `convOut` probe 的数字不可信**：`c32=5.6–6.9 ms` 那段夹着一条无条件的
`fprintf(stderr, "convertOutput …")`，stderr 被重定向到文件时会阻塞，probe run 的每对耗时因此被
抬到 90–160 ms。看 probe 分段时先忽略 `c32`，或把那条 fprintf 挪出计时区间。

## 仍然没有验证到的部分

1. "分辨率变化时重建本地纹理"（`d3d12_ctx.cpp:836`）：需要同一进程内切换分辨率，本轮没构造。
2. VVFG 的换档/流水收益是提交时的实测数（`4fc926a`、`d5822b0` 的 commit body），本会话没有复测；
   本轮只确认四个 DLL 能加载、`vspipe` 与桥两条路径都能正常出帧。

## 发行

- 依赖侧先发 `k7sfunc`：版本号 `1.9.0+pre.9`（`83a830f`，补齐 `mod_vpy` 宿主层、`[vpy-debug]` 探针
  与若干修复）已推送到 `o5-null/K7sfunc` main；`mpv-playkit` 按 commit 钉这一版。
- 本仓库 main 推到 `06a2a01`，tag `v0.2.2` 已推；release
  <https://github.com/o5-null/vs-framegen/releases/tag/v0.2.2>，附件
  `vs-framegen-0.2.2.zip`：**253,832,410 B**，
  `sha256 bfa079362644a532b15be7a1c8796c653f9c7edbfd5d53f0db5e341c8038d289`
  （GitHub 侧资产 digest 与本地一致）。
- 发行时的 tag 指向 `06a2a01`；本条记录随后单独提交，所以 main 会在 tag 之后多一个 docs 提交。
- 下一步（不在本仓库）：`mpv-playkit` 的 `sources.lock.json`（`vs-framegen-dlls` 与
  `engine-framegen` 两条）升到 0.2.2 与新 sha256、`k7sfunc` 钉到 `83a830f`，
  `PLAYKIT_LOCAL_VS_FRAMEGEN_DLLS` 覆盖件才可以撤掉。

## 教训

- CPU 路径与共享纹理桥是两条独立的调用链（`generate()` / `generateTexturesMulti()`），同一个
  引擎改动容易只落在其中一条上；冒烟前先确认测试走的是哪条，否则"跑了 288 帧"给不了任何保证。
- 开关型实验代码要打印一条无条件的一行日志（这次是 `convertOutput share=… local=…`），
  否则"没输出"与"没进入分支"分不清。
