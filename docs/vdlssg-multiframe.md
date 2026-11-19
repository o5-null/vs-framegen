# DLSS-G 多帧生成能不能迁移到视频

结论：**能，`multi=2..6` 已在插件里落地**（2–4× 有完整质量/吞吐数据，6× 需随包
`vs-dlssg\bin\dlss45`，见 §6）；但单帧质量随 `multi` 增大而下降，且插值时刻被锁死在等分点上
（1/multi、2/multi…），不能像 RIFE 那样指定任意 timestep。

## 1. 参数语义（SDK 层）

`ngxsdk\include\nvsdk_ngx_params_dlssg.h:41-48`：

```c
unsigned int multiFrameCount = 1;   // 1 = 2x, 2 = 3x, ...
unsigned int multiFrameIndex = 1;   // 从 1 起，到 multiFrameCount（含）
```

`multiFrameCount = m` 表示把 (prev, cur) 区间等分成 `m+1` 段，`multiFrameIndex = i` 取第
`i` 个等分点（`i/m+1`）。`NVSDK_NGX_D3D12_DLSSG_Eval_Params` 只有一个 `pOutputInterpFrame`，
所以**一次 Evaluate 只产出一张中间帧**，要 m 张就得 Evaluate m 次。

本机 stock 代理实测 `DLSSG.MultiFrameCountMax = 3`（即最多 4×）。插件的 `multi` 参数范围
是 2..6，运行时上限在读该参数后二次校验，`multi-1 > multiFrameCountMax` 时直接报错，不会
静默降级。**6× 只有换 `MaxGeneratedFrames=5` 的引擎包才成立**（`vs-dlssg\bin\dlss45`，同一份
310.9.16 运行库，只改 ini），详见 `docs\vdlssg45-engine.md`。

## 2. 一个必须知道的运行库行为：抑制标志

`pOutputDisableInterpolation` 读回的值（插件里叫 `suppressed`）：

- **单独求值某个 index**（reset 后只 Evaluate 一次）：恒为 0，无论 index 是 1 还是 3。
- **同一 reset 后连续求值多个 index**：只有第一个拿到 0，其余全部被置 1。

这个 1 只是"这一帧别拿去呈现"的提示值，**不代表纹理没写**。探针验证：把被抑制的 index 2
的输出读回来，它和单独求值 index 2 的结果**逐位相同**（见下节 `--only 3` 实验）。

所以插件在 `multi > 2` 时忽略该标志（每个 slot 都照常读回），`multi == 2` 时仍按抑制处理
（这也是 2× 路径早先就在用的行为）。代价是：这张"提示别呈现"的帧在游戏里永远不会被看到，
因此**没有厂商对它的画质背书** —— 实测也确实比 2× 差，见第 4 节。

## 3. 位置语义验证（`spike\multiframe.exe`）

探针把 k 张连续真实帧喂进去，用 `pair=(f0, f_{k-1})`、`multiFrameCount = k-2`，
这样第 i 个 index 的时间位置恰好落在真实帧 `f_i` 上，可以算精确 GT。

- 素材：`media\seq\tNN_f0..f4.rgb`（10 组，md5 与既有 `frames\tNN_{a,gt,b}.rgb` 三元组一致）。
- `--len 5 --ignoresuppress` 升序全量：`verify_mf5.py` 核对 t01/t05/t06 均 i1→f1、i2→f2、i3→f3。
- **决定性实验**：`--len 5 --only 3 --ignoresuppress` → index 3 单独求值 20 次全部 `suppressed=0`，
  gen 均值 **0.01069**，逐组数值（t00 0.03740、t02 0.01057、t05 0.01495、t06 0.01502、
  t07 0.00371、t08 0.00024、t09 0.00002）与升序全量里 index 3 的数值**完全相同**。

→ 位置由 `multiFrameIndex` 单独决定，与求值顺序无关。中途一次 `--reverse` 得出的
"顺序决定内容"结论作废（降序循环的计数逻辑有缺陷，且与 `--only` 结果矛盾）。

## 4. 质量（Sintel 1080p，`test\parity3.py`）

做法：取窗口内全部真实帧作参考，`SelectEvery(multi,0)` 抽稀后喂插件，再逐 slot 对真实帧算 MAE。

| multi | 组数 | slot1 (α) | slot2 (α) | slot3 (α) | 全 slot | 线性 blend | 相对 blend |
|---|---|---|---|---|---|---|---|
| 2 | 16 | 0.006143 (.50) | — | — | 0.006143 | 0.013062 | **2.13×** |
| 3 | 8 | 0.009510 (.33) | 0.009716 (.67) | — | 0.009613 | 0.016043 | **1.67×** |
| 4 | 6 | 0.010965 (.25) | 0.012024 (.50) | 0.012027 (.75) | 0.011672 | 0.017351 | **1.49×** |

