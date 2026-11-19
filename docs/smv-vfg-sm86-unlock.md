# VFG 在 RTX 30（sm_86）上解锁 — 实测跑通

**结论：** SmoothMyVideo 的 NVIDIA Video Frame Generation（`--nvvfg`）在 RTX 3080 上已经能出帧。
做法是一个 166 KB 的 `winmm.dll` 代理，把两处硬件门骗过去，模型文件一行没改。

实测（`测试数据\test.mp4`，1920x1080，4×）：

```
native: vfg session 1920x1080, NVIDIA Video Frame Generation (highest quality mode,
        every pair interpolated), 8-bit RGBA, loaded in 683 ms
offline: 10 frames in, 37 out, 9 pairs, 0.55 s, 60.94 ms per pair
native: vfg session: 27 tweens
```

10-bit 也一样（`10-bit R10G10B10A2` 路线，rgb48le 进 rgb48le 出）。

---

## 1. 原来的门槛是什么

`engine\nvvfx\nvVFXVideoFrameGeneration.dll`（207,321,712 B，来自 NVIDIA `nvidia-vfx 0.2.0.0` 的 435 MB wheel）
里 124 个内嵌 cubin 只有两种 `e_flags`：`0x00590559`（sm_89 / Ada）和 `0x06007802`（sm_120 / Blackwell），
外加 62 处 NVRTC 的 `-arch sm_120` 编译选项，没有 PTX。sm_86 上 `NvVFX_Load` 于是返回 **-1999**
（`NvCV_GetErrorStringFromCode` 把它映射成 `unknown error`），对应 DLL 里的字符串
`GPU SM %d is not supported by this Video Frame Generation build`。

用户日志里就是这一行：

```
[native] native: vfg: the model's load failed: unknown error (status -1999)
```

## 2. 解锁手法（照 `Xchen991/RTX30-Broadcast-FrameGen` 的思路）

模型 DLL 自己在加载内核前问 CUDA 两件事，代理在 IAT 上换掉这两个函数：

| hook | 做什么 |
| --- | --- |
| `cuDeviceGetAttribute` | `attrib == 76`（`COMPUTE_CAPABILITY_MINOR`，日志里是 75 那次调用带出 major=8）且值 6 → 改成 9，把 8.6 报成 8.9 |
| `cuModuleLoadData` | 拿到 fatbin 镜像后线性扫：`ELF` 头的 `e_flags` 低字节 89 → 整字改成 `0x00560556`；fatbin 条目里的 sm 字段 89 → 86；再交给真函数 |

两个 hook 都在 `nvVFXVideoFrameGeneration.dll` 自己的导入表里（它导入 `nvcuda.dll` 34 个符号，
没有 NVRTC / cuLink*，所以不需要碰编译期），按模块名 + 符号名替换槽位即可，不用改文件、不用反汇编偏移。

和 Broadcast 那件的差别：Broadcast 还要改 `NvBroadcastCore.dll` 的 UI 开关（它把能力藏在界面后面），
SmoothMyVideo 的宿主只按"四个文件在不在"判断（`nvvfg-ready` IPC 只查文件名），所以**不需要改 app**。

## 3. 怎么注入

`engine\live\smv-live.exe` 导入 `WINMM.dll`（不导入 version.dll），而 `winmm` 不在 `KnownDLLs` 里，
所以把代理命名成 `winmm.dll` 放在 exe 同目录，加载器会优先取它。代理把系统 winmm 的 180 个导出
全部转发到 `C:\Windows\System32\winmm.*`，行为与系统 DLL 一致。

装载通知用 `ntdll!LdrRegisterDllNotification`：看到 `nvVFXVideoFrameGeneration.dll` 一进进程就立刻
patch 它的 IAT（此时它的导入还没被解析完，正好来得及），另有 2 ms 轮询兜底。

## 4. 安装 / 卸载

```bat
dist\vfg-sm86-unlock\install.bat    :: 默认装到 参考插件\SmoothMyVideo-protable\resources
dist\vfg-sm86-unlock\uninstall.bat  :: 删掉 winmm.dll 即完全还原
```

`install.bat [app-root]` 可以指定别的 app 根（该目录下要有 `engine\live\smv-live.exe`）。
源码树 `参考插件\SmoothMyVideo` 也已经装好（那份还要额外补 `engine\gpu_runtime\` 和 `engine\nvvfx\`）。

代理自己的日志：`engine\live\vfg-unlock.log`（每次 `patchIat`、hook 命中都会记一行）。

## 5. 重新编译

```bat
dist\vfg-sm86-unlock\src\build.bat
```

只需 `cl.exe`（本机在 `C:\software\msvc`，`vcvars64.bat` 自带）。产物 `bin\winmm.dll`。
`src\gen_exports.py` 现解析系统 winmm.dll 的导出表生成 `exports.inc`（180 条转发）。

## 6. 验证与回归

```bat
python dist\vfg-sm86-unlock\test_vfg.py --w 1920 --h 1080 --frames 10 --multi 4 --ss 3
```

它用 ffmpeg 从 `测试数据\test.mp4` 解帧喂宿主，打印出帧数、`native: vfg` 那几行和代理日志。
`--backend=--no-interp` 可以只验证代理本身不破坏宿主。

## 7. 已知边界

- **只在 8-bit / 10-bit 的 on-grid 路线上验证过**；HDR（`SMV_VFG_HDR10`）没测。
- **性能**：1080p 每对 61 ms（4× 即每对 3 次 tween，约 20 ms/tween），25 fps 素材的 4× 实时率约 0.65×。
  想要更快只能用 `Mode=medium`（`NVVFXVIDEOFRAMEGENERATION_MODE`），当前宿主写死 high。
- **驱动敏感**：驱动必须 ≥ 617.42（插件作者的门槛，我们没在更老驱动上试）。
- 这是"骗过检查"的做法：内核仍是 Ada 的 SASS。目前所有 kernel 在 3080 上都能执行且结果正常，
  但如果哪天 `Load` 之后出现 `invalid device function` / 图像损坏，就是某个内核用了 Ada 专属指令，
  那时只能退回其它后端。
- 4K 4× 在 3080 10 GB 上可能显存吃紧，没测。

## 8. 相关文件

| 路径 | 作用 |
| --- | --- |
| `dist\vfg-sm86-unlock\src\winmm_proxy.cpp` | 代理本体（转发 + 两个 hook + 装载通知） |
| `dist\vfg-sm86-unlock\src\gen_exports.py` | 生成 180 条导出转发 |
| `dist\vfg-sm86-unlock\src\build.bat` | 编译 |
| `dist\vfg-sm86-unlock\test_vfg.py` | 离线链路验证 |
| `dist\vfg-sm86-unlock\install.bat` / `uninstall.bat` | 部署 / 还原 |
| `docs\smv-vfg-sm86-blocked.md` | 之前的定性分析（为什么原版 -1999） |
