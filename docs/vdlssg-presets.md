# DLSS-G 有没有"预设"，以及还有哪些能调的开关

结论先说：**DLSS-G（帧生成）没有任何 quality preset**。游戏里能选的"质量档"
（Quality / Balanced / Performance / Ultra Performance）属于 DLSS 超分（SR）与
Ray Reconstruction（RR），它们才有 `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_*`
（preset A–F / J / K）。帧生成是固定算法，质量档位在 `nvngx_dlssg` 内部，API 不暴露。

因此"把不同预设迁移到视频插件"这件事对 FG **没有对应物**。能迁的只有一批行为开关，
其中真正会改变画面的我们逐个实测过。

> **2026-11-23 补充**：上面这条只对**官方 NGX API** 成立。本项目实际走的 DLSS 4.5 引擎包
> （`bin\dlss45`）里，sm86 代理自己实现了 `[DLSSG] Preset`（A/B/C），它不是 NGX 参数、也不走
> `params->Set`，而是代理装载时从 `dlssg_sm86.ini` 读入并钉死在进程上。已实测三档差异并做成
> 插件参数 `preset="a"/"c"`，详见 §6。

## 1. 取证：API 里确实没有 preset

```bash
findstr /I /C:preset /C:quality /C:mode E:\code\dlssg-vfi\ngxsdk\include\nvsdk_ngx_defs_dlssg.h
```

`E:\code\dlssg-vfi\ngxsdk\include\nvsdk_ngx_defs_dlssg.h` 全文 400 余行，上述检索只有三处命中：

| 命中 | 实际含义 | 是否与画质有关 |
|---|---|---|
| `// Optional Input Textures to improve Image Quality` | 注释，讲 hudless/depth/mvec | 否 |
| `DLSSG.FullscreenMode`（`Windowed=0/Borderless=1/Exclusive=2`） | 全屏模式，给 Streamline 判断呈现方式 | 否 |
| `NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset` | Automode 是否覆写 reset 标志 | 否（只影响历史重置） |

对照 `ngxsdk\include\nvsdk_ngx_defs.h` 里的 SR/RR 项，`DLSS_Hint_Render_Preset_A..F/J/K`
全部以 `DLSS.` 前缀存在，DLSSG 一项都没有。

另一条证据在创建结构体：

```cpp
// E:\code\dlssg-vfi\ngxsdk\include\nvsdk_ngx_params_dlssg.h:31-39
typedef struct NVSDK_NGX_DLSSG_Create_Params
{
    unsigned int Width;
    unsigned int Height;
    DXGI_FORMAT NativeBackbufferFormat;
    unsigned int RenderWidth;
    unsigned int RenderHeight;
    uint32_t DynamicResolutionScaling;
} NVSDK_NGX_DLSSG_Create_Params;
```

只有六个字段，**没有任何质量/预设字段**。想在创建期改算法行为，只能走
`params->Set("<name>", ...)` 这条旁路。

## 2. 可迁移开关清单

行号取自 `E:\code\dlssg-vfi\ngxsdk\include\nvsdk_ngx_defs_dlssg.h`。

| 参数 | 行 | 时机 | 语义 | 对视频插帧是否有意义 |
|---|---|---|---|---|
| `DLSSG.LiteCandidateBlendingEnabled` | 376 | **创建期** | "the algorithm will use a lighter blending mode for generating the final output" | 唯一能改混合算法的开关 → 已实测，**无效**（见 §3） |
| `DLSSG.MenuDetectionEnabled` | 216 | 求值期 | 在菜单/静态叠加层上抑制生成 | 视频里没有 HUD 语义 → 已实测，**无效**（见 §3） |
| `DLSSG.MultiFrameCount` | 357 | 求值期 | "Total of Number Frames expected to Interpolate for given Real-Frame Pair. For 4x, this is 3. For 2x, this is 1." | **已迁移**，插件 `multi=2/3/4` |
| `DLSSG.MultiFrameIndex` | 359 | 求值期 | "Current inner frame index. For 4x, this is 1, 2, and 3. For 2x, this is 1." | **已迁移**，决定插值时刻（等分点） |
| `DLSSG.BackbufferFrameID` | 364 | 求值期 | 每个完整渲染的 backbuffer 帧 +1 | **已迁移**，插件每次 Evaluate 递增 |
| `DLSSG.DepthInverted` | 186 | 求值期 | 深度方向 | 不需要（纹理必需但内容可常量：0.05/0.5/0.95 扫描逐位相同） |
| `DLSSG.InvertXAxis` / `InvertYAxis` | 207 / 208 | 求值期 | 运动矢量轴翻转 | 不需要（零矢量；真实块匹配矢量 max 只差 8/255） |
| `DLSSG.MvecDilated` / `MvecJittered` | 213 / 214 | 求值期 | 矢量已膨胀 / 已抖动 | 不需要（同上） |
| `DLSSG.MinRelativeLinearDepthObjectSeparation` | 250 | 求值期 | 线性深度下前景后景最小分离 | 没有真实深度，无从设置 |
| `DLSSG.UserInterfaceRecompositionEnabled` | 370 | 创建期 | UI 重合成模式 | 视频没有 UI 层 |

