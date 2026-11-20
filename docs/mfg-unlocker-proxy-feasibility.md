# MFG-Unlocker 能不能做 DLSS-G 的 sm86 代理

日期：2026-11-19
范围：`参考插件\MFG-Unlocker`（v1.7.1，本地源码）、`dist\dlss45\dlssg_sm86`、`vs-dlssg\src`

## 结论

**直接拿它当 `dlssg_for_sm86` 的替代代理：不建议。**
它的 proxy 外壳与一个 5 MB 的游戏后端绑死（菜单、Streamline、XeSS、swapchain、单 LUID），
不是可拆出来单独加载的“解锁器 DLL”；源码 checkout 也缺官方发行版的内核缓存，不能复现发布 DLL。

**真正有价值的是它的 Ampere 内核翻译层。** 它证明了一条比“写代理”更短的路：

> 我们的插件自己就是 NGX host，不需要代理、不需要 hook 导出。
> 只要在 `Init` 前 `LoadLibrary` 官方 provider，改两处架构立即数；
> 在 `CreateFeature` 前找到 provider 的 48 字节 kernel 描述符表，
> 把每个 slot 的 sm_89 PTX 改成 sm_86 后写回即可。

这条路的代码量约 **900–1200 行**（MIT 可参考/摘录），
不需要 `version.dll`、`sm86_backend.dll`、`fg_kernels.dll`，也不需要按 LUID 选卡。
它是目前唯一同时满足“源码可复现、不随包发 NVIDIA 内核、任意单卡可用”的方案。

一句话：**当 proxy 不方便；当“不用 proxy”的参考实现，很方便。**

## 1. 它其实不是 proxy，是进程内 provider 补丁

MFG-Unlocker 的交付形态是一个可改名的 DLL（`version.dll` / `winmm.dll` … 19 个家族名），
`DllMain` 里按自身文件名选代理导出、加载原版系统 DLL、转发导出（`single_module.cpp:682-758`）。
这部分是给游戏用的“代理外壳”，与解锁逻辑没有解耦。

Ampere 解锁本身在 `source/native`：

- `LdrRegisterDllNotification` 发现 `nvngx_dlssg.dll` 载入（`patcher.cpp:10359-10364,10509-10564`）；
- `ampere_provider_arch::Declare()` 在进程内改 provider 的两处架构声明（`ampere_provider_arch.cpp:281-303`）；
- 在 provider 首次建 feature 前，定位 kernel 描述符表，逐 slot 重写 fatbin（`ampere_gpu.cpp:1443` 起）。

它**没有** hook CUDA，也没有 hook `cuModuleLoadData`。provider 仍然走自己的
`nvapi_QueryInterface` → `NvAPI_D3D12_CreateCuModule` / `CreateCuFunction` / `LaunchCuKernelChain`；
MFG 只是让 provider 拿到一份“目标架构对”的 PTX fatbin，后面由 provider 自己交给驱动。

## 2. Ampere 路径的四步

1. **架构门**：provider 的 `NVSDK_NGX_GetGPUArchitecture` 是 `B8 90 01 00 00 C3`
   （`mov eax,0x190; ret`，0x190 = Ada）。进程内把立即数改成 `0x170`（Ampere）或 `0x160`（Turing）；
   同时改 `NVSDK_NGX_D3D12_GetFeatureRequirements` 记录里 FG feature id `0x0B` 后的 `0x190`。
   不落盘、不改 NVIDIA 文件。
2. **找表**：扫 `.pdata` 的注册函数，用三组字节模式匹配注册循环，校验描述符名字/entry 名，
   得到 26/27/28/33/38 个 slot 的指针表；`table = anchor - temporalSlot * 8`。
   310.9.16 是 26 个 slot，midpoint 在 slot 10（`ampere_gpu.cpp:137-173`）。
