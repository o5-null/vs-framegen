# SmoothMyVideo 的 DLSS 4.5 在 RTX 20/30 上跑通

结论：**可行，已在本机 RTX 3080 上端到端跑通**。做法是把
[dlssg_for_sm86 0.4.2](../参考插件/dlssg_for_sm86) 当作解锁器塞进 SmoothMyVideo 自己的
帧生成宿主 `dlssg2f.exe`，并放宽 app 侧的 GPU 门限。产物是
[`dist/smv-dlssg-unlock`](../dist/smv-dlssg-unlock)（`install.bat` 就地安装/还原）。

本文记录机制、证据与坑；用法见该目录的 `README.md`。

## 1. 为什么原版在 RTX 20/30 上失败

SmoothMyVideo 的 `engine\dlssg\nvngx_dlssg.dll` 是官方 DLSS-G provider 310.9.16
（`NVIDIA DLSS-G MFGLW - DVS PRODUCTION`，7,506,544 B，SHA256 `8D59D4…`）。
和 MFG-Unlocker 审计的所有版本一样，它在两处把 Ada 声明为最低架构：

- 导出 `NVSDK_NGX_GetGPUArchitecture`：`mov eax,0x190; ret`（`0x190` = sm_89）；
- `NVSDK_NGX_D3D12_GetFeatureRequirements` 的 request record：FG feature id `0x0B`
  之后紧跟 `0x190`。

NGX core 用前者做 snippet metadata 与 `CreateFeature` 校验，于是 RTX 30 上得到
`NGXValidateSnippetMetaData:1499 error: snippet is using newer GPU arch 190 > 170`
→ `CreateFeature 0xbad0000b`。后者是 Streamline 读插件最低架构用的，离线宿主用不到，
但解锁器一起改。

MFG-Unlocker 的做法（`source/native/ampere_provider_arch.h`、
`ampere_gpu.cpp`、`ampere_patterns.h`）是在进程内存里改这些立即数，并在 provider
首次 pipeline create 之前把内嵌的 SM89 PTX 翻译成 SM86 cubin、替换内核表。它自己那条
路还要过 “GPU-program preparation + capacity” 两道门，代码量极大（`patcher.cpp` 536 KB、
`dlss5.cpp` 157 KB）。

**但本机已经有更小的等价物**：`参考插件\dlssg_for_sm86` 0.4.2（sdli1995，MIT）——
`version.dll` 代理 + `backend\sm86_backend.dll` + 自有 `kernels\fg_kernels.dll`
（18 MB，SM86/SM75 重编内核）+ 未修改的 310.9.16 provider。本项目 vdlssg 插件早就靠它
在 3080 上跑 DLSS-G。它的 310.9.16 内核表与 SmoothMyVideo 的 310.9.16 完全对应
（MFG-Unlocker 里那张 `k310916KernelCount=26` 表就是同一份 payload）。

## 2. 只改宿主的三处

原版 `engine\dlssg\build_src\ngx_direct.h` 的 `coreStart()` 只把 exe 目录交给 NGX：

```cpp
const wchar_t* paths[] = {exeDir.c_str()};
fci.PathListInfo.Path = paths;
fci.PathListInfo.Length = 1;
```

两个后果：代理没人加载（NGX 由 `nvsdk_ngx_s.lib` 静态链，Init 时才打开驱动的 NGX），
且即使加载了，snippet 也会从 exe 同目录找到 Ada-only 那支。

补丁（`dist/smv-dlssg-unlock/payload/src/ngx_direct.h`）：

```cpp
const std::wstring unlocker = unlockerDir(exeDir);   // exe 旁 version.dll，或 SMV_DLSSG_PROXY
loadUnlocker(unlocker);                             // 建 dlssg_sm86\logs + LoadLibraryW
const std::wstring runtimeDir = unlocker + L"\\dlssg_sm86\\runtime";
const wchar_t* paths[] = {exeDir.c_str(), unlocker.c_str(), runtimeDir.c_str()};
common.PathListInfo = {paths, unlocker.empty() ? 1 : 3};
```