线性深度换算（同文件 `:223-224`）：`DepthInverted=false → lin_depth = 1/(1-depth)`；
`DepthInverted=true → lin_depth = 1/depth`。

## 3. 实测：两个 create 期开关都无效

`E:\code\dlssg-vfi\spike\main.cpp` 加了 `--liteblend` / `--menudetect` 两条，
设置在 `params->Set(Width/Height)` 之后、`NGX_D3D12_CREATE_DLSSG` 之前：

```cpp
if (opt.liteBlend) {
  params->Set("DLSSG.LiteCandidateBlendingEnabled", 1);
  std::printf("set DLSSG.LiteCandidateBlendingEnabled = 1\n");
}
if (opt.menuDetect) {
  params->Set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 1);
  std::printf("set DLSSG.MenuDetectionEnabled = 1\n");
}
```

**判据注意**：`NVSDK_NGX_Parameter::Set` 全部 8 个重载返回 `void`
（`ngxsdk\include\nvsdk_ngx_params.h:55-62`），所以参数名写错、被运行库忽略，
都不会报错——只能靠输出差异判断。

### 结果

| 运行 | 参数 | gen-vs-GT mean | blend 基线 | 优于 blend |
|---|---|---|---|---|
| `work\lite\base.log` | 无 | 0.00416 | 0.00708 | 1.70× |
| `work\lite\lite.log` | `LiteCandidateBlendingEnabled=1` | 0.00416 | 0.00708 | 1.70× |
| `work\lite\menu.log` | `MenuDetectionEnabled=1` | 0.00416 | 0.00708 | 1.70× |

逐组数字也完全一致（t00 0.00439 / t01 0.00366 / t02 0.00524 / t04 0.01314 / t06 0.00884 …）。

逐字节比较（`test\compare_ppm.py`，把 `work\out_base` 与 `work\out_lite` 的 30 个 ppm 全做 md5）：

```
---- 30/30 bit-identical ----
```

生成帧、blend 基线、真值帧三类全部逐位相同。

### 解读

- `LiteCandidateBlendingEnabled` 名为"更轻的混合模式"，实测**一个字节都不变**。
  两种可能：该参数在 310.9.16 运行库里已被忽略；或者它只对候选帧（lite candidate）
  路径生效，而单帧插值不走那条路径。无论哪种，对视频插帧都没有可用价值。
- `MenuDetectionEnabled` 需要真正的菜单/静态叠加层特征才触发，Sintel 素材没有，
  所以"无变化"符合预期——但这不构成"有效"的证据，只是"在视频场景没有触发条件"。
  视频里等价的需求是**切镜检测**，而实测运行库本身不做（`--cut` 下 `suppressed=0`），
  所以这件事仍由插件自己做（两帧梯度方向直方图距离 > `scene_threshold`）。

## 4. 对视频插帧的实际可调项

既然没有 preset，插件侧真正能调的只剩这些（都不来自运行库）：

| 插件参数 | 作用 |
|---|---|
| `multi` = 2 / 3 / 4 | 每对源帧生成 1 / 2 / 3 张中间帧（走 `MultiFrameCount`/`MultiFrameIndex`） |
| `scene_threshold` = 0.12（缺省） | 切镜判定灵敏度：两帧梯度方向直方图（HOG）的 L1/2 距离，超过即吐源帧（`_DLSSGFallback=1`）。与亮度无关，闪光不误判 |
| `duplicate_threshold` = 0.005（缺省） | 重复帧判定：按 32px 块比较，块内亮度最大差低于它就复制前一帧（`_DLSSGFallback=4`）。0 关闭 |
| `gpu` | 选哪张显卡；不写时跟随 sm86 代理选中的那张（多显卡机器别再写死 0） |
| `preset` = `a` / `c`（缺省沿用引擎包） | DLSS 4.5 引擎包的模型预设，见 §6 |

质量本身没有档位可调。想更好只能换算法（RIFE/FILM 等），想更快只能降 `multi`。

## 5. 复现

```bat
rem 无开关基线
cmd /c "copy /y E:\code\dlssg-vfi\spike\spike.exe E:\code\dlssg-vfi\work\ >nul & cd /d E:\code\dlssg-vfi\work & spike.exe --triple E:\code\dlssg-vfi\media\frames --out E:\code\dlssg-vfi\work\out_base --iterations 1 > E:\code\dlssg-vfi\work\lite\base.log 2>&1"

rem LiteCandidateBlending
cmd /c "cd /d E:\code\dlssg-vfi\work & spike.exe --triple E:\code\dlssg-vfi\media\frames --out E:\code\dlssg-vfi\work\out_lite --iterations 1 --liteblend > E:\code\dlssg-vfi\work\lite\lite.log 2>&1"

rem 逐字节比对
cmd /c E:\code\VFI\.venv\Scripts\python.exe E:\code\dlssg-vfi\test\compare_ppm.py
```

