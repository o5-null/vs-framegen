# 第三方组件与许可证

本仓库只提交自己写的源码。运行时需要的第三方二进制（NVIDIA / AMD / Intel 运行库、社区
代理、vkd3d）**不在 git 里**：`.gitignore` 忽略 `/vs-dlssg/bin/`，那里放的是构建产物与四个
引擎包。下表说明每个文件从哪来、受什么许可约束、能不能再分发。

## 逐组件

| 组件 | 位置（构建/部署后） | 许可 | 能否再分发 | 从哪获取 |
|---|---|---|---|---|
| DLSS-G 运行时 `nvngx_dlssg.dll` | `bin\dlss45\dlssg_sm86\runtime\` | NVIDIA RTX SDKs License | 可作为应用的一部分以**目标码**随应用分发；**不得**单独分发、不得逆向 | NVIDIA DLSS SDK，或已安装的同版本游戏/应用 |
| `dlssg_for_sm86` 代理：`version.dll`、`sm86_backend.dll`、`fg_kernels.dll`、`xefg_kernels.dll`、`dlssg_sm86.ini` | `bin\dlss45\`、`bin\xessfg\` | 第三方社区产物，**不在** NVIDIA 许可范围内（它改写/替换运行库行为） | **否**。只在本机测试环境使用 | SmoothMyVideo 的 sm86 解锁包 |
| XeSS-FG 运行包 | `bin\xessfg\`（同一代理，`dlssg_sm86.ini` 里 `Engine=XeSS`） | 同上；换成 Intel 原生运行库时适用 Intel XeSS 许可 | 代理否；Intel 运行库可按 Intel 条款随应用分发 | SmoothMyVideo 包 / Intel XeSS SDK |
| VFG 运行时 `nvVFXVideoFrameGeneration.dll`、`NVCVImage.dll`、`NVVideoEffects.dll`、`cudart64_12.dll` | `bin\vfg\` | NVIDIA RTX SDKs License（含 NVIDIA Video Effects SDK 与 CUDA 运行库） | 同上：随应用、目标码、不单独分发 | NVIDIA Video Effects SDK / SmoothMyVideo 包 |
| FSR 4 桥 `smv_fsrfg_bridge.dll` | `bin\fsr4\` | SmoothMyVideo 项目（MIT） | 可以，附 MIT 声明 | SmoothMyVideo 发布包 |
| `amd_fidelityfx_framegeneration_dx12.dll`、`amd_fidelityfx_loader_dx12.dll`、`amdxc64.dll` | `bin\fsr4\` | AMD FidelityFX SDK 许可（MIT 式，见包内许可证） | 可以，须附许可与版权声明 | AMD FidelityFX SDK |
| `smv_vkd3d_d3d12.dll`、`smv_vkd3d_d3d12core.dll`（本地打过 patch） | `bin\fsr4\` | vkd3d-proton，LGPL-2.1 | 可以，但须附 LGPL 文本并提供获取对应源码的途径；patch 已随包（`source\*.patch`） | vkd3d-proton |
| `dxil-spirv`、`glslang`、`spirv-headers`、`bc-decoder`（编进上面的 DLL） | `bin\fsr4\licenses\` 内有许可文本 | MIT / BSD-3 / CC-BY-4.0 / MIT | 可以，附声明 | 上游仓库 |
| NGX SDK 头文件与 `nvsdk_ngx_d.lib` | `repos\ngxsdk\`（**不入库**，独立 clone） | NVIDIA RTX SDKs License | 否，仅供构建链接 | `git clone https://github.com/NVIDIA/DLSS repos/ngxsdk` |
| VapourSynth 头文件 | 构建环境 | LGPL-2.1 | 不随本仓库分发 | VapourSynth |

## 三条实践结论

1. **自己用 / 内部测试**：不受影响。按 `docs\licensing.md` 的布局把包放到 `bin\<引擎>\`，
   插件默认就在 DLL 旁边找它。
2. **对外发插件**（只发 `v*.dll` + 源码）：可以。NVIDIA 许可允许把它作为应用的一部分以目标码
   分发，条件是随分发物附许可文本——包内 `runtime\nvngx_dlss.license.txt` 就是原文。但不要
   把 SDK 说成你在分发，也不要把它单独挂出来下载。
3. **对外转发引擎包**（尤其带 `version.dll` 代理的 `dlss45` / `xessfg`，以及进程内解锁的
   `vfg`）：不要。代理不属于 NVIDIA 可分发资产，且 NVIDIA 许可禁止逆向/修改运行库与"作为
   独立产品分发 SDK"。这类包只在本地机器上使用。

引用到的许可条款（NVIDIA RTX SDKs License，原文见包内 `nvngx_dlss.license.txt`）：

- §1(c)：允许以目标码形式并入软件应用分发，须满足许可中的分发要求；
- §2(b)：使用 NVIDIA 提供的源码时须保留 "This software contains source code provided by NVIDIA Corporation."；
- §3(a)：不得逆向、反编译、反汇编或移除专有声明；
- §3(b)：不得把 SDK 作为独立产品分发或再许可。

完整说明（放哪、怎么拿、边界在哪）见 `docs\licensing.md`。
