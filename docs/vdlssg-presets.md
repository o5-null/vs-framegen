# DLSS-G 有没有"预设"，以及还有哪些能调的开关

结论先说：**DLSS-G（帧生成）没有任何 quality preset**。游戏里能选的"质量档"
（Quality / Balanced / Performance / Ultra Performance）属于 DLSS 超分（SR）与
Ray Reconstruction（RR），它们才有 `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_*`
（preset A–F / J / K）。帧生成是固定算法，质量档位在 `nvngx_dlssg` 内部，API 不暴露。

因此"把不同预设迁移到视频插件"这件事对 FG **没有对应物**。能迁的只有一批行为开关，
其中真正会改变画面的我们逐个实测过。

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
  所以这件事仍由插件自己做（8 像素采样 luma 平均绝对差 > `scene_threshold`）。

## 4. 对视频插帧的实际可调项

既然没有 preset，插件侧真正能调的只剩这些（都不来自运行库）：

| 插件参数 | 作用 |
|---|---|
| `multi` = 2 / 3 / 4 | 每对源帧生成 1 / 2 / 3 张中间帧（走 `MultiFrameCount`/`MultiFrameIndex`） |
| `scene_threshold` = 0.15（缺省） | 切镜判定灵敏度，超过即吐源帧（`_DLSSGFallback=1`） |
| `gpu` | 选哪张显卡；不写时跟随 sm86 代理选中的那张（多显卡机器别再写死 0） |

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