运行目录必须是含 `version.dll` 的 `work\`（代理要被显式拉起），否则
NGX 链路起不来，见 `docs\vdlssg-on-3080-spike.md` 的三条前提。

## 6. 引擎包的 `[DLSSG] Preset`（A / B / C）

官方 API 没有预设，但本项目用的 DLSS 4.5 引擎包在**代理层**加了一个：`bin\dlss45\dlssg_sm86.ini`
的

```ini
[DLSSG]
Optimized=1
Preset=A
```

`version.dll` 代理在 install 时读它，把 `preset_id` 钉死在整个进程上（日志
`{"event":"preset_pinned","requested":"A","preset_id":1,...}`，`hooks.preset_select=0x344f0`）。
NGX 每进程只 init 一次，后端 `sm86_backend.dll` 也没有 `ConfigurePreset` 之类的导出，所以运行期
改不了，只能在装载代理之前改文件。

| preset | 包内原文 | preset_id | 语义 |
|---|---|---|---|
| `A` | UI recomposition off, FG Lite off | 1 | 历史模型 |
| `B` | UI recomposition on | 2 | 插帧用无 UI 画面，再把 UI 平面贴回去，让 HUD/字幕保持清晰 |
| `C` | FG Lite | 3 | 每生成帧 GPU 时间约少 32–54%（文档说法），画质略低，仍走本项目的加速内核 |

`Auto` 已于 2026-10-22 删除（按 `A` 处理）；旧的 `[Compatibility] Preset` 不再读取。

### 实测（RTX 3080、1080p、multi=6、sintel 90..113 源帧、各两轮）

| preset | 代理日志 | wall ms/输出帧 | GPU mean | GPU p50 |
|---|---|---|---|---|
| `A` | `preset_id:1, effective:0, overrode_runtime:false` | 6.44 / 6.26 | 3.91 / 3.86 | 3.05 / 3.11 |
| `B` | `preset_id:2, effective:1` + `preset_ineffective` | 6.37 | 3.97 | 3.06 |
| `C` | `preset_id:3, lite_effective:1, overrode_runtime:true` | 6.04 / 6.06 | 3.57 / 3.53 | 2.66 / 2.66 |

`C` 相对 `A`：wall −4.5%、GPU 均值 −8%、p50 −13%。

画质（同源同图，只改 INI）：静态段（帧 2/5/8/11/14/17）`C` 与 `A` **逐像素完全相同**；运动段
PSNR 45.64–47.95 dB、max|Δ| 0.14–0.18（线性 0..1）、约 68% 像素差 >1e-4，差异集中在 HUD 类硬边缘。

### B 为什么等于 A

UI 重组要运行库同时拿到三件东西（`ngxsdk\include\nvsdk_ngx_helpers_dlssg_d3d.h:32-44`）：

| 输入 | 含义 | 插件实际给的 |
|---|---|---|
| `pHudless` | 不含 UI 的画面 | 与后缓冲同一张纹理（`src\ngx_session.cpp` 的 `eval.pBackbuffer = eval.pHudless = gpu->colorTexture()`），UI 已烧进去 |
| `pUI` / `pUIAlpha` | UI 颜色层与覆盖度 | 从未赋值（空指针） |

代理因此在 install 后立刻判：

```json
{"event":"preset_ineffective","hudless":true,"ui":false,"missing":["DLSSG.UI"],
 "reason":"preset B (UI recomposition) needs the game to hand DLSS-G both a HUD-less image and a UI plane; it does not, and the pin cannot supply them",
 "effect":"the runtime's own behaviour applies: generated frames are the same as without Preset=B"}
```

`hudless:true` 只说明指针非空。成片视频里也没有可分离的 UI 层——真要用 B 得新增"无 UI 画面 +
UI 颜色 + UI 遮罩"三路输入，那是新的输入设计，不是预设开关。

### 插件侧

`preset="a"` / `preset="c"`（`core.vdlssg.FrameGen`，其它引擎传了会报错）。实现：

- `src\frame_gen.cpp` 解析参数（`b` 直接报错并解释原因）。
- `src\ngx_session.cpp` 在 `Impl::init` 里、`loadProxy()` 之前把 `[DLSSG] Preset` 改写成 `A`/`C`
  （只替换该值，写临时文件再 `MoveFileExW` 换入），并用命名互斥 `Local\vs-dlssg-preset-ini`
  把"改写 + 装载 + 建 feature"串起来，避免两个进程互相覆盖。
- 同一进程里第二个 `FrameGen` 换预设会报错；换预设要另起进程。
- 改写落在**引擎包文件**上：跑过 `preset="c"` 之后包内 INI 就停在 `C`，恢复默认用 `preset="a"`。

细节与文档见 `vs-dlssg\docs\vdlssg-plugin.md` 的"模型预设 `preset`"一节。
