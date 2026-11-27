# AnimeJaNai (libaji) 的 RIFE 接进 k7：现状

结论先行：**上游没有 VapourSynth 插件**，也拿不到可复现的二进制（release 由作者手工构建上传）。
所以自己写了 VS 插件并**从引擎源码构建 libaji**，代码独立成仓库：

- 插件 + 构建脚本：`E:\code\dlssg-vfi\vsaji`（github.com/o5-null/vsaji，MIT）
- k7 接入：`E:\code\dlssg-vfi\k7sfunc` 的 `RIFE_AJI`（`src/k7sfunc/mod_memc.py:1095`）
- 打包产物：`vsaji/dist/aji/` → 播放器 `<包根>\vs-plugins\aji\`，其中的 `vsaji.dll` 单独进
  `Lib\site-packages\vapoursynth\plugins\`（见 DECISIONS 第 17 条）

本文只留结论与对接约定；构建细节看 vsaji 的 README。

## 1. 上游事实（研究记录）

- `the-database/mpv` 是带 `vf_animejanai` 的 mpv fork，`license: NOASSERTION`，release **只有
  Linux**；Windows 构建在 `the-database/mpv-winbuild`（整只 mpv，不是滤镜）。
- 那个"滤镜"是 mpv 的 `video/filter/vf_animejanai.c`，由 `meson.build:1448-1450` 在
  `cuda-hwaccel` 可用时**静态编进 mpv**，没有任何运行期加载机制；mpv 的 `mp_filter`/`mp_image`
  是内部 ABI，不能做成跨版本 DLL。所以 `--vf=animejanai` 只存在于那只预编译 mpv 里。
- 29 个 `the-database` 仓库里没有任何 VapourSynth 插件仓库；能复用的只有它的
  `video/filter/aji.h`（C ABI，`AJI_API_VERSION 9`）和 `vf_animejanai.c`（LGPL，作参考）。
- 引擎 `the-database/animejanai-inference` 的 Windows 二进制是手工构建上传的，
  `docs/BUILD-WINDOWS.md` 有完整的源码构建流程 —— 我们走这条路，版本按 commit 钉死。

## 2. 我们这套的形态

```
vsaji.dll         VS 插件（本仓库写的胶水，MIT）
aji.dll              libaji dispatcher（上游 release v0.33.0）
aji_trt.dll          后端：CUDA 原生 RIFE 内核 + TensorRT 引擎路径（上游）
aji_dml.dll / aji_vk.dll  DirectML / Vulkan 后端（上游，随包但本宿主用不了）
aji_trtrtx.dll       TensorRT-RTX 后端（同样含原生内核；上游，默认随包）
cudart64_13.dll      CUDA 运行时
aji-runtime.json     {"format":1,"trt":"trt","trtrtx":"trtrtx"}
trt\                 nvinfer_11.dll 等（NVIDIA 公开包，含 cudart 副本）
trtrtx\              TensorRT-RTX 运行库（约 230 MB）
conf\                k7 按模型生成的 rife_only_<code>.conf
rife\                rife_v4.*.onnx
```

要点：

- 插件只声明/使用 `include/aji.h` 的 C ABI，**不复制、不改写引擎代码**；后端 DLL 直接取上游
  release（sha256 钉死），头文件从同一 commit 导出（只把 `AJI_EXPORT` 的 dllexport 去掉）。
  目录搜索、`aji-runtime.json` 预载、后端加载全部由 libaji 自己做。
- RIFE-only conf 下 `aji_configure` 返回 0；引擎默认走 `ajinative`（ONNX 直读 + CUDA 内核），
  **不建 TensorRT 引擎、不调 trtexec**，所以启动快。
- 运行时包校验要求 `trt\` 内有 cudart，只放在 `aji.dll` 旁边会拒绝启动。
- 扁平布局是为了 k7：`Version()` 同时报插件目录与它探测到的 `aji` 目录（同级 `aji.dll` 或
  下级 `aji\`），k7 优先按这个自报路径找 conf/模型，再退回 `vs-plugins\aji`。
- 插件在 `Version()["backends"]`/`["missing"]` 里报告**后端 DLL 的组成**（只查文件，不加载），
  但不再自己选后端：`backend=""`（auto）按 `trt+native=1` → `TensorRT-RTX` → `trt+native=0`
  的梯队决定，必要时写临时 conf；DirectML/Vulkan 永远不会入选——DirectML 要 `ID3D11Device` +
  共享 D3D11 纹理（`aji_dml.cpp:2028`），Vulkan 要 `aji_create_vk` 的 `aji_vk_image` 帧
  （`aji_vk_frames.cpp:24`），本插件只有主机帧 + CUDA context。
- TensorRT-RTX 后端默认随包（`aji_trtrtx.dll` + `trtrtx\` 运行库，约 230 MB）：conf 写
  `TensorRT-RTX` 或用 `backend="TensorRT-RTX"` 时才走这条链，加载失败 dispatcher 会回落 `aji_trt`。
  `backend=` 可强制后端、强制 CUDA 原生（`native`）或限定 `conf`。

## 3. 插件接口

```python
core.vsaji.Rife(clip, conf, rife_model_dir, factor=2, device=0,
                   matrix=-1, range=-1, scene_prop=1, lib="", fps=25.0, backend="")