没有解锁器时 `Length` 仍为 1，行为与原版逐字一致，所以这份 exe 也能直接替换到原版目录里
（只是不解锁）。`loadUnlocker` 里两个 `CreateDirectoryW` 是必需的：代理只往
`dlssg_sm86\logs` 追加写，不自建目录。

## 3. app 侧只改一行正则

`resources\app.asar` 的 `dist\main-setup.js`（解包后第 367 行）：

```js
resolve({ gpu, hags, supported: hags && /GeForce RTX\s*(40|50)\d{2}\b|\bAda\b|Blackwell/i.test(gpu) });
```

`supported` 只被渲染层用来决定**是否默认选中** DLSS 4.5
（`renderer/app.js`：`dlssDefault = dlssReady && d.supported`），不能选的门槛是
`dlssReady`（两个文件存在即可）。所以这行不改也能手动用，改了才会在 20/30 上默认用。

asar 重打包的要点（`dist/smv-dlssg-unlock/install.py`）：

- 布局：`4 | headerSize | headerSize-4 | jsonLen`（原包 `(4, 9460, 9456, 9449)`），
  header 区从 offset 8 起共 `headerSize` 字节 = 后两个字段 + JSON + 4 字节对齐 padding；
  payload 从 `8 + headerSize` 开始。
- 每个文件带 `integrity{algorithm, hash, blockSize: 4194304, blocks[]}`，重打包必须重算
  （4 MB 分块 SHA256）。
- **验证方法**：原包 → 解包 → 重打包 → 再解包，37 个文件逐字节一致，且重打包文件大小
  与 header 字段与原包完全相同（1,539,096 B / `(4, 9460, 9456, 9449)`）——说明序列化与
  `@electron/asar` 的输出结构一致。
- 装好后启动 `SmoothMyVideo.exe` 12 秒仍有主进程 + 3 个子进程存活（asar 未损坏）。

## 4. 证据

### 4.1 宿主直连（`work\smv\run2`，含 app 自带 `nvngx_dlssg.dll`）

640×360，`--gen 5`，6 帧输入 → `exit 0`，handshake `DLSSG READY gen=5 max=5 wq=1 ngx=1`，
输出 25 帧（(6-1)×5）。关键日志：

```
NGX: unlocker loaded from E:\code\dlssg-vfi\work\smv\run2
NGX core: ...\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\_nvngx.dll
NGXValidateSnippetMetaData:1499 error: snippet is using newer GPU arch 190 > 170
Found dlssg driver fallback snippet
NGXCubinGeneric::SetGPUArch:473] m_gpuArch = 0x170        <- 解锁器改写生效（Ampere）
app 3AC09EF feature dlssg snippet: ...\dlssg_sm86\runtime/nvngx_dlssg.dll version: 310.9.16
DLSS-G ready: NGX's own calls
```

1080p 实测（`work\smv\run_real.py`，`测试数据\1080p` 40 帧 → 1920×1080，`--gen 5`）：
输出 195 帧（39 对 × 5），`server done, 195 interpolated frames`，抽帧目视干净。

运动正确性：合成素材里 24 px 盒子每输入帧右移 10 px（真实中心 `32 + 10·index`），
5 张中间帧质心 33 / 35 / 36.5 / 38 / 40，理论 33.7 / 35.3 / 37 / 38.7 / 40.3
（差值来自盒子整像素量化）→ 6x 插帧内容真实正确，不是复制帧。

### 4.2 走 app 自己的管线（安装后）

`resources\engine` 下跑 `native.js` 里 `dlssgLimit()` 用的同一条命令
（`smv-live.exe --offline … --dlssg`），它内部 spawn `engine\dlssg\dlssg2f.exe --server`：

```
DLSS Frame Generation ready (256x128, 2x, server max 5)
offline host ready:256x128 rgb24 in, 256x128 rgb24 out, x2, padded 256x128, graph off, model dlss 4.5
server done, 1 interpolated frames
```

`server max 5` → app 换算 `limit = 6`（单趟 6x）。原版在 RTX 40 上是 `max 1`（2x），
RTX 50 上 `max 5`。所以这台 3080 现在拿到的是和 RTX 50 相同的单趟倍率。

