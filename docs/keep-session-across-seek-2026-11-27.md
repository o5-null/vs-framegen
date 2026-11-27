# 跨 seek 保留补帧 session（2026-11-27）

## 现象

mpv 里按 `frame-back-step` 后退一帧，每次都要重新等 1~2 秒，画面才出来；乱序 seek 同样。

## 根因

mpv 的 seek（含帧步退）会整体销毁 VapourSynth 滤镜：`video/filter/vf_vapoursynth.c`
`vf_vapoursynth_reset()` → `destroy_vs()` → `freeNode()` → `drv_vss_unload()` → `freeScript()`。
重新播放时再 `createScript(NULL)` 建新 core、`evaluateFile()` 重新构建整条 VS 图。

关键一步用 ctypes 直调 `vsscript.dll` 实测（`work/vs-unload-probe/probe3.py`）：

```
before akarin: 0x0
loaded akarin: 0x7ff8953e0000
after  akarin: 0x0          # freeScript() 之后
```

即 **`freeScript()` 会连带 `FreeLibrary()` 掉脚本 LoadPlugin 进来的插件 DLL**，插件里的 static
session（CUDA/D3D12/NGX 上下文、nvVFX effect 与模型）随模块卸载一起消失。所以只改
`Session::acquire/release` 的引用计数保活是无效的。

同一进程内连续 `createScript + evaluate` 的 vpy 求值成本实测只要 ~3 ms（`probe4.py` + `evaltime.vpy`，
k7sfunc 已在 `sys.modules` 里），所以慢的不是脚本重算，而是插件 session 冷启动。

## 做法

1. `vs-dlssg/src/frame_gen.cpp`：`VapourSynthPluginInit2()` 里用
   `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, ...)`
   把插件自己钉在进程里。之后 VSScript 的 `FreeLibrary()` 只减引用，模块与 static 全局存活。
2. `vs-dlssg/src/fg_types.h`：新增 `fg::keepSessionAcrossFilters()`，读环境变量 `VSFG_KEEP`：
   默认保留；`VSFG_KEEP=0` 恢复旧的「引用归零即 delete」行为，便于 A/B。
3. 四个引擎 session 的 `acquire/release`：
   - 形状相同（尺寸、像素布局、引擎/档位、proxy_dir、adapter）→ 复用并 `newStream()`；
   - 形状不同且仍有引用 → 仍然报原来那条「一个进程只支持一种尺寸」的错误；
   - 形状不同且已空闲 → `delete` 重建（等同旧行为），所以换视频尺寸不会出问题；
   - `release` 到 0 时按 `keepSessionAcrossFilters()` 决定保留还是销毁。
   - NGX/FSR4 复用时会刷新 camera / motion / force8Bit 这类每次调用读取的参数。
4. `vfg_session.cpp` 的 `newStream()` 现在会清 `sharedIndex[]`、`packedIndex[]`、`sharedNext`、
   `packedNext`、`batchPair`、`batchA/B`：这些是**按调用方帧号**缓存的面/主机缓冲，新滤镜的帧号
   从 0 重来，不清会命中上一条流的缓存帧。

## 实测

`work/vs-unload-probe/vfg_keep.py`（同进程内先后建两个 FrameGen，模拟一次 seek）：

| | 第一次滤镜 | 第二次滤镜 |
|---|---|---|
| 默认（保留） | 2.009 s | **0.013 s**（157×） |
| `VSFG_KEEP=0` | 1.979 s | 1.868 s（重建） |

输出帧逐帧一致（means 都是 `[0.0, 0.2668, 0.15, 0.5154]`）。

`work/vs-unload-probe/mpv_seek_test.py`（真 mpv + IPC 驱动，`VFG_TIMING=1`）：

```
COLD 1712 ms  (随后的所有 seek / 帧步退 / 帧步进都没有第二次冷启动)
seek 8 absolute        -> session reused (refs 1)，内插 14.8/10.4/12.3/12.1 ms
frame-back-step ×3     -> session reused (refs 1)，内插 22.8/20.8/25.3 ms（首帧）
seek 3.4 absolute      -> session reused，20.8 ms
seek 17 absolute       -> session reused，33.7 ms（首帧）
frame-step ×2          -> 无新滤镜（前进步进本来就不 reset）
```

注意：`frame-step`（前进）不会触发 reset，所以本来就不重建；只有后退/跳转才是 seek。

## 没有缓存的部分（重要）

保留的只是 **引擎 session**（CUDA 上下文、模型、effect、共享面），**不是帧**：

- mpv seek 会销毁整条 VS 图，新滤镜拿到的帧号又从 0 开始，旧帧对象已经没了；
- 因此后退一帧仍要重新生成那一对内插（10~30 ms），而不是从缓存直接返回；
- VFG 内部只缓存「最近一对」的面，跨对的结果不保留。

如果想做到帧步退「零成本」，需要在插件里加一个按源帧号索引的环形结果缓存（1080p RGBS 一帧约
24 MB，加正确性风险：重复帧/切镜/多帧内插的语义都要跟着缓存键走）。目前 10~30 ms 已在可接受范围，
没有做。

## 副作用

- 插件 DLL 一旦加载就不能卸载；session 空闲也占显存（VFG medium 每 lane 约 0.7 GB），
  关闭视频后不会立刻释放，直到进程退出。
- VFG 的模型历史存在 CUDA context 里，理论上跳到新位置的第一对会失配；实测不是问题：
  `work/vs-unload-probe/vfg_seek_equal.py` 用真实视频跑「顺序播放 40 对 → 新建滤镜跳回 pair 150 /
  逐帧后退 / 一下跳回 25 对前」，两次结果 **逐像素完全一致**（max diff 0.0000），
  DLSSG 同样为 0.0000。所以帧步退看到的帧就是播放时那一帧，可以用来做瑕疵复查。
- 调试：`VSFG_KEEP_DEBUG=1` 会打印 `[fg] plugin init, pin ok` 和每次 `[vvfg] session reused/created`。