3. **翻译**：每个描述符 48 字节：`+0` 名字、`+8` fatbin 指针、`+16` supplied size、`+24` entry 名。
   解析 fatbin 外层（magic `0xBA55ED50`），要求恰好一个 `kind==1 && arch==89` 的 PTX 条目；
   解 NVIDIA LZ 压缩，把唯一 `.target sm_89` 改成 `.target sm_86`，重写条目头 arch=86，
   拒绝 `.e4m3/.e5m2/wgmma./tcgen05./mma.sp::ordered_metadata/sm_90/sm_120`。
4. **发布**：`VirtualAlloc` 一块 `48 + 新 fatbin`，复制描述符，改 `+8/+16` 指向新 blob，
   `VirtualProtect(PAGE_READONLY)`，再把表项 CAS 到 clone。失败可回滚；不改 provider 文件。

这是一套完整的事务：先为 26 个 slot 全部准备、预检，确认全部成功后才发布，绝不混用一半新一半旧。
`ampere_cuda_program.cpp` 还会用临时 CUDA context 在目标 LUID 上逐个 `cuModuleLoadData +
cuModuleGetFunction` 预检，把失败提前到 `CreateFeature` 之前——这是最值得抄的工程习惯。

## 3. 为什么不能直接换上去

| 障碍 | 事实 |
|---|---|
| 形态不匹配 | `MFGUnlocker.dll` 是游戏代理 + 后端单体，不是库；加载后要挂 DLL 通知、D3D12/DXGI/Streamline 钩子、内置菜单。我们的 host 没有 swapchain，也不需要被劫持。 |
| 仍然单卡 | `gpu_selection_policy.h:1-36` 一个进程只接受一个 LUID，之后遇到不同设备直接 `eConflict`；`ampere_gpu.cpp:1286-1292` 已发布 LUID 变化就 `eRestartRequired`。拿它当代理会**重新引入**现在的多卡问题。 |
| 缺构建输入 | `BUILD.md:37-58`：validated SM86 kernel cache（`native-cache*/RTX30MFG-Kernels/*.fatbin`、manifest、`all-provider-layouts/...`）是单独准备的 build input，源码 checkout 不能复现 DLL。本机没有这些文件。 |
| 资产许可 | 代码是 MIT（`LICENSE`），但仓库不含 NVIDIA 内核；官方发行 DLL 里的内核缓存不在 MIT 内。直接分发它的二进制并不比现在更干净。 |
| 体量 | 核心源文件之外还有 `patcher.cpp`（536 KB）、`dlss5.cpp`（157 KB）、ImGui、Streamline、XeSS；`CMakeLists.txt:48-50` 还强制 embedded kernels、XeSS 不可关。拆不出轻量代理。 |

补充：native cache 不是必需。`ampere_native_cache.cmake` 默认就是 PTX 路径，
只有 `CUBIN` 模式才强制 manifest；没有缓存时仍可由驱动 JIT。因此“缺 fatbin”只影响冷启动和稳定性，
不是自研的绝对阻塞。

## 4. 三条路线

| 路线 | 做法 | 工作量 | 评价 |
|---|---|---|---|
| **A（推荐）** | 插件内 `LoadLibrary(provider)` → 改架构立即数 → `FindDescriptorEntry` → 26 个 slot 的 PTX 重定向并发布 → `Init/CreateFeature` | 核心 900–1200 行；3–5 天出原型，再 3–5 天稳定化 | 无代理、无 hook、无私有内核包；任意单卡可用；源码可复现 |
| B | 自己 hook `nvapi_QueryInterface` 返回的 CUDA 三桩，或改 provider IAT，把 blob 换成 sm86 内核 | hook 1–2 天；内核流水线 1–2 周 | 只在 A 的表定位实在打不通时兜底；许可证不改善 |
| C | 维持现状：`version.dll` + `sm86_backend.dll` + `fg_kernels.dll` | 0 | 已验证可用，但黑盒、单卡、无法只靠源码交付 |

### A 与现有代理的关系

