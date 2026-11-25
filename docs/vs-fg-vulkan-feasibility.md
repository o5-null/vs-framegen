# 当前 vs-fg 能否用上 VS Vulkan GPU 驻留

日期：2026-11-25
关联：`docs/vs-vulkan-gpu-path.md`（R80+ 驻留模型与实测）、`docs/vdlssg-opt12.md`（逐帧耗时账）、`docs/vs-api4-adaptation.md`

## 结论

**现状用不上，一行都不沾。** 四个引擎（vdlssg / vxessfg / vvfg / vfsr4）注册的都是普通 CPU 帧滤镜，
接口是主机指针；把现有 DLL 插进 `std.GPUUpload() → … → std.GPUDownload()` 的驻留链，不会省拷贝，
反而每个滤镜多两次穿越（下载 + 上传），比现在更慢。

改造可行，但四个引擎的代价完全不同，因为瓶颈不在 VS，而在引擎各自 API 能吃什么东西：

| 引擎 | 引擎侧 API | 能否直接读写 VS 的 vkBuffer | 最小改造 | 拷贝性质 |
| --- | --- | --- | --- | --- |
| vvfg | CUDA driver API（`nvcuda.dll` 动态取符号） | **能**（CUDA 可导入 Vulkan 导出的不透明内存） | 输入 `exportGPUPlane`→`cuImportExternalMemory`；pack/unpack 两个 CPU 循环换成 CUDA kernel，写回 VS 平面 | 真零拷贝，PCIe 归零 |
| vfsr4 | 自带 VkInstance/VkDevice + vk 导入 bridge 的 D3DKMT 共享面 | **能，但要跨设备**（Vulkan 外部内存跨 device 合法；生产者在 core 的 device 上） | 在自有 device 上导入 VS 平面，`vkCmdCopyImageToBuffer` 直写，用导出的 timeline semaphore 回签 | 一次显存内拷贝 |
| vdlssg / vxessfg | D3D12 + NVNGX | **不能**（见下） | D3D12 纹理 → 共享 NT 句柄 → CUDA/Vulkan 导入 → 写 VS 平面 | 每帧 1–2 次显存内拷贝 |
| （任意） | — | — | — | — |

关键硬约束：**VS 的 GPU 平面是核心池里分配的 `vkBuffer`（线性格子，stride 与 CPU 帧相同），
只能由"能导入外部内存"的 API 写入，不能作为 D3D12 资源打开。** D3D12 方向只有 D3D12→Vulkan
（导出共享句柄后被 Vulkan/CUDA 导入），反方向没有等价物；所以 NGX 两个引擎必须留一次显存内搬运。
`VSVulkan4.h` 只提供 `exportGPUPlane`（VS→外部），**没有**"把外部内存包成 VS GPU 帧"的入口，
这一点决定了改造方向只能是"引擎写进核心分配的平面"。

## 现状证据（代码级）

统一入口 `vs-dlssg/src/frame_gen.cpp`：

- `frame_gen.cpp:742-746` 注册即 CPU 契约：`"clip:vnode;multi:int:opt;…"` → `"clip:vnode;"`。没有 `vnode:gpu`。
- `frame_gen.cpp:356` 输出 `vsapi->newVideoFrame(...)`；`frame_gen.cpp:366-373` 用
  `getReadPtr/getWritePtr/getStride` 组装 `PlaneView`/`PlaneSink`（主机指针）；`frame_gen.cpp:401-403`
  把 `sink` 交给 `session->generate(...)`。整条路径假定像素在 CPU 内存。
- `frame_gen.cpp:734-741` 是唯一和 API 版本相关的地方：`apiVersion = min(coreApi, VAPOURSYNTH_API_VERSION)`，
  低于 4.3 的内核上 GPU 入口必须自己门控。

各引擎的传输实现：

- **dlss/xess**（`d3d12_ctx.cpp`）：`319-324` 建 `D3D12_HEAP_TYPE_UPLOAD` 常量缓冲，`1116-1128`
  `CopyTextureRegion` 上纹理；输出侧 `274-278` 建 `D3D12_HEAP_TYPE_READBACK`，`1169-1176` 把结果拷进
  回读堆，`1246-1260` `Map` 后由 CPU 取走。`460-471` 说明回读路径已经把 RGBA16F 颜色纹理转成
  planar 4:2:0 8/10bit 紧凑排布——这段转换逻辑正是驻留版要保留、但落点改成 vkBuffer 的部分。
- **vvfg**（`vfg_session.cpp`）：`100-140` 全部 CUDA driver API 动态加载（无 CUDA SDK 依赖，加
  `cuImportExternalMemory` / `cuExternalMemoryGetMappedBuffer` / `cuWaitExternalSemaphoresAsync`
  只是多几个 `get()`）；`684-726` 输入是**CPU 逐行打包** YUV→RGB 再 `cuMemcpyHtoDAsync`；
  `781-797` VFG 的 `SrcImage`/`DstImage` 就是裸 `CUdeviceptr` + 任意 pitch + 交错 RGBA/RGB10A2
  （`kVfgCuda` 后端）；`807-857` 输出 `cuMemcpyDtoHAsync` 后再 CPU 解包 + YUV 下采样。
