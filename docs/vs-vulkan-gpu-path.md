# VS Vulkan GPU 驻留路径（R80+）与 vpy 用法

> 调研日期 2026-11-25，实测环境 `.venv82`（uv venv，Python 3.12.10，vapoursynth==82 / Core R82 / API R4.3），
> GPU：RTX 3080 10GB，驱动 Vulkan 1.4.351。探针脚本见 `work/vk_probe.py`、`work/vk_chain_bench.py`（均不进 git）。

## 一句话结论

R80 起 VapourSynth 自带 **GPU 驻留（vnode:gpu）**：一个 core 只建一个 VkDevice，帧直接活在显存里，
`std.GPUUpload()` 上传一次、中间任意多级 GPU 滤镜、`std.GPUDownload()` 下载一次，**中间帧不过 PCIe**。
这就是"一个上下文、只有一次拷贝"的官方机制。但**只有声明 `vnode:gpu` 的滤镜能待在链上**；
本仓库的 vdlssg / vsrife / vsaji 都是 CPU 帧插件，塞进去等于强制 download+upload 各一次，比现在更慢。

## 模型要点（用户视角）

| 事项 | 行为 |
| --- | --- |
| 设备 | 每 core 一个 VkDevice，首次用到 GPU 滤镜时才创建；默认挑独显/显存最大者 |
| 选择设备 | `core.vulkan_devices` 枚举；**创建任何 GPU 滤镜之前** `core.set_vulkan_device(index)` |
| 设备状态 | `core.vulkan_device_info` → 名称 / budget / allocated / limit / uuid / export 句柄类型 |
| 帧 | 每平面一个 vkBuffer，线性排列，stride 与 CPU 帧相同；可引用计数、可透传 |
| 读像素 | GPU 帧在 Python 侧读会抛错：`The frame is GPU resident and its pixel data cannot be accessed directly, pass the clip through std.GPUDownload first`；C API 返回 NULL，绝不静默下载 |
| 驻留属性 | 节点/帧都有 `gpu_resident` |
| 边界 | `clip.std.GPUUpload()` / `clip.std.GPUDownload()`；已对应驻留时是 no-op |
| 隐式转换 | 给 GPU-only 滤镜喂 CPU clip 会自动插 upload 并打日志；`set_output()` 存 GPU 节点，`output()`/vspipe 会插一次下载 |
| 不打断链的滤镜 | `vnode:all`：Trim/Splice/Reverse/Loop/Interleave/SelectEvery/DuplicateFrames/DeleteFrames/FreezeFrames/AssumeFPS/SetFrameProp(s)/SetFieldBased/CopyFrameProps/RemoveFrameProps/ClipToProp/PropToClip/SetVideoCache/ShufflePlanes/SplitPlanes/FrameEval/ModifyFrame |
| 硬限制 | GPU 滤镜要求**恒定格式 + 恒定尺寸**（变量 clip 直接拒绝，让你先 GPUDownload）；多输入必须同为一种驻留，混用是错误 |
| VRAM | 默认上限 ≈ 驱动报告可用量的 2/3；实测本机 `max_vram_cache_size` = 6190MB，budget 9736MB；`VS_VULKAN_MAX_VRAM_MB` 可在启动前覆盖；在途提交保留 ≤ 上限的 1/4 |

## vpy 里怎么用

```python
import vapoursynth as vs
core = vs.core

# 1) 选设备（必须在创建第一个 GPU 滤镜之前）
print(core.vulkan_devices)      # [{'index':0,'name':'NVIDIA GeForce RTX 3080','api_version':(1,4,351),
                                #   'type':'discrete','device_memory':10541334528,'usable':True,'reason':'', 'uuid':'…'}]
core.set_vulkan_device(0)

# 2) 上传一次
clip = core.ffms2.Source(r"...")            # 普通 CPU 帧
gpu  = clip.std.GPUUpload()                 # 到此为止只发生一次 H2D

# 3) 中间全在显存里跑（这些滤镜必须有 GPU 实现；R82 里 std.BoxBlur、std.PlaneStats 已有）
from functools import reduce
gpu = reduce(lambda c, _: core.std.BoxBlur(c, hradius=8, hpasses=2, vradius=8, vpasses=2), range(4), gpu)

# 4) 下载一次
out = gpu.std.GPUDownload()
out.set_output()
```

