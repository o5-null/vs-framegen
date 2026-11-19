# 第三方运行时的许可与实际部署

两个问题分开答：**仓库里怎么处理**（不进 git，只留获取与放置说明），**跑起来放哪里**
（`bin\<引擎>\`，插件自己在 DLL 旁边找）。逐文件许可表在根目录 `THIRD_PARTY_NOTICES.md`。

## 1. 仓库里怎么处理

本仓库是源码仓库，提交的是：

- `vs-dlssg\src\` 四个插件的 C++ 源码与 `build.bat`；
- `bench\src\` 基准脚本，`docs\` 文档；
- `bench\results\` 的报告与图表快照（自己生成的，几 MB）。

不提交的是：NVIDIA/AMD/Intel 运行库、社区代理、vkd3d、模型与生成帧。`.gitignore` 用一条
`/vs-dlssg/bin/` 覆盖了构建产物和四个引擎包——`bin\` 是我们约定的"本机可重建目录"，里面的
东西都能从上游拿到或自己编出来。

这样处理的原因有三层，不只是体积：

1. **许可**。NVIDIA RTX SDKs License 允许把运行时"as incorporated in object code format
   into a software application"分发，但明确禁止"distribute or sublicense the SDK as a
   stand-alone product"、禁止逆向与修改。把 `nvngx_dlssg.dll` 交给 git 历史，等于给 SDK 组件
   开了一个独立的、永久的下载渠道，正好落在禁止的那一侧。
2. **代理与解锁的状态**。`dlss45` / `xessfg` 里的 `version.dll`（`dlssg_for_sm86`）不是 NVIDIA
   的资产：它绕过架构门、改写运行库的加载路径，属于本机自用的补丁。VFG 侧的解锁更轻——它在
   `vvfg.dll` 进程内完成（`vfg_session.cpp` 改导入表，见 `vs-dlssg\docs\engines.md`），不需要额外
   文件，但同样是针对运行库的本地修改。这些都适合留在本机 `bin\`，不适合作为仓库内容传播。
3. **可复现**。二进制进 git 只会让仓库变重、让"这版 DLL 是哪来的"变模糊；源码 + 获取步骤
   反而能对齐到具体版本。

## 2. 跑起来放哪里

四个引擎一个 DLL，包目录名与 DLL 名对齐，全部平铺在同一个 `bin\` 下：

```
vs-dlssg\bin\
├─ vdlssg.dll  vxessfg.dll  vvfg.dll  vfsr4.dll      ← 构建产物
├─ dlss45\    version.dll + dlssg_sm86\{backend,kernels,runtime,xefg,logs}\ + dlssg_sm86.ini
├─ xessfg\    同一代理，dlssg_sm86.ini 里 Engine=XeSS、MaxGeneratedFrames=3
├─ vfg\       nvVFXVideoFrameGeneration.dll / NVCVImage.dll / NVVideoEffects.dll / cudart64_12.dll
└─ fsr4\      smv_fsrfg_bridge.dll + AMD FidelityFX 运行库 + vkd3d-proton
```

插件找包的顺序（`vs-dlssg\src\frame_gen.cpp:298` 的 `isProxyDir`，`:315` 的默认搜索）：

1. `proxy_dir` 参数给了就用它，不再猜；
2. 否则看 `<本 DLL 所在目录>\<包名>` 是不是有效包——`version.dll`（dlss45/xessfg）、
   `nvVFXVideoFrameGeneration.dll`（vfg）、`smv_fsrfg_bridge.dll`（fsr4）任一存在即认；
3. 都不是就退回 DLL 所在目录本身（只有原版 4× 上限，没有 6× / XeSS / VFG 解锁）。

所以部署就是"把 DLL 和它那一包平铺到同一个插件目录"：

```
<vapoursynth>\vapoursynth64\plugins\vdlssg.dll
<vapoursynth>\vapoursynth64\plugins\dlss45\...
```

三个必须注意的点：

- `dlss45\dlssg_sm86\logs\` 必须预先存在，代理自己不建目录，缺了是零日志静默失败；
- `dlss45` / `xessfg` 必须从含 `version.dll` 的目录启动，代理要被显式拉起；`vfg` 反过来不需要
  代理文件，解锁在 `vvfg.dll` 进程内改导入表；
- 一个进程只能初始化一次 NGX。同进程加载 `vdlssg` + `vxessfg` 会让第二个失败，所以
  `vs-dlssg\test\all_engines.py` 每个引擎起一个子进程。

## 3. 各包从哪拿

| 包 | 步骤 |
|---|---|
| `dlss45` | 拿 NVIDIA DLSS SDK（`developer.nvidia.com` 登录后下载）或从已安装的同版本游戏/应用里取 `nvngx_dlssg.dll`；再叠上 `dlssg_for_sm86` 代理（`version.dll` + `dlssg_sm86\` 三件套 + ini）。本机现成的一份来自 SmoothMyVideo 便携版，见 `docs\smv-dlssg-unlock.md` |
| `xessfg` | 同一代理包，改 `dlssg_sm86.ini`：`Engine=XeSS`、`MaxGeneratedFrames=3`。XeSS-FG 走代理自带的 CUDA 内核，不需要 `libxess_fg.dll` |
| `vfg` | NVIDIA Video Effects SDK 的四个文件。3080 上的门由 `vvfg.dll` 自己打开（进程内改导入表），不需要 `winmm.dll`；`docs\smv-vfg-sm86-unlock.md` 记录的是同一套 hook 在参考宿主里的做法 |
| `fsr4` | SmoothMyVideo 的 `fsrfg` 包：桥 DLL + AMD FidelityFX 运行库 + 打过 patch 的 vkd3d-proton；包内 `licenses\` 与 `source\` 是随包义务文件，别删 |

`vs-dlssg\test\smoke.py` 可以绕过 VapourSynth 直接验证包是否可用：默认就找
`..\bin\dlss45`，`python vs-dlssg\test\smoke.py 640 360 5 6`。

## 4. 再分发的边界

| 场景 | 结论 |
|---|---|
| 本机自用 / 内部测试 | 全都可用，按第 2 节摆好即可 |
| 发布插件（`v*.dll` + 源码） | 可以。属 §1(c) 的"并入应用的目标码"，随分发物附 `runtime\nvngx_dlss.license.txt` |
| 发布整包（DLL + 引擎包） | 谨慎：`fsr4` 侧（AMD MIT 式 + vkd3d LGPL-2.1）没问题，`dlss45` / `xessfg` / `vfg` 侧的代理与解锁不属于可分发内容 |
| 单独提供引擎包下载 | 不要。§3(b) 明确禁止把 SDK 作为独立产品分发 |
| 修改 NVIDIA DLL 后分发 | 不要。§3(a) 禁止逆向/反编译/反汇编 |

LGPL 侧（`fsr4`）的额外义务：分发 `smv_vkd3d_d3d12*.dll` 时要附 LGPL-2.1 文本与"如何取得对应
源码"的说明；本项目已把两份 patch 放在 `bin\fsr4\source\`，对应上游 vkd3d-proton / dxil-spirv
的提交号写在文件名里。

## 5. 许可文本在哪

- `vs-dlssg\bin\dlss45\dlssg_sm86\runtime\nvngx_dlss.license.txt` —— NVIDIA RTX SDKs License 原文；
- `vs-dlssg\bin\fsr4\licenses\` —— AMD FidelityFX SDK、vkd3d-proton（LGPL-2.1）、dxil-spirv、
  glslang、spirv-headers、bc-decoder 的许可文本；
- `THIRD_PARTY_NOTICES.md` —— 一页表格，逐组件写清许可、能否再分发、从哪获取。

仓库本身不复制这些文本：它们随包存在，包不进 git，所以义务跟着包走而不是跟着仓库走。
