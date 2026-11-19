# 多显卡机器：会话跑在哪张卡上

两块（或更多）NVIDIA 卡时，`core.vdlssg.FrameGen` / `core.vxessfg.FrameGen` 曾经会因为选错适配器
直接失败。原因是 sm86 代理只覆写**一张**卡，而插件原来按 DXGI 序号取卡（`gpu` 缺省 = 0）。
现在改成插件自己跟随代理的选择，`gpu` 只在你确实想指定时写。

## 现象与实测

双 RTX 3080（`0000:01:00.0` 无显示器、`0000:08:00.0` 带桌面），驱动 617.42，HAGS 开：

| `gpu=` | 结果 |
|---|---|
| `0`（旧缺省） | `NGXCubinD3D12::CreateKernel NvAPI_D3D12_CreateCuModule failed - nvapi status -1` → `vdlssg: CreateDlssgFeature failed (NGX 0xbad00002)` |
| `1` | 正常出帧 |
| `2`、`3`（虚拟显示器） | `NVSDK_NGX_D3D12_Init failed (NGX 0xbad00001)` |

`Nvapi status -1` 就是代理在 sm_89 cubin 之外没有为这块 device 准备 sm_86 内核时的回答：
`CreateCuModule` 把运行库自带的 Ada cubin 交给 Ampere，CUDA 拒绝。

## 代理为什么只覆写一张卡

`参考插件\dlssg_for_sm86\docs\ARCHITECTURE.md` 里的机制（该插件只有二进制，没有源码）：

- 只钩 CUDA 的三个桩 `CreateCuModule` / `CreateCuFunction` / `LaunchCuKernelChain`；
- 内核变体缓存键是 `(device, 源 RVA)`，所以钩子只服务它识别到的那块设备；
- 安装时 `detectGPU` 按路由挑一张卡（`TARGET_SM75` 找 Turing，其余找 SM86），
  然后才按 `kernelFamily::choose(sm)` 决定用 sm_86 cubin；
- 代理在 `dlssg_sm86\logs\dlssg_sm86.log` 的 `install` 记录里写明 `"selected_luid":"0x…"`，
  `fg_gate_environment` 里也有 `"adapter_luid"` —— 它不藏着这个选择。

它的 INI / 环境变量里**没有**公开旋钮可以指定覆写哪张卡：`FORCE_ROUTE` 只给它的测试夹具用，
`Router`、`ForceSM86Route` 选的是内核族（SM75/SM86/PTX），不是卡。

## 现在的行为

`vs-dlssg\src\ngx_session.cpp` 的 `resolveAdapter`：

1. 脚本给了 `gpu=N` → 直接用 DXGI 序号 N（旧行为，显式优先）；
2. 没给 → 载入代理后读它刚写给本进程 pid 的最后一条 `install` 记录，取 `selected_luid`
   （32 位，即 `LUID::LowPart`），再枚举 DXGI 适配器取 `AdapterLuid.LowPart` 相同的那张；
3. 记录里没有（日志被关掉、换了布局）→ 退化为「接显示器的 NVIDIA 卡 → 任意 NVIDIA 卡 → 适配器 0」。

`log=1` 时会打一行确认：

```text
[vdlssg] adapter 1 follows the proxy: selected LUID 0x11f20, NVIDIA GeForce RTX 3080
```

显式写错卡时报错也会说清楚：`CreateDlssgFeature failed (NGX 0xbad00002): the sm86 proxy patches one
card only, and gpu=0 is not it - drop gpu= to follow the proxy, or try another index`。

## 为什么不能"试错重来"

`NVSDK_NGX_D3D12_Init` 每进程只能一次（`claimNgxSlot` 还会挡住同进程里的第二个 NGX 插件），
初始化之后就绑定了那张 device 的 LUID 闸门，所以选错只能在**进程外**重来。

## 还没做：把覆写本身搬进插件

要让插件对**任意**一张卡都能用 DLSS-G（而不是跟随代理选中的那张），就得自己接 CUDA 三桩和
sm_86 内核包：把运行库的 sm_89 fatbin 按 RVA 映射到 `dlssg_sm86\kernels\fg_kernels.dll` 里的同名
内核，再通过 `cuModuleLoadDataEx` 交给真 NvAPI。代理的内核包 `kernel_pack` 有自己的 manifest
（`fg-kernels 310.9.16 7710da62f151`），但 RVA→kernel 的对照表在它自己的私有工具里，靠公开资料
拿不到，需要正向分析 sm_86 内核包与运行库的 PTX。这是独立课题，见 `docs\vdlssg-engines.md` 的接入口径。