实测输出（本机，640×360 探针）：

```
device count: 1
  [0] {'index': 0, 'name': 'NVIDIA GeForce RTX 3080', 'api_version': (1, 4, 351), 'type': 'discrete',
       'device_memory': 10541334528, 'usable': True, 'reason': '', 'uuid': '51eb4c6fe8775ceb46696fa3d06b3da9'}
current device info: {'budget': 9736028160, 'allocated': 0, 'limit': 6490685440,
                      'export_handle_type': 2, 'semaphore_export_handle_type': 2, 'unified_memory': False}
std funcs: ['GPUDownload', 'GPUUpload']
source gpu_resident: False   → uploaded: True   → downloaded: False
read gpu plane failed: Error The frame is GPU resident and its pixel data cannot be accessed directly…
```

## 实测：一次往返 vs 多级驻留（1080p YUV420P10，24 帧，单线程取帧）

| 链路 | ms/帧 | fps |
| --- | --- | --- |
| CPU BoxBlur ×1 | 14.61 | 68.4 |
| CPU BoxBlur ×3 | 40.23 | 24.9 |
| Upload→Download（无滤镜，纯往返） | 2.21 | 451.7 |
| Upload→Blur×1→Download | 2.81 | 356.1 |
| Upload→Blur×3→Download | 3.06 | 326.3 |

读法：**固定成本是那一次往返（≈2.2ms），每多一级驻留滤镜只加 0.25–0.6ms**。
官方数字是同量级（1080p 往返 ≈0.75ms，4K16 ≈5.4ms，提交地板 ≈0.2ms）——本机 2.2ms 里包含
单线程取帧与 10bit 数据量，不必按绝对值对齐，看比值即可：往返 = 滤镜的 4–8 倍。

## 与本仓库现状的对照

`docs/vdlssg-opt12.md`（1080p YUV420P10，m6）：稳态逐帧 `up 0.00 / rb 0.55–0.89 / gpu 1.77 / cp 1.03 / wall 3.10–3.43 ms`，
回读 6.22MB(10bit) / 3.11MB(8bit)。也就是说 vdlssg **在插件内部**已经做到"上传一次 + 每帧回读一次"，
剩下的瓶颈正是**每个生成帧都必须回读成 VS CPU 帧**（rb + cp ≈ 1.6–1.9ms）。

R80 的驻留模型恰好能把这一步省掉——前提是**输出侧不再落到 CPU 帧**。可行路线按代价排序：

### 路线 A：把引擎插件改成 GPU 输出（唯一真正"零回读"的做法）

- 注册 `vnode:gpu` 参数/返回 + `ffGPUOutput`，用 `getVulkanAPI()` 拿 `VSVULKANAPI`（API 4.3，
  需 `VS_USE_API_43` / `VS_USE_LATEST_API` + `#include VSVulkan4.h`），**不得自建 instance/device**。
- 中间计算仍走自己的 D3D12/CUDA：`exportGPUPlane` 给出平面不透明句柄 + (offset,size)，外部 API 导入同一块 VRAM 零拷贝读写。
  本机实测 `export_handle_type = 2`（OPAQUE_WIN32）、`semaphore_export_handle_type = 2`，即
  **导出 NT 句柄 + timeline semaphore 都可用**，可以完全设备侧同步（DLSS-G 走 D3D12 共享资源，CUDA 侧走 `cudaImportExternalMemory`）。
- 输出用 `newGPUVideoFrame` 新分配、`newVideoFrame2` 共享未处理平面（4K 下共享比拷贝划算）；
  只对真正写过的平面 `gpuExecWritesPlane`；每个平面 publish `(semaphore, value)` 的 producer pair。
- 实施抓手：`VSGPUExecPool`（acquire/reads/writes/submit，已替你处理 producer 等待、队列锁、在途深度、显存压力回收）；
  更省事的声明式封装 `gpufilter.h` 属内部头（C++20），要复制到插件侧编译，不保证 ABI。