- **fsr4**（`fsr4_vk_device.cpp:49,122`）：自建 `VkInstance` 和 `VkDevice`，与 core 的 device 无关。

构建：`vs-dlssg/src/build.bat:48` 已经带 `/DVS_USE_LATEST_API`，`:9` 的 `VS_INCLUDE` 目前指向 R74 头
（API 4.2）。把 `VS_INCLUDE` 指到 `.venv82\Lib\site-packages\vapoursynth\include` 就能编到 API 4.3，
头里 `VSVulkan4.h` 提供 `newGPUVideoFrame`(:594)、`exportGPUPlane`(:968)、`exportGPUSemaphore`(:981)、
`waitGPUFrame`(:991)、`setGPUPlaneProducer`(:613)、`VSVulkanExportedMemory`(:429)、
`VSVulkanCoreInfo::exportHandleType`(:518)。

生态：`vnode:gpu` 目前只出现在 core 自身（`GPUUpload`/`GPUDownload`）和 `sdk/` 示例
（`gpu_invert_example.c`、`gpu_invert_driver_example.cpp`、`gpu_cuda_invert_example.cu`）。没有第三方
ML 插件（vs-mlrt、vsrife 等）落地过，官方 CUDA 示例自己注明"还没在 NVIDIA 硬件上跑过"。也就是说
**没有现成模板可抄，只有参考代码**。

## 收益边界（必须先讲清楚）

驻留的输出帧最终给谁，决定省多少：

- 消费端在 GPU（vspipe→NVENC、mpv 原生 hw 滤镜、后续驻留滤镜）：省掉的是"每个生成帧回读 + CPU 拷贝"，
  即 `docs/vdlssg-opt12.md` 里 1080p10 的 `rb 0.55–0.89ms + cp 1.03ms`，换成约 0.05ms 的显存内写。
- 消费端在 CPU（现在 mpv 的 `vf_vapoursynth` 只吃 CPU 帧）：出 VS 图时仍需一次 `GPUDownload`
  （官方 1080p≈0.75ms、4K16≈5.4ms），净省约 0.9–1.1ms/帧（wall 3.1–3.4 → ~2.1–2.3ms），
  也就是 25–30%，不是"归零"。

4K 时两边都按面积放大：往返 5.4ms 的量级下，驻留改造的收益是 1080p 的 4 倍以上。

## 改造蓝图

### 公共部分（四个引擎都要做）

1. 用 `.venv82` 的头编译，保留 `min(coreApi, header)` 的注册技巧；新建的 GPU 入口在
   `apiVersion < 4.3` 时直接报错返回（`vsapi->getVulkanAPI()` 的空指针/短结构不能碰）。
2. 注册第二个函数名（例如 `FrameGenGPU`，`"clip:vnode:gpu;…"` → `"clip:vnode:gpu;"`），
   不动现有 CPU 入口的 ABI。CPU 入口继续服务 mpv/R74。
3. 输出：`newGPUVideoFrame` + `gpuExecWritesPlane`；slot 0 的直通仍用 `newVideoFrame2`
   （`VSVulkan4.h:895` 明确允许共享未写入的平面）；抑制/切镜的"保持上一帧"用 `newVideoFrame2` 共享全平面，
   零开销。
4. 提交与同步：要么在自己的队列上录命令后 `gpuExecSubmit(pool)`（走 core 的 producer pairs），
   要么引擎自有队列 + `exportGPUSemaphore` / `setGPUPlaneProducer` 回签。第一版建议后者最简：
   引擎跑完 `cuStreamSynchronize`/fence 等待后直接 `setGPUPlaneProducer(信号已完成的 timeline 值)`，
   代价是占住一个调用线程（与现在 D3D12 串行 mutex，`ngx_session.cpp:77`，等价）。
5. GPU 滤镜要求**恒定格式与尺寸**，现有 `vi` 已固定，`multi` 输出同尺寸同格式，满足条件。

### vvfg：先做这个

- 输入：`getExportableFrameFilter` + `exportGPUPlane(frame, plane)` → `cuImportExternalMemory`
  （`CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32`）→ 以 VS 的 `stride` 建 pitched 设备指针。
  YUV→RGB 的 pack 从 CPU（`vfg_session.cpp:684-726`）改成 CUDA kernel。
- 输出：VFG 写到私有 RGB10A2 面，再一个 kernel 做解包 + 色度下采样直写导入的 VS 平面
  （对应 `vfg_session.cpp:807-857` 的 CPU 版本）。
- 同步：VS 平面 → CUDA 用 `cuImportExternalSemaphore`（VS 导出的 timeline），CUDA → VS 用
  `cuSignalExternalSemaphore` + `setGPUPlaneProducer`。
