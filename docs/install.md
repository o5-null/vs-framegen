# 安装与使用（vs-framegen）

本文件是**所有 release 共用的安装说明**，release 正文只写这一版改了什么。装法只有两种：

| 场景 | 用哪个 |
|---|---|
| 只想直接看效果 | **mpv-playkit-pre 懒人包**（推荐） |
| 自己有 mpv / VapourSynth 管线要接 | **K7 包**（K7sfunc 的 mpv 覆盖层）+ 本仓库 release 的 `vs-plugins` 附件 |

## 1. 推荐：mpv-playkit-pre 懒人包

<https://github.com/o5-null/mpv-playkit-pre>

解压即用的 Windows x64 便携 mpv（自带 Python、VapourSynth、K7sfunc 预设层），不写注册表、不打安装器。
它只通过 GitHub Actions artifact 交付、不发 release，取最新的
`mpv-playkit-pre-<版本>-windows-x64.zip` 解压即可。

帧生成插件 DLL 已经在基础包里；引擎（以及可选的推理档）是单独的补丁包，把同一个 artifact 列表里的
`mpv-playkit-pre-<版本>-windows-x64-pack-vsdlssg.zip` 解压到**同一个根目录覆盖合并**（默认档，约 43 MB）：

```text
mpv-playkit-pre-<版本>-windows-x64\
  mpv.exe
  portable_config\              ← K7sfunc 预设层、脚本
  vs-plugins\
    vdlssg.dll  vxessfg.dll  vvfg.dll  vfsr4.dll   ← 基础包自带，脚本首次使用时 LoadPlugin
    dlss45\                     ← 来自 pack-vsdlssg：DLSS-G（6×）代理 + 运行库
    xessfg\                     ← 来自 pack-vsdlssg：XeSS-FG（4×）
```

需要 TensorRT-RTX 超分/推理的，再叠 `-pack-vs-mlrt.zip`（~841 MB，装到
`Lib\site-packages\vapoursynth\plugins\vsmlrt\`）。

`Ctrl+Shift+p` / `Ctrl+Shift+o` 是 PLAYKIT 的超分/插帧预设开关（走 RIFE / RIFE_AJI），
**不包含这四个帧生成引擎**。帧生成要在 mpv 里单独挂：基础包已经带 `portable_config\vs\MEMC_DLSSG_NV.vpy`
这类脚本，加一条滤镜即可，例如

```ini
# input.conf
y  vf toggle vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"
```

脚本里的 `Multi` / `Preset` / `Scene` 等默认值可以改文件，也可以用 `vf` 的 user-data 覆盖；
播放中调参用 `k7f_vpypar.lua`（见第 2 节）。四个 `MEMC_*.vpy` 分别对应 DLSS-G / XeSS-FG / VFG / FSR4。

## 2. 自行搭建：K7 包

<https://github.com/o5-null/K7sfunc>

K7 包指 K7sfunc 的 `mpv\portable_config` 覆盖层（`vs\` 下的 `MEMC_*.vpy`、`scripts\` 下的
`k7f_*.lua`、`script-opts\`）。把它覆盖到自己 mpv 的 `portable_config`，再从本仓库 release 的
`vs-framegen-<版本>.zip` 取 `vs-plugins\`（四个 DLL + 四个引擎包）。

不建议自己从零拼 `vpy`：参数解析、`proxy_dir` 查找、OSD 调参、算力不足降级都在 K7sfunc 的
`MEMC_DLSSG_NV.vpy` / `k7f_vpypar.lua` / `k7f_memc_guard.lua` 里，直接改那三个文件比复刻省事。

### 引擎包位置

引擎包与 DLL 放在同一层（`vs-plugins\<引擎>\`），插件默认就按 `<DLL 所在目录>\dlss45` 找，
手动 `LoadPlugin` 不用写 `proxy_dir`：

```text
<mpv 根目录>\vs-plugins\
  vdlssg.dll
  dlss45\version.dll             ← 与 DLL 同级
  dlss45\dlssg_sm86.ini
  dlss45\dlssg_sm86\runtime\nvngx_dlssg.dll
  dlss45\dlssg_sm86\kernels\fg_kernels.dll