- 本项目已有 D3D11↔D3D12 共享纹理与 FSR4 Vulkan 桥（`vs-dlssg/src/fsr4_vk_device.cpp`、`fsr4_session.h:4`）的现成经验，
  `D3DKMTShareObjects` 换 `exportGPUPlane` 的心智模型是连续的。

### 路线 B：不改插件，只在 vpy 侧把 GPU 滤镜接在引擎前后

- 能省的是"引擎前面的 CPU 侧滤镜链"（deband/blur 等）的多次往返 → 一次往返，但引擎本身仍要
  download→CPU→upload 两次穿越（≈4ms/帧 @1080p，比现在更亏）。
- 现实价值：几乎为零，除非引擎已在 GPU 输出侧（路线 A 完成）。

### 路线 C：mpv 播放路径上不成立（重要限制）

- mpv 的 `vf=vapoursynth` 只吃 CPU 帧（`mpv-src/video/filter/vf_vapoursynth.c` 里没有任何 hwframe / IMGFMT_VULKAN 路径），
  解码后的帧先落到系统内存再进 VS，出 VS 再回 mpv。**播放时 vpy 驻留链只省 VS 图内部的往返**，
  边界永远各有一次上/下传。
- 想要播放端也零拷贝，得走 mpv 原生 hw-frame 滤镜（即 `vf_animejanai.c` 那种：输入 IMGFMT_VULKAN/CUDA/D3D11，
  输出自有 AVHWFramesContext），而不是 vpy 插件。

## 环境与版本坑（实测）

- `.venv82` 用 uv 建：`uv venv --python 3.12 .venv82` + `uv pip install --python .venv82\Scripts\python.exe vapoursynth==82 numpy`；
  PyPI 有 `cp312-abi3-win_amd64` 轮子（82 于 2026-11-01 上传），不用自己编译。
- **R80 起 drop API 3**：`akarin.dll` 在 R82 上直接报
  `Plugin … uses API 3, which is no longer supported.` → RIFE_NV / RIFE_AJI 的现有路径在 R80+ 全部不可用。
- 好消息：本仓库引擎 DLL（API 4.2）在 R82 上**能正常加载**（实测 `vdlssg.dll` 载入成功、注册命名空间 `vdlssg`），
  所以"R82 + 自家引擎、不要 akarin"是可行组合；.venv 里钉 74 只是为了保住 akarin。
- 第三方插件（vs-placebo / vs-mlrt / ffms2 等）stock 版本都还没有 `vnode:gpu`，接上去就是多次往返。
- GPU 帧不进 `copyFrame` 的写时复制（显存无法独立复制），改属性可以、改像素要走新帧。

## 建议的下一步（若要把回读彻底干掉）

1. 先给 vdlssg 加一个"输出 GPU 帧"的开关（`ffGPUOutput` + `exportGPUPlane` 给 D3D12 侧导入），
   保持原有 CPU 输出路径不变，用 `docs/vdlssg-opt12.md` 的逐帧账（rb/cp）作为回归基线。
2. 在 `.venv82` 里跑 `GPUUpload → vdlssg(gpu) → GPUDownload` 与纯 CPU 路径对比，
   预期省掉 1.6–1.9ms/帧（1080p10）；4K 上省得更多（回读 ≈5ms 量级）。
3. 显存按 6190MB 默认上限留余量，m6 1080p10 一帧 ≈6MB，链上在途 ≤ 上限 1/4，一般无压力；4K 需实测。

## mpv 边界能不能吃 GPU 帧（上游现状，2026-11-25）

### vf_vapoursynth 本身：不支持，且没有人在做

- `mpv-src/video/filter/vf_vapoursynth.c` 里没有任何 hwdec / hwframe / `IMGFMT_VULKAN` 分支：
  `mp_to_vs()`（:140）只接受 `mp_get_regular_imgfmt()` 认得的常规 planar 格式，初始化时（:779-785）
  也只把这些常规格式加进 `mp_autoconvert`，因此 hw 帧在进滤镜前就被 hwdownload 成 CPU 帧。