- 预期：PCIe 与 CPU 打包全部消失，逐帧只余模型本身（run 的 GPU 时间）。

### vdlssg / vxessfg：能做，但留一次显存内搬运

D3D12 不能写 vkBuffer。最短路径（按现有代码改动最小）：

1. 引擎输出保持 `D3D12_HEAP_TYPE_DEFAULT` 的 RGBA16F，加 `D3D12_HEAP_FLAG_SHARED`
   + `CreateSharedHandle`。
2. CUDA 侧 `cuImportExternalMemory(CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE)` 导入，
   运行端口现有的"颜色→planar YUV"转换（现在是 D3D12 拷进回读堆后 CPU 做，`d3d12_ctx.cpp:460-471`），
   改成 CUDA kernel 直接写 VS 平面。
3. 输入方向同理：VS 平面 → CUDA 读 → 写进 D3D12 纹理的共享 staging（或 `cuMemcpy2D` 到共享缓冲再
   `CopyBufferToTexture`）。这一步可以用 GPU 拷贝引擎，约 6MB/帧 ≈ 几十微秒。
4. 同步：D3D12 fence → 共享句柄 → Vulkan/CUDA timeline；或者第一版直接引擎 sync 后主机侧回签。

结果：PCIe 归零，每帧多 1–2 次显存内拷贝（各 ~0.02–0.05ms），比现在的 rb+cp（1.6–1.9ms）低一个量级。

### vfsr4：Vulkan 对 Vulkan，反而干净

自有 device 导入 VS 平面导出的 `OPAQUE_WIN32` 内存，`vkCmdCopyImageToBuffer` 直写（FSR 输出本来就在
VkImage 上），跨 device 用导出的 timeline semaphore 同步。风险是 FSR bridge 的共享面生命周期与
"每帧一次"约束——这条路线改动集中在 `fsr4_gpu.cpp` 的 readback（`:224-…`）与 `fsr4_vk_device.cpp` 的
外部内存支持查询。

## 风险与坑

- **设备唯一性**：R80+ 规定插件不得自建 instance/device，只能用 core 的 device。vvfg 现在用 CUDA
  独立上下文（每 lane 一个），CUDA context 与 core VkDevice 是两套对象，靠 UUID 对齐设备即可（VS 已给
  `VSVulkanCoreInfo.deviceUUID`），这不违反规则；vfsr4 的自建 VkDevice 是违规的，只能走"跨设备导入"。
- **VRAM 记账**：本机 VS 上限 6190MB（`core.max_vram_cache_size`），DLSS-G 自己的 NGX/纹理开销另一份账。
  4K + multi=6 在 10GB 卡上要实测，别先把预算吃满。
- **并发**：现在每帧 GPU 工作被一把 mutex 串起来（`ngx_session.cpp:77`）。驻留后瓶颈转向队列，
  但 D3D12 单队列 + NGX 的串行性不变，多线程取帧不会自动变快。
- **Python/调试**：GPU 帧不能 `getReadPtr`，报 "The frame is GPU resident …"；调试要 `std.GPUDownload`。
- **链路混用告警**：只要链上有任一 CPU 滤镜，VS 会插隐式 transfer 并打 warning——这也是一条快速验证
  "链是否真的全程驻留"的探针。
- **播放端**：mpv `vf_vapoursynth` 无 hwframe 路径，端到端收益只在 vspipe/编码或后续驻留滤镜之后。

## 实施建议（分阶段）

1. **最小验证（1–2 天）**：编一个 `FrameGenGPU` 壳（不做推理），输入 `vnode:gpu`、输出
   `newGPUVideoFrame` + 一次 CUDA 拷贝（或纯 Vulkan `vkCmdCopyBuffer`），跑
   `Upload → FrameGenGPU → Download` 与 CPU 版对比，确认 producer pair 与 VRAM 记账没问题。
2. **vvfg 全驻留**：把 pack/unpack 搬上 GPU，接真实模型，对齐 `docs/vdlssg-opt12.md` 的计时口径出表。
3. **dlss/xess 共享纹理搬运**：先只做输出方向（最贵的 cp+rb），输入方向可继续用现有上传堆。
4. **vfsr4**：视 FSR bridge 的共享面约束再定，优先级最低。

## 未决问题

- `getExportableFrameFilter` 在离散卡 + resizable BAR 下会多一次平面拷贝（`VSVulkan4.h:999-1004`），
  我们的 3080 是否在列需实测；R82 已修 "resizable BAR 下跑 PCIe 速度" 的问题，但仍要量。
- VFG 的 `SrcImage` 是否接受"外部内存映射出来的 pitched 指针 + 非对齐 pitch"，还是要求 `cuMemAlloc`
  出来的分配；这决定输入方向能否真正零拷贝。
- DLSS-G 的 `D3D12_HEAP_FLAG_SHARED` 纹理是否允许 `RENDER_TARGET` 绑定并在 NGX 写完后立即被 CUDA 读；
  需要 fence 对齐（NVOF 侧同理）。