- `fg_kernels.dll` 的 manifest 已经有 71 个 provider 源 fatbin 的 SHA-256 映射，
  可以作为后续“cubin 快路径”；但 A 的第一版不需要它，直接用 provider 自带 PTX 改 target 即可。
- 做完 A 后，`dlssg_for_sm86` 整个目录可以从发布包里去掉；
  仍然随包的是 NVIDIA 官方 `nvngx_dlssg.dll`（这一项许可证问题不变）。
- 多卡：A 不选 LUID。provider 在哪个 D3D12 device 上建 feature，就在哪张卡上加载内核；
  `gpu=N` 选任意一张 3080 都能工作。NGX 仍然每进程只能 `Init` 一次，所以“一个进程同时用两张卡”
  依旧不支持，但这不是本次要解决的问题。

## 5. 先做哪个 spike

建议按“能失败得早”的顺序：

1. 在 `vdlssg` 的 `ngx_session.cpp` 里，`NVSDK_NGX_D3D12_Init` 之前
   `LoadLibraryW(runtimeDir\nvngx_dlssg.dll)`，按 `single_module` 的模式钉住模块；
2. 实现 `DeclareArchitecture()`：只改 `GetGPUArchitecture` 和 `GetFeatureRequirements` 两处，
   先让 3080 上的 `0xbad0000b` 消失（这一步不碰内核表，失败面最小）；
3. 实现 `FindDescriptorEntry`，只打印命中表和 26 个 slot 的名字/大小，不发布；
   确认 310.9.16 的布局扫描正确；
4. 只对 slot 10（`EstimateIntermMvecsScatter`）做 PTX 重定向并发布，`CreateFeature` + 两趟 `Evaluate`，
   看是否出帧；成功后扩到 26 个 slot；
5. 验收：`vspipe test\mf6.vpy NUL` 在 `gpu=0` / `gpu=1` 都能跑；不加载 `version.dll`；
   日志里能看到 provider 的 RVA/slot 命中，失败时能回滚到未打补丁状态。

## 6. 风险

- **时序**：provider 必须在 NGX `Init` 前载入并改好架构；如果 NGX core 自己先加载了，需要
  `LdrRegisterDllNotification` 或预加载 + 模块钉住。先在 3080 上实测。
- **表定位**：`FindDescriptorEntry` 依赖 `.pdata` 注册循环和字节模式；只对 310.9.16 做白名单，
  版本不符直接拒绝，不要猜。
- **JIT 冷启动**：26 个 PTX 首次编译可能让首帧多等数秒；可接受就先跑通，后续再用
  `fg_kernels.dll` 的 cubin manifest 做快路径。
- **PTX 兼容**：黑名单只覆盖已知的 sm_89 专有指令；换 provider 版本必须重新审计。
- **发布安全**：描述符表在只读页，发布用 CAS/`PAGE_READONLY`，失败要能回滚；
  不要让一半 slot 指向新 blob。

## 7. 参考位置

- `参考插件\MFG-Unlocker\source\native\ampere_gpu.cpp:137-173`（310.9.16 的 26 个 kernel 名）
- 同文件 `:487-551`（48 字节描述符校验）、`:553-780`（注册循环/表定位）、
  `:787-848`（NVIDIA LZ 解压）、`:995-1135`（PTX sm_89→sm_86）、`:1443-1668`（准备/发布事务）
- `source\native\ampere_provider_arch.cpp:281-303`（架构立即数）
- `source\native\single_module.cpp:682-758`（可改名代理外壳）
- `source\native\ampere_backend.cpp:3039-3106`（对 NGX 导出的 detour，游戏路径用）
- `BUILD.md:37-58`（内核缓存是外部 build input）
- `docs\vdlssg-adapter.md:20-32,90-96`（现有代理为什么单卡、还没做什么）
- `docs\dlssg-on-3080-spike.md:9-57`（我们的 NGX host 调用序列）