- 上游（mpv-player/mpv）检索 "vapoursynth" 相关共 425 条，唯一相关的新 PR 是
  [#18854 "vf_vapoursynth: allocate output frames with vo_get_image()"](https://github.com/mpv-player/mpv/pull/18854)
  （open，2026-11-18，作者 petralithic）：把输出帧改成 `vo_get_image(p->dr_vo, ...)` 分配 DR 缓冲再
  `mp_image_copy()`，省掉的是 **VO 那次再拷贝/再上传**，不是 hw 帧。作者实测 M1 Pro + gpu-next，
  4K HDR 120fps 从 ~86 提到 91-97fps，4K 8bit 无变化，只在 macOS 测过。
  这个收益依赖 DR 缓冲本身是 CPU 可写、VO 零拷贝的（macOS IOSurface 统一内存）；
  Windows/D3D11 上 DR 缓冲仍要上传，收益另说。
- "vapoursynth hwdec" 检索 0 条、"vapoursynth vulkan" 153 条全部无关；本仓库 fork（o5-null/mpv）
  在 mpv-player/mpv 下没有任何 PR。
- 参考实现：the-database/mpv（mpv-AnimeJaNai 的 fork，2026-11-24 更新）对 `vf_vapoursynth.c` 只加了
  两处，都与 GPU 无关、但对 ML 链有实用价值，可直接借鉴到我们 fork：
  1. `skip-seek-pre-target`（默认 1）：hr-seek 时在进 VS 之前丢掉 pts < target-0.005 的帧，
     避免为注定被丢弃的帧跑一遍昂贵的模型/重载引擎；
  2. `soft_reset_vs()`：reset 时不再 `destroy_vs`，而是清空在途请求与队列、保留 VS 节点
     （TensorRT / NGX / CUDA 上下文不重载），帧号单调前进到 `frames_sent` 以免 VS 缓存脏命中。

### 要做"边界零拷贝"需要什么

mpv 侧其实是有的：`mpv-src/video/out/hwdec/hwdec_vulkan.c:374` 用
`pl_vulkan_wrap(p_owner->gpu, pl_vulkan_wrap_params(...))` 把外部 VkImage（AVVkFrame）包成纹理给
gpu-next 直接采样。所以"VS 出 GPU 帧 → mpv 直接渲染"在原理上成立，缺的是入口和胶水：

1. 一个会 `getVulkanAPI()` 的滤镜（vf_vapoursynth 得先改：脚本侧 `vnode:gpu` 输出时不做
   `GPUDownload`，而是 `exportGPUPlane` 取句柄）；
2. 把导出的共享内存导入 mpv 的 VkDevice —— `pl_vulkan_wrap` 只包**同一个 device** 上的 image，
   跨 device 要自己 `vkAllocateMemory(VkImportMemoryWin32HandleInfoKHR)` + `vkCreateImage`
   （VS 的 `export_handle_type=2` 即 OPAQUE_WIN32，实测可用）；
3. 逐帧 fence 同步：`exportGPUSemaphore` 的 timeline semaphore 要么在 mpv 侧 vkWaitSemaphores，
   要么退回 `waitGPUFrame` 主机等待（每帧一次 CPU 阻塞，吃掉部分收益）；
4. 设备唯一性：mpv 的 VkDevice 由 libplacebo 自建，和 VS core 的 VkDevice 不是同一个，
   UUID 必须对应同一张卡，否则只能走"导入外部内存"的慢路径。

而反方向永远不通：`VSVulkan4.h:384` / `:417` 明确说 VS 不提供"把外部内存包成 GPU 帧"的入口，
只能导出、不能导入。**所以解码帧进不了 VS 图，驻留链最多只能从 VS 图内部开始**，
播放器边界的最少代价是"一次上传（解码帧→VS）"；只是现在连这一次上传都省不掉，
因为 vf_vapoursynth 逼着 VS 用 CPU 帧。

### 三条可选路线（按投入排序）

| 路线 | 做法 | 代价 | 收益 |
|---|---|---|---|
| 1. 跟 #18854 | 输出帧走 `vo_get_image()` DR 缓冲 | 小（十几行） | 只省 VO 的再拷贝，Windows 上很可能为 0 |
| 2. 改 vf_vapoursynth 支持 GPU 帧 | 脚本 `vnode:gpu` + `exportGPUPlane` + 外部内存导入 + fence | 大（滤镜侧 + ra/pl 侧） | 真正边界零拷贝，但仅当引擎已能 GPU 输出 |
| 3. mpv 原生滤镜跑引擎 | 像 `vf_animejanai.c` 那样自己吃 hw 帧、不走 VS | 大（等于把引擎移植成 vf） | 边界零拷贝且无 VS 中介，但丢掉整个 vpy 生态 |

## vf_playkit 能不能"暴露给 vpy"（评估，2026-11-25）

问题：把现有 mpv 侧 `vf_playkit`（D3D11 零拷贝 DLSS-G）搬到 VapourSynth 里用。

**能，但移植出来的是现有 `vdlssg` 的重复实现，且吞吐更差；playkit 的零拷贝恰恰来自它活在 mpv 里。**

### playkit 里与 mpv 绑定的部分（不可复用）

`mpv-src/video/filter/vf_playkit.c`：

| 依赖 | 用处 | VS 侧有没有 |
|---|---|---|
| `hwdec_devs` → `AVHWFramesContext`/`AVD3D11VADeviceContext`（:347-367） | 直接拿解码器的 D3D11 device | 没有 |
| 输入 `IMGFMT_D3D11` + `hw_subfmt=NV12/P010`（:371-382） | 解码帧零拷贝进滤镜 | 没有（VS 只给 CPU 帧） |
| `mp_update_av_hw_frames_pool`（:461-467） | 输出帧池，VO 直接采样 | 没有 |
| D3D11 VideoProcessor（:398-424、:951-1000） | 把引擎的 RGBA8 输出转回 NV12/P010 | 要自己实现（vdlssg 已有自己的 CSC） |
| `mp_filter` 调度 + `pk_wait`（:595-609、:741） | 每对帧一次阻塞往返 | vpy 有自己的取帧/队列模型 |

### 唯一可复用的是 bridge ABI

`vs-dlssg/src/bridge.cpp:72 / :120 / :156` 导出 `vdlssg_bridge_create / _process / _destroy`，
参数是 `(handle, prev_nt_handle, cur_nt_handle, out_nt_handles[], multi, stats*, err)` —— 纯 C、与 mpv 无关。
VS 插件完全可以 `LoadLibrary(vdlssg.dll)` 调它，但插件侧仍要自己做：

1. CPU VSFrame → D3D11 共享纹理上传（每输入帧一次）；
2. `vdlssg_bridge_process`；
3. 输出 RGBA8 纹理 → CPU VSFrame 下载（每生成帧一次）。

= 现有 `vdlssg` 已经做的事（`d3d12_ctx.cpp` 的 UPLOAD/READBACK 路径），只是把 D3D12 前端换成 D3D11。
没有净收益，除非要复用 playkit 的 VP 色彩转换结果。

### 反方向也不通（vpy GPU 帧 → playkit）

`std.GPUDownload` 之外，VS 导出的平面是 **vkBuffer（线性格子）**，`exportGPUPlane` 给的是 buffer 的 NT 句柄；
D3D11 的 `OpenSharedResource1` 只能开纹理，开不了 buffer。要走通得：改 bridge 接受 buffer 句柄 →
D3D12 `OpenSharedResource` 成 buffer → `CopyBufferRegion`/buffer→texture 拷贝，多一次显存内拷贝且改桥。

### 已有的对照数据（说明该往哪投力气）

`docs/k7-vpy-debug-probe-2026-11-25.md`：1080p 2×，vpy 宿主（`FRAMEGEN_VPY`）149.7 fps，
原生 `vf_playkit` 51.2 fps；瓶颈是 playkit 每对帧一次阻塞往返（本文件 :759 单次 `process`、
:741 `pk_wait`，单组 in_tex + 单 query），不是零拷贝失效。
→ 想让 playkit 追上 vpy 吞吐，是给 vf_playkit 加双缓冲/双 group 流水（改动局限在 vf_playkit.c），
而不是把 playkit 搬进 vpy。
