# vs-dlssg 四引擎插件的 VapourSynth API 4 适配

日期：2026-11-21　范围：`E:\code\dlssg-vfi\vs-dlssg\src`（vdlssg / vxessfg / vvfg / vfsr4 同源）

## 一、为什么要改

插件原来按 API 4.0 写死：入口 `configPlugin(..., VAPOURSYNTH_API_VERSION, 0, plugin)` 无条件上报
编译期版本，`createVideoFilter2` 声明**零依赖**（`nullptr, 0`），而 `getFrame` 内部却用
`getFrameFilter` 向 `clip` 与 `mv` 两个节点建帧。API 4 的语义是「滤镜请求的每个节点都必须是它声明的
依赖」，API 4.1 起还能从外部（`getNodeDependency`）检查这张依赖表，4.2 的核会把这张表当作图结构的一
部分。版本写死则意味着：在 4.0 的核上只有恰好声明成 4.0 才注册成功，未来核把 4.0 判弃用时又会被直接
拒载。

## 二、改动

| 文件 | 改动 |
|---|---|
| `src/build.bat` | 四个 target 的公共编译行加 `/DVS_USE_LATEST_API`（头文件 `VapourSynth4.h` 里它把 `VAPOURSYNTH_API_MINOR` 置 2，即按 API 4.2 的结构体编译） |
| `src/frame_gen.cpp` 入口 | `VapourSynthPluginInit2` 读 `vspapi->getAPIVersion()`，用 `min(核 API, 头文件 API)` 去 `configPlugin`——头文件是新的、核可能旧，声明高版本会被 `configPlugin` 拒绝 |
| `src/frame_gen.cpp` 建滤镜 | `const VSFilterDependency deps[] = {{data->node, rpGeneral}, {data->mvNode, rpGeneral}};`，`numDeps = mv ? 2 : 1`，交给 `createVideoFilter2`。`rpGeneral` 是因为一帧要取 `srcA` 与 `srcA+1`，顺序由核调度决定 |
| `src/frame_gen.cpp` 日志 | `log=1` 的那行打头加 `core API %d.%d`（`api>>16`、`api&0xffff`），方便确认实际核版本 |
| `vs-dlssg/README.md` | 构建要求后补一段 API 兼容说明；`scene_threshold` 的说明改成新的亮度语义 |

刻意**没有**用 4.1/4.2 独有的东西：`clearNodeCache` 一组函数头文件里写着「不要在滤镜内部使用」，
`VS_GRAPH_API` 是实验接口且要求核以 `ccfEnableGraphInspection` 建立、主次版本严格匹配。插件的需求只
是「声明正确 + 哪个核都能加载」，多调一组有风险且没有收益。

## 三、顺带修掉的一个既有崩溃

验证 API 4 改动时发现：**YUV 输入 + 内置切镜（`scene_threshold>0`）必崩**（`0xC0000005`），RGBS 输
入不会。原因是早前加 YUV 直通时不涉及这段代码，`isSceneCut` 仍按 RGBS 假设实现：

```cpp
for (int plane = 0; plane < 3; ++plane) {
  const float* pa = reinterpret_cast<const float*>(vsapi->getReadPtr(a, plane));
  const ptrdiff_t sa = vsapi->getStride(a, plane) / sizeof(float);
  for (int y = 0; y < height; y += step) ...   // 用亮度尺寸遍历色度平面
}
```

色度平面是 `subSamplingW/H` 之后的小平面（420 是 1/4），却按亮度宽高和 `float` 步长读，必然越界；
10bit YUV 的样本还是 2 字节整数，当成 `float` 读也完全错。

修法：`isSceneCut` 增加 `const ColorFormat& format` 参数。YUV 分支只读平面 0（亮度），按
`format.bytesPerSample()` 与 `yuvQuant(bits, fullRange).luma()` 归一化到 [0,1]；RGBS 分支保持原来的
三通道浮点逻辑（三个平面都是全尺寸）。两边都是 [0,1] 亮度，所以 `scene_threshold` 的含义不变。

切镜本来也主要看亮度：硬切在亮度上必然出现大跳变，而色度是半采样、按亮度栅格去读只会带来越界而不
是更好的判定。

## 四、验证记录

环境：VapourSynth R77 / API R4.2（`E:\code\VFI\.venv`），1080p sintel（`media\sintel_trailer_1080p.mp4`），
YUV420P10 直喂。

| 检查 | 结果 |
|---|---|
| 四 target 编译 | `cmd /c vs-dlssg\src\build.bat` 全过，只有既有的 `getenv` C4996 与 ngx helpers deprecated |
| 注册版本 | `log=1` 打印 `[vdlssg] core API 4.2, args: ...` |
| 单链（无 mv，默认 scene_threshold） | 2506 帧、fps 48/1，取 0..3 帧正常，`_DLSSGGenerated` 0/1 交替 |
| 单链（带 `mv=std.BlankClip(RGBS)`） | 同上，正常 |
| 两条 FrameGen 同进程 | 无 mv / 带 mv 各两条链都正常退出 |
| 切镜 hold（YUV） | 6 帧暗 + 6 帧亮 `std.Splice`：切点 pair 输出保持暗帧（亮度 64.00），切后为亮帧（899.89） |
| 切镜 hold（RGBS） | 同一素材同样成立（hold 在 0.0，切后 0.8） |
| 四个引擎 | `vxessfg` / `vvfg` / `vfsr4` 在 YUV + `scene_threshold=0.15` 下各取 4 帧正常，`_DLSSGGenerated` 0/1 交替 |
| 依赖表可查性 | Python 侧没有 `num_node_dependencies` / `get_node_dependency`，无法从脚本读回这两条边；依赖是否合法由核在取帧时校验（能取帧即通过） |

脚本留在 `C:\Users\benben\AppData\Local\Temp\opencode\`：`api4_log.py`（`log scene` 两参数取帧）、
`api4_check.py`（两条链 + 核 API 行）、`api4_engines.py`（`<dll> <engine>` 单链 4 帧）、
`api4_cut.py`（YUV / RGBS 切镜 hold，自造 6+6 帧硬切）。

## 五、兼容性结论

- 同一个 DLL 在 API 4.0 / 4.1 / 4.2 的核上都能注册：编译按 4.2 结构体，注册按 `min(核, 头文件)`。
- 不调用任何 4.1/4.2 独有函数，所以低版本核不会因为函数指针越界而崩。
- 核比头文件新时（例如将来 4.3），注册版本停在 4.2，行为不变。
- 头文件比核新时（本机头与核同为 4.2；若换成旧的 `vapoursynth` pip 包，`VAPOURSYNTH_API_VERSION` 会
  退到 4.0/4.1），注册版本随之降低，仍然安全。