```

| 参数 | 含义 |
| --- | --- |
| `conf` / `rife_model_dir` | RIFE-only conf 与 `rife_v4.*.onnx` 所在目录 |
| `factor` | 2..16；时间轴 `src = n / factor`、`t = (n % factor) / factor` |
| `scene_prop` | 1 = 读 `_SceneChangeNext` 传 scene 0/1；0 = 交 libaji 内建检测（-1） |
| `lib` | `aji.dll` 路径；空则用插件同目录的 `aji.dll`（不再做目录搜索，那是 libaji 的事） |
| `fps` | 只喂 conf 的 `chain_1_max_fps` 判定（必须大于源 fps） |
| `backend` | `""`/`auto` = 梯队 `trt+native=1` → `TensorRT-RTX` → `trt+native=0`；`conf` = 严格按 conf；`native`/`cuda` = `TensorRT` + 强制原生；也可写后端名。需要换后端时写临时 conf |

- 帧：`phase == 0` 或尾部直接 `copyFrame(a)`；`AJI_SCENE` 时同样输出 `a`。
- 格式：4:2:0 的 8bit（NV12）或 10bit（P010）；10bit 在收发时做 LSB↔MSB 位移。
- 输出帧数：`(源帧数-1) * factor + 1`（30 帧 2 倍 = 59 帧）。
- 实例内部一把锁串行：VS 并行取帧不会撞同一个 CUcontext。

同一个插件还注册了超分滤镜（k7 侧暂无封装）：

```python
core.vsaji.Upscale(clip, conf, model_dir="", device=0, matrix=-1, range=-1,
                      lib="", fps=25.0, backend="")
```

conf 换成带超分链的模板（`chain_1_model_1_name` = ONNX 文件名，不含 `.onnx`），输出尺寸取链
自报的 `w×h`。SPAN/SPANF3/Compact 有 CUDA 原生内核（RTX 3080 实测 720p Balanced 26.3 ms/帧、
Performance 10.8 ms/帧；1080p Balanced 57.2 ms/帧），整帧模型仍需能建 engine 的 TensorRT。
模型来源与打包方式见 vsaji 的 `README.md`「超分（Upscale）」。

## 4. conf 模板（k7 自动生成）

```ini
[global]
config_version=2
backend=TensorRT
logging=no

[slot_1]
profile_name=k7 RIFE
chain_1_max_resolution=4096x2304
chain_1_max_fps=241
chain_1_rife=yes
chain_1_rife_factor_numerator=<factor>
chain_1_rife_factor_denominator=1
chain_1_rife_model=<code>
```

模型编码：`4.14` → `414`、`4.25_lite` → `4251`（4 位代码末位 1 = lite）、
`4.26` → `426`。**`component-rife 3.12.5` 里没有 4.25_lite**（lite 只有 4.12/4.13/4.14/
4.15/4.16/4.17/4.22），所以 `4251` 这个 k7 字面量在该模型包上找不到文件。

## 5. k7 侧

- `src/k7sfunc/mod_memc.py:1095-1217` `RIFE_AJI(input, model, ensemble, fps_in, fps_num,
  fps_den, sc_mode, gpu)`：`_aji_runtime_dir()` 依次看 `AJI_DIR` → `Version()["aji"]` → 插件同级/
  上级 `aji\` → `<包根>\vs-plugins\aji` → 生成/复用 conf → 调 `core.vsaji.Rife` →
  `AssumeFPS(fps_in * factor)`；非整数倍率直接报错。
- `PLAYKIT_MEMC` 也可以直接用：`memc_engine=aji`，顺序在 `rife_nv` 之后（`mod_playkit.py`）。
- `mpv/portable_config/vs/MEMC_RIFE_AJI.vpy`：`_here` 递归找 `vs-plugins`、`AJI_*` 环境变量。
- `FORK.md:31` 一行改动说明；`doc/Windows.md:582` 起是用户文档。

## 6. 限制

- 每帧一次主机↔设备拷贝：VS 帧在系统内存，拿不到解码器的 GPU 帧句柄；mpv 内置滤镜的零拷贝
  路径这里做不到。1080p 一次往返约 9 MB，相对推理可忽略。
- 只支持 4:2:0 8/10bit；其他格式先转。
- `Rife` 只吃 RIFE-only conf；带超分链的 conf 请改用同插件的 `Upscale`（k7 暂无封装）。
- `aji_trt.dll` 仍链接 nvinfer（CUDA 原生内核与 TensorRT 后端在**同一个** DLL 里），TensorRT
  运行时必须在 `trt\` 里（`aji-runtime.json` 指路，别手写 PATH）；缺 `nvinfer_11.dll` 时整个
  后端加载失败（error 126），即使 conf 只走原生内核。`Version()["missing"]` 会提前指出。
- 授权：插件 MIT、引擎保留所有权利。仅本机自用时无顾虑；对外分发不要把 libaji 打进包里，
  让使用者自己按 vsaji 的脚本构建。