```

K7 的 `MEMC_*.vpy` 为了兼容会先试 `vs-plugins\bin\<引擎>` 再试同级，命中哪个就把绝对路径传给插件。
所以把引擎包多套一层 `bin\` 也能用，但不是发行包的布局；不要把两种混着放，更不要出现
`vs-plugins\bin\dlss45\bin` 这种双 `bin`。

## 3. release 附件

只有一个包：

| 文件 | 内容 |
|---|---|
| `vs-framegen-<版本>.zip` | 四个插件 DLL（`vdlssg` / `vxessfg` / `vvfg` / `vfsr4`）+ 四个引擎包（`dlss45` / `xessfg` / `vfg` / `fsr4`），都在 `vs-plugins\` 下、与 DLL 同级 |

每个 release 正文里附 sha256。覆盖安装会保留你自己的 `portable_config`，只动 `vs-plugins\`。

`vvfg` / `vfsr4` 的引擎包是 NVIDIA Video Effects 与 AMD FidelityFX 的原厂二进制（体积大，`vfg` 约
200 MB），release 包里随附，各自目录下带 `licenses\`；单独分发时按各自许可处理。

## 4. 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `multi` | 2 | 生成倍率；`vdlssg`/`vvfg` 2–6，`vxessfg` 2–4，`vfsr4` 2 |
| `mv` | 无 | 外部运动矢量 clip（两平面，浮点），不给则由插件自己估 |
| `scene_threshold` | 0.15 | 切镜检测阈值，超过就不生成中间帧 |
| `proxy_dir` | `dlss45` | 引擎包目录名，相对 DLL 所在目录 |
| `gpu` | 无 | 指定适配器；多卡机器建议不填，插件跟随代理 install 记录里的卡 |
| `camera` | 0 | 0 自动 / 1–3 强制某种相机模型；一般不动 |
| `log` | 0 | 1 时每帧在 stderr 打计时与槽位 |
| `force8bit` | 0 | 1 时把输出压回 8 bit |
| `preset` | 无 | 仅 `vdlssg`：`a` 或 `c`；不给就沿用引擎包里的值（出厂是 `a`） |

直接调用示例：

```python
core = vs.core
core.std.LoadPlugin(r"D:\mpv\vs-plugins\vdlssg.dll")
out = core.vdlssg.FrameGen(src, multi=6, preset="c")
```

## 5. 模型预设（`preset`，仅 DLSS-G）

预设不是 NGX API 参数，而是引擎包里 `dlssg_sm86.ini` 的 `[DLSSG] Preset` 键，解锁代理在装载
`version.dll` 的那一刻钉死。插件因此在装载代理**之前**改写这个值：

| 值 | 语义 | 1080p 6× 每输出帧（RTX 3080） |
|---|---|---|
| `a` | 出厂模型 | wall 6.4 ms / GPU 3.9 ms / p50 3.1 ms |
| `c` | FG Lite，生成帧的 GPU 时间少约三成 | wall 6.0 ms / GPU 3.6 ms / p50 2.7 ms |

画质：静态画面与 `a` 逐像素相同；运动段 PSNR 45.6–48.0 dB，差别集中在 HUD 与遮挡边缘。
`preset="b"`（UI 重组）会直接报错：它要同时拿到无 UI 画面、UI 颜色与遮罩，本插件只有一张已合成的
画面，运行库只会按 `a` 跑。

两点行为要注意：

- 改写**落在引擎包文件上**。跑过 `c` 之后包就停在 `C`，下次不给 `preset` 也会是 `c`；想回到 `a`
  就传 `preset="a"` 跑一次。mpv 侧的 K7sfunc 脚本默认 `preset=a`，每次播放都会顺手顶回 `a`。
- 预设是**进程级**的。同一进程里第二个过滤器要求不同预设会报错，换预设要另起进程（mpv 里换预设
  等于重建滤镜，没问题；一次 vspipe 里挂两条只能用同一个值）。

## 6. 引擎与硬件

| 插件 | 引擎 | 倍率 | 依赖 |
|---|---|---|---|
| `core.vdlssg.FrameGen` | NVIDIA DLSS-G | 2–6× | RTX 20/30 系（sm86 解锁）；6× 需要本仓库 `dlss45` 包 |
| `core.vxessfg.FrameGen` | Intel XeSS-FG（model 15） | 2–4× | 无硬件锁定，走同一套代理 |
| `core.vvfg.FrameGen` | NVIDIA Video Frame Generation | 2–6× | NVIDIA Video Effects 运行库（不随本仓库分发） |
| `core.vfsr4.FrameGen` | AMD FSR 4 | 2× | Vulkan，需 vkd3d-proton 运行库（不随本仓库分发） |

`dlss45` 引擎包与 stock `dlssg_sm86` 的唯一差别是 `MaxGeneratedFrames=5`：stock 最高 4×，6× 只能用它。
一个引擎一个 DLL，`build.bat` 一次编四个。

## 7. 排错

| 现象 | 原因 / 处理 |
|---|---|
| `proxy version.dll not found in the plugin directory` | `proxy_dir` 指错了，或引擎包没和 DLL 同级 |
| `LoadLibraryW(version.dll) failed` | 同目录缺 `dlssg_sm86\runtime\nvngx_dlssg.dll` 等运行库，重下完整引擎包 |
| `CreateDlssgFeature` 返回 `0xbad00002` | 多卡机器选错卡；删掉 `gpu=` 让插件跟随代理，或换一个索引 |
| `another NGX engine plugin ... already initialized NGX` | `vdlssg` 与 `vxessfg` 不能同进程，按引擎分进程 |
| `only one frame size, pixel layout and engine per process` | 一个进程里挂了两种尺寸/格式的源 |
| `preset must be "a" ... or "c"` / `preset b ... is not offered` | 见第 5 节 |
| 画面变糊但帧率没涨 | 可能落到 `_DLSSGFallback`（运动估计失败），开 `log=1` 看 stderr |
| 越跑越占空间 | `dlss45\dlssg_sm86\logs\dlssg_sm86.log` 每个进程追加一条，可随时删 |

底层机制、四引擎对照实测见 `docs\` 下的 `vdlssg-presets.md`、`vdlssg-multiframe.md`、`engines-benchmark.md`。