### 4.3 解锁器自己的日志（证明走的是"翻译内核"这条路）

安装后那次探测在 `resources\engine\dlssg\dlssg_sm86\logs\dlssg_sm86.log` 留了 189 行，
关键几条：

```
{"event":"backend_loaded","id":"dlssg-310.9.16-d3d12-sm86","loaded":true,
 "expected_runtime_sha256":"cadee1aa…","expected_runtime_version":"310.9.16.0"}
{"event":"kernel_pack","image":"cubin_sm86","images":444,"kernels":71,
 "kernels_matched":71,"kernels_unused":0,"manifest":"fg-kernels 310.9.16 7710da62f151"}
{"event":"install","active":true,"actual_sm":86,"executed_on_sm86_hardware":true,
 "executed_on_sm75_hardware":false,"frame_generation_refused":false}
{"event":"cu_chain_launch","rewritten":true,"first":{"entry":"custom_block0_conv0_c8_kernel",…}}
{"event":"fg_gate_exit","clean":true,"create_failures":0,"evaluates":4,"features":1}
```

`kernels_matched: 71`、`rewritten: true`、`executed_on_sm86_hardware: true` 说明跑的是
`fg_kernels.dll` 里重编的 SM86 cubin，不是原 provider 的 SM89 payload；`create_failures: 0`
说明架构门与 create 校验都没拦。注意它绑的是**随包**那份 310.9.16
（`cadee1aa…`），app 目录里的 `nvngx_dlssg.dll`（`8D59D4…`）虽然同版本同尺寸但没被用到。

## 5. 坑与边界

- **`dlssg_sm86\logs` 必须存在**：代理不会自建，缺目录时日志与部分初始化路径会静默失败。
- **代理必须在 NGX Init 之前常驻**：`LoadLibraryW` 放在 `coreStart` 里、`NVSDK_NGX_D3D12_Init`
  之前；晚一步就完全拦截不到。
- **`MaxGeneratedFrames` 语义**：ini 里写的是“额外生成的帧数”，`5` = 6x。解锁器只接受
  `1`/`5`（`ampere_policy.h` 的 `CapacityValid`）或 `1..5`（certified ring），写别的值会被
  钳到 2x。app 侧读 `server max` 后自己 `+1` 得到档位上限，倍率由它拆分趟数，不需要改。
- **HAGS**：`supported` 里 `hags` 条件保留。DLSS-G 依赖硬件加速 GPU 调度，关掉时 app 不会
  默认选中（仍可手动试）。
- **VRAM**：3080 10 GB 上 1080p 6x 没有问题；4K 6x 的单趟 ring 明显更吃显存，遇到
  `out of memory` 时把 `MaxGeneratedFrames` 降回 `3`（4x）。
- **RTX 20（SM75）未实测**：解锁器按物理卡自动挑 SM75 内核族，理论上同一份文件即可；
  本机无 Turing 卡，无法给出实测结论。TU116/TU117（如 GTX 1660）没有 tensor core，
  解锁器会直接排除，不支持。
- 只动 `resources\engine\dlssg`（宿主 + 解锁器）与 `resources\app.asar`（一行正则）
  两处，均可还原（`dlssg2f.exe.bak` / `app.asar.bak`）。

## 6. 复现

```
dist\smv-dlssg-unlock\install.bat                 # 装到 参考插件\SmoothMyVideo-protable
dist\smv-dlssg-unlock\install.bat --revert        # 还原
work\smv\probe_installed.py                       # 走 app 管线验证 server max == 5
work\smv\test_asar.py                             # asar 解包/重打包逐字节回环
work\smv\test_revert.py                           # --revert 还原 + 重装 + 二次安装幂等
work\smv\run_real.py                              # 1080p 真实素材 6x 抽帧
```

宿主源码与编译脚本在 `dist\smv-dlssg-unlock\payload\src`（`build.bat` 需要
`NGX_SDK=E:\code\dlssg-vfi\ngxsdk` 与 `vcvars64.bat`；首次编译若报 `C1083 windows.h`
就是没 call vcvars64）。