- 各 slot 互差（互异性）：multi=3 mean 0.012990、multi=4 mean 0.013437，`min=0.000000`（静止段）。
  非零即证明各 slot 确实是**不同画面**，参数没有被忽略。
- 4× 的单帧 MAE 约为 2× 的 **1.9 倍**。仍远好于 blend，但作为"插帧质量"已经明显不如 2×。

## 5. 吞吐（`test\bench_multi.py`，best of 3）

| 分辨率 | multi | 输出 fps | 源帧/秒 | evaluate 均值 |
|---|---|---|---|---|
| 2160p | 2 | 51.61 | 25.80 | 2.97 ms |
| 2160p | 3 | 56.50 | 18.83 | 2.71 ms |
| 2160p | 4 | 62.15 | 15.54 | 2.43 ms |
| 1080p | 2 | 130.18 | 65.09 | 1.35 ms |
| 1080p | 3 | 164.11 | 54.70 | 0.99 ms |
| 1080p | 4 | 176.70 | 44.17 | 0.87 ms |

两个方向相反的量：

- **输出 fps 随 multi 上升**：第 2..m 个 slot 复用同一对颜色纹理（`samePair` 路径），
  只多花 0.6–0.9 ms（4K ~2.4 ms），reset 趟和上传不必重做。
- **源帧吞吐随 multi 下降**：每组仍要付一次 reset + 上传，摊到更多输出帧上显得便宜，
  但换回"每秒能吃多少源帧"就少了。

实际选择看源帧率：24 fps 素材做 3× 需要 72 fps 输出（1080p 满足、4K 不满足）；
做 4× 需要 96 fps 输出（1080p 勉强、4K 不行）。

## 6. 6× 与 `vs-dlssg\bin\dlss45`

`MaxGeneratedFrames` 是 `dlssg_sm86` 代理 ini 里的开关（不是运行库版本差异）：

| 值 | MultiFrameCountMax | 最高倍率 | 文件 |
|---|---|---|---|
| 3 | 3 | 4× | stock `dist\dlssg_sm86.ini`、`work\dlssg_sm86.ini` |
| 5 | 5 | **6×** | `vs-dlssg\bin\dlss45\dlssg_sm86.ini`（从 SmoothMyVideo 提取） |

- 运行库本体逐字节相同（`nvngx_dlssg.dll` 310.9.16，SHA256
  `CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72`），
  SmoothMyVideo 只是把 ini 提到 5，所以这里的"提取 4.5 引擎"= 6× 上限 + 自包含目录。
- 插件 `multi=6` → `multiFrameCount=5`、`multiFrameIndex=1..5`，与引擎自报上限一致。
- 已验证：`vs-dlssg\bin\dlss45\smoke.py 640 360 5 6` 走 SMV 的 `dlssg2f.exe --server`，
  handshake `gen=5 max=5`，25 帧 `(6-1)*5`，质心误差 ≤2 px（子像素取整）。
- 未验证：6× 的 MAE/VMAF 与吞吐（`parity3.py` / `bench_multi.py` 目前只测到 4×）。
  按 §4 的趋势（4× 已是 2× 的 1.9 倍 MAE），6× 每张中间帧质量预计继续下降；
  SmoothMyVideo 侧同引擎实测 1080p 6× 约 195 fps 输出（`docs\smv-dlssg-unlock.md`），
  但那是它自己的宿主 + 8bit 路径，不能直接当 vdlssg 的浮点路径数据。

## 7. 结论

- `multi=3/4` 在架构上完全可行，插件已实现并端到端跑通；`multi=5/6` 走 `vs-dlssg\bin\dlss45`
  引擎包也同样成立（上限由 ini 决定），只是还没有专门的质量/吞吐测量。
- 质量与吞吐两边都不占优：**2× 是画质最优点**，多帧只在"必须把 24 fps 素材硬拉到 60/120 fps"
  这类场景才有意义，且要接受每帧质量下降约 0.6–1.9 倍 MAE。
- 插值时刻固定在等分点，做不了 RIFE 那种任意 timestep；这是模型语义决定的，不是插件限制。
- 运行库的抑制标志在多 index 场景下不可信（只是"别呈现"提示），插件已按此处理。

## 8. 复现

```
cd E:\code\dlssg-vfi
cmd /c "spike\run_mf.bat --len 5 --only 3 --ignoresuppress > work\mf_only3.log 2>&1"
cmd /c "E:\code\VFI\.venv\Scripts\python.exe test\parity3.py 3 300 8"
cmd /c "E:\code\VFI\.venv\Scripts\python.exe test\bench_multi.py 4k"
cmd /c "E:\code\VFI\.venv\Scripts\python.exe test\bench_multi.py 1080p"
```

`test\perf3.py` 从环境变量 `VDLSSG_MULTI` / `VDLSSG_SIZE` 取配置，因为 vspipe 不传 argv。
