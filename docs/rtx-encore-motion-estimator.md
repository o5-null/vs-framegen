# rtx-encore 自研运动矢量估计（nr_motion_estimator::Encore）逆向分析

> 目标：`参考插件\rtx-encore\mod\rtx-encore.dll`（46.8 MB，纯二进制、无 PDB）
> 方法：IDA Pro 9.2 idalib 无头反编译 + 内嵌 DXBC 反汇编（d3dcompiler_47 `D3DDisassemble`）
> 产物：`work\rtx-encore.i64`、`work\dec\*.fixed.c`、`work\dxbc\*.asm`

---

## 0. 一句话结论

Encore 的运动估计是**纯 D3D12 compute 的 10 pass 图**（sh77–sh86）：先把当前帧颜色/深度降成
R16F 亮度金字塔，用**带深度权重的加权 SAD**做由粗到细的块匹配，然后用 **9 项多项式最小二乘**
拟合全局/相机运动并只保留残差，最后做 **25 抽头的联合双边上采样（亮度 + 深度 + 引导）**，
输出 **每 2×2 像素一个 S10.5 定点矢量**（1/32 像素精度）。

它**不做**真正的光流迭代（没有 Horn-Schunck/Lucas-Kanade 的雅可比迭代），也**不是** PatchMatch；
本质是「深度加权的分层块匹配 + 全局相机模型扣除 + 引导滤波上采样 + 静态检测」。
PatchMatch 式传播（sh89）属于同文件里的 **vort（Shader）** 方法，Encore 只是复用了那个子对象。

---

## 1. 三种估计器的工厂分派

`sub_180424380(est, device, w, h, kind, desc3, ...)`：按 `kind` 三选一

| kind | 类 | 实现 |
|---|---|---|
| 0 | `OpticalFlow`（Fast/Quality） | `sub_180456280`，NVIDIA Optical Flow 硬件 |
| 1 | 同上，`quality=true` | 同上 |
| 2 | `Shader` | `sub_18045DAE0` + `Shader::vftable`（vort，5 pass：sh87–sh91） |
| 3 | **`Encore`** | `sub_1804575E0` → `Encore::vftable` |

`Encore` 虚表 `0x1809552C8`（12 槽），每帧入口：

```c
// 槽[8] 0x1804247F0 → 转发
char sub_180457F00(__int64 *a1, __int64 *cmdlist, unsigned a3 /*bufIdx*/,
                   unsigned char a4 /*resetStatic*/, unsigned char a5 /*hasDepth*/);
```

槽[4]/[5] 返回最近 300 帧的 GPU 耗时（ms），用于面板显示。

---

## 2. 对象与配置

### 2.1 估计器对象（792 字节，`sub_180456FA0` 构造）

| 偏移 | 内容 |
|---|---|
| +0 | ID3D12Device* |
| +8 | root signature |
| +16/+24/+32/…/+72 | **8 个 PSO**：sh79 pyramid / sh80 coarse / sh81 seed refine / sh82 / sh83 / sh84 fit / sh85 solve / sh86 apply+encode |
| +80 | 描述符堆 / 分配器 |
| +88 | 描述符步长（`*v277`，用于 UAV 相对推进） |
| +96/+104 | timestamp query heap / readback buffer |
| +112 | 11×8B timestamp 数据区（双 buffer ⇒ +88*n） |
| +120 | 外部 timestamp 对象（可空） |
| +128/+144 | 每个 buffer 的 timestamp 有效标志 |
| +160 | timestamp 使能 |
| +176…+288 | 9 个 pass 的耗时累加器（double） |
| +256/+264 | 帧计数 / 总次数 |
| +272/+280/+288 | 300 帧滑窗累计 / 均值 / 窗内计数 |
| +296[6] / +320[6] | **各级 level 的宽 / 高** |
| +344 | 金字塔总级数 |
| +348 | 拟合循环终点级（最细） |
| +352 | 相机拟合触发级 |
| +356 | 16×16 tile 数 = `ceil(w16/16)*ceil(h16/16)` |
| +360[12] | 每级 estimate UAV 描述符（双 buffer：+48 一组） |
| +456[6] / +504[6] | 每级 estimate 读 SRV / polish 槽 |
| +552[2] | motion vectors（对外输出，2 组） |
| +568 / +576 | propagated / polished 中间结果 |
| +584 | **partials** raw buffer（264 B × tile） |
| +592 | **camera model** raw buffer（128 B） |
| +600 | **flow** raw buffer（flowW × flowH） |
| +608 / +616 / +624 | 输入工作纹理（a6[0..2]） |
| +632 | vort 子对象（sh87–91 管线，可选复用） |
| +640 | 帧奇偶（双 buffer 索引） |

### 2.2 88 字节配置块（`sub_1804578A0`）

```c
+0  = flags            // 2 或 4（==每格像素数，工厂硬编码 2 ⇒ 2×2）
+4  = w ;  +8 = h
+12 = 0
+16 = gridW = ceil(w/flags)      // 矢量格宽
+20 = gridH = ceil(h/flags)
+24 = flowPitch = (4*gridW + 255) & ~255   // 每格 4B（RG16_SINT）
+28 = w ;  +52 = h                // level0
+32/+36/+40/+44/+48 = level1..5 宽
+56/+60/+64/+68/+72 = level1..5 高
+76 = 金字塔级数（min(w,h) >= 48 才加级，最多 6，逐级 ceil(>>1)）
+80 = (flags != 2) + 1
+84 = 拟合级起点 = (flags!=2)+2，且只要该级 w*h > 0x27100(160000) 就继续加深
```

`flags` 只允许 2 / 4（校验 `((a5-2)&0xFFFFFFFD)==0`）；工厂固定传 2 ⇒ 文档里说的
「one vector per 2×2 pixels」。

### 2.3 资源落地（`sub_18045A8B0`，1197 行）

- 逐级：2 张 R16F 亮度 + 1 张深度（调试名 `RTX Encore motion luma level` / `depth level`）+ 每级 estimate。
- 5 个 UAV 描述符段（propagated=10、polished=10、motion=34）、partials/model/flow 三个 raw buffer。
- 描述符堆大小 `48 * 级数 + 144`；**双缓冲**（`v23=0..1`，`v43 = v23^1`）。
- 失败串：`frame too small for Encore motion`、`Encore motion pyramid/estimates/buffers/descriptor heap refused`。

---

## 3. 每帧调度 `sub_180457F00`（1763 行伪代码 / 2357 条指令）

命令列表 vtable 用法：`+112` Dispatch、`+200` SetPipelineState、`+208` SetDescriptorHeaps、
`+224` SetComputeRootSignature、`+248` SetComputeRootDescriptorTable、`+280` SetComputeRoot32BitConstants、
`+424` ResourceBarrier。

### 3.1 pass 表（10 个 pass 名 / 11 组 timestamp）

```
pyramid → coarse → refine above fit → refine fit level → camera fit
        → refine below fit → propagate → refine output → propagate out + polish → filter + encode
```

### 3.2 实际录制顺序

| # | PSO | shader | Dispatch | 关键绑定 / 常量 |
|---|---|---|---|---|
| 1 | +16 | sh79 | 每级 `ceil(w/8)×ceil(h/8)` | CurLuma/CurDepth → OutLuma/OutDepth；Level/LevelSize/ParentSize |
| 2 | +24 | sh80 | `ceil(w/8)×ceil(h/8)` @ 最粗级 | Seed/Cur/Prev/Dep/Model → Out；`Flags = 4*hasDepth \| 3`，`Param0=0.01`，`Param1=4.0`，`Scale=1<<L` |
| 3 | +32 | sh81 | 同上 | 加 `Parent`（上一级 flow 作初值）；`Flags \| 16`（或 `\|24` 在 fit 级） |
| 4 | +56 | sh84 | `ceil(w/16)×ceil(h/16)` | 法方程累加：Partials(UAV raw) + ModelRw |
| 5 | +64 | sh85 | `(1,1,1)` | 9×9 并行消元，回写 model |
| 6 | +40/+48 | sh82/sh83 | `ceil(w/8)×ceil(h/8)` | 精化 / 相机模型扣除后的 refine |
| 7 | +72 | sh86 | `ceil(w/8)×ceil(h/8)` | Est + Depth + Model + Guide → Out + raw Flow |
| — | 可选 | vort 子对象 pass 表 | 每项 `ceil(w/8)` | `sub_18045F630`，14 个 dword 常量/项 |

收尾：`ResolveQueryData` 一次取回 11 组 timestamp（`88*idx` 字节步长），
`*(v9+144) = 有timestamp && 有历史帧`，`*(v9+640) ^= 1` 切换双缓冲，
`a1+36 = 1`（有了历史帧）、`a1+37 = 上一帧是否有效`。

### 3.3 Flags 位语义（跨 sh78–sh86 综合）

| 位 | 含义 |
|---|---|
| bit0–1 | 模式：0 = 无深度，3 = 有深度 |
| bit2 (4) | `hasDepth` 提示（深度权使能） |
| bit3 (8) | 金字塔顶 / 传播阶段 |
| bit4 (16) | refine pass |
| bit5 (24 = 8+16) | **fit level**（使用 Parent seed） |
| bit5 (0x20) | **扣除相机模型**（用 model 预测做残差） |
| bit6 (0x40) | 预留 |

---

## 4. 各 pass 的算法

### sh77 — 预处理（`sub_180415C00` 建的 PSO，5072 B）
`Color/Depth → CurLuma(R16F) / CurDepth(R32F) + Luma8`
常量：`Size, LumaPitch, Encoding, Reversed, HasDepth`；8×8 线程。
`Encoding` 支持 sRGB / PQ 等；`Reversed` 处理反转深度。

### sh78 — 静态检测 + 原始运动（23936 B）
- 3×3 局部亮度均值（×1/9），得到**平均绝对离差** `Σ|luma-mean|/9` 作为纹理度量。
- 静止判定三条并联：`contrast < 0.035294`（≈3/85，近乎平坦）直接判静止；
  `StaticMinContrast` 门槛；连续帧 `PrevStatic` 迟滞（`>0.5` 就锁存）。
- 深度项：`exp(-(Δd / max(d, 0.001))² · 400.748627) + 0.05`
  ⇒ 等价 `exp(-½(Δd/d)²/σ²)`，σ≈**3.5% 相对深度**（3.5% 深度差即衰减到 1/e）。
- 输出：`Motion(float2)`、`StaticNow`、`RawMotion`（把上一帧 S10.5 定点 `bfe 16 / ishr 16 → ×1/32` 还原后乘权）、`Mask`。

### sh79 — 亮度/深度金字塔
2×2 盒式均值 `×0.25`；level≥2 用双线性 `0.25/1.75` 偏移；深度走最近邻 `ld T1[2]`。

### sh80 — coarse 全搜索（84344 B，300 次 `sample_l`）
1. 在 3×3（含 5×5 对角/±2）邻域算**深度加权均值** `L̄ = Σw·L/Σw`，
   权重即上面的深度高斯 + 0.05（无深度时恒 1）。
2. 代价 = `Σ w |L_prev(center+δ) - L̄| / Σ w`（**加权 SAD**）。
3. 搜索：9×9 方格（`Param1 = 4.0` 半径，`udiv 3` 生成菱形顺序），`1e-3` 改进阈值。
4. 输出 `(mv.xy·(1<<L), cost, confidence)`，置信再乘 **5 像素边缘渐隐**
   `clamp(min(x,W-x,y,H-y)+2.5,0,1)·0.2`。

### sh81 — 带父级初值的分层匹配（57552 B）
与 sh80 同代价，额外读 `Parent`（上一级 flow 上采样）做种子，加速收敛。

### sh82 / sh83 — 精化（1748 / 1648 条指令）
同样的 25 抽头加权 SAD；改进阈值 `1e-5`；输出第 4 分量
`accepted ? 1.0 : 0.1`（**失败候选置信打一折**）+ 边缘渐隐；sh83 用 `SeedSize/Grid` 定步长。

### sh84 — 相机拟合：法方程累加（96084 B，8×8 线程 × 每线程 2×2 像素）

```hlsl
// 像素归一化到 tile 中心 [-1,1]
float2 p = (px + 0.5) * Scale - 0.5*Size;  p /= 0.5*Size;
float  w_depth = (Flags & 4) ? min(0.01 / max(d, 0.001), 1.0) : 0;
float2 r = est - model(p);                       // Flags&32 时扣模型
float  robust = 1.0 / (1.0 + dot(r,r)/Scale²);   // Cauchy 权
float  w = w_depth * robust;                     // <= 0 的样本丢弃
```

9 个基函数（`x, y, x², xy, y², xd, yd, …`）累加成 66 个 float：

| 累加器 | 内容 |
|---|---|
| `x0[0..44]` | A 上三角 45 项 `Σ w·φi·φj` |
| `x0[45..53]` / `x0[54..62]` | 右端项 b_x / b_y（各 9 项） |
| `x0[63..65]` | `Σw·x`、`Σw·y`、`Σw` |

8×8 线程经 tgsm（4224 B）归约后 `store_raw U0`（264 B = 66 float / tile）。
法方程还有 `0.005·w·h` 的归一化因子（`v313.f32[2]`）。

### sh85 — 解 9×9 法方程（102956 B）
`dcl_thread_group 64,1,1`、858 `ld_structured` / 462 `store_structured`、7 次 `sync_g_t`：
对对称正定阵做**并行的 Gauss/Cholesky 消元**，结果写回 `ModelRw`：
`offset 0..32 = x 分量 9 个系数，36..68 = y 分量 9 个系数`（`ld_raw` 成对读写）。

### sh86 — 应用模型 + 引导上采样 + 定点编码（21168 B）

1. 从 `Model` raw 读 9 项系数，tile 内求值得到平滑全局流；
   `r0.yz = (tex.z*4.0, tex.z*-9999.25+10000.0)` 把模型置信映射到 0..1 的引导权重。
2. **25 抽头联合滤波**（±2/±1/0 的 (dx,dy)）：

```hlsl
w = exp(-|p-q|²/(2σ²))                       // 空间
  * (exp(-(Δd/max(d,0.001))²·400.748627)+0.001)  // 深度
  * exp(-|ΔL|)                               // 亮度（彩色/遮挡）
  * exp(-|guide_delta|²/σ²);                 // 引导（模型响应）
out = Σw·mv / Σw;
```

引导项系数 `0.135335 / 0.286505 / 0.367879` = `exp(-2)/exp(-1.25)/exp(-1)`（切比雪夫距离分段）。
3. **编码**：`mv*32 → round_ne → clamp(±32768) → ftoi → bfi 16/16` 打包成一个 u32，
写入 `U0[2].x`，索引 `y*FlowPitch + 4*x` ⇒ **S10.5 RG16_SINT（1/32 px，2×2 像素/格）**。

---

## 5. 关键常量速查

| 值 | 位置/用途 |
|---|---|
| `0.03125` | S10.5 定点还原（1/32） |
| `400.748627` | 深度高斯 `exp(-½(Δd/d)²/σ²)`，σ≈3.5% |
| `0.001` / `0.0001` | 深度下限（防除零） |
| `0.01` | 深度权系数 `min(0.01/d,1)`；也是 sh80/91 的 `Param0` |
| `4.0` | sh80 coarse 搜索半径 `Param1` |
| `0.05` / `0.001` | 深度权常数项（sh80 / sh86 不同） |
| `0.035294` | 静止对比度地板（3/85） |
| `1e-3` / `1e-5` | 候选接受阈值（coarse / refine） |
| `0.2`、`2.5` | 边缘渐隐 `clamp(min+border+2.5,0,1)*0.2` |
| `0.005` | 法方程归一化因子（`0.005·w·h`） |
| `0.1` | 失败候选置信折扣 |
| `0x408000003C23D70A` | sh80 的 `(Param0=0.01, Param1=4.0)` 成对常量 |

---

## 6. 数据格式

| 资源 | 格式 |
|---|---|
| 亮度金字塔 | `R16_FLOAT` |
| 深度 | `R32_FLOAT` |
| 运动输出 | `S10.5_RG16_SINT`（每 2×2 px 一格，4 B/格，pitch 256 B 对齐） |
| partials | 264 B/tile（66 float，16×16 tile） |
| camera model | 128 B（9+9 系数 + 头部） |
| 中间 estimate | float4（mv.xy, cost, confidence） |

`sub_180414C50` 可导出调试包：`current-luma.bin`、`previous-luma.bin`、`current-depth.bin`、
`previous-depth.bin`、`flow.bin`、`manifest.txt`（`version=1 / estimator= / width= / height= /
grid= / frame= / hasDepth= / lumaFormat= / depthFormat= / flowFormat= / flowWidth= / flowHeight= / flowPitch=`）。

---

## 7. 对 vdlssg 的可用性

- **格式兼容，但必须按整幅喂**：输出是 `S10.5 RG16_SINT`、2×2 px/格、1/32 px 精度，
  数值上是 DLSS-G 期望的 motion vector。实测两件事（`测试数据\results\report_final.md` §6.1）：
  ① NGX 在 `EvaluateFeature` 硬拒比帧小的 mvec 纹理（半幅 960×540 → `NGX 0xbad00005`
  = `NVSDK_NGX_Result_FAIL_InvalidParameter`，而 `CreateFeature` 通过），所以 2×2 网格要先
  复制展开回整幅再喂（4 倍上传流量）；② 即便如此，画质也不动 —— 按此格式复现后 VMAF 只
  从 58.123 到 58.137（+0.014）。**这份估计器对 vdlssg 的价值不在"喂更好的场"，而在可以
  借用它的静态检测与（若要自己做后处理时的）矢量本身。**
- **不需要深度**：`flags=2` 时深度权恒为 1，退化为纯亮度加权 SAD + 边缘渐隐；
  有深度时只是把匹配权换成深度高斯。这解释了 README 里「无深度也能跑」。
- **最贵的是 sh80**（84344 B，300 采样/像素）与 sh84/sh85（9×9 法方程）。
  纯视频插帧场景没有相机运动，`camera fit` 可以直接删掉，能省约 1/3 的 pass。
- **静态检测（sh78）值得单独借用**：字幕/HUD/静止文字的保护就是那三条对比度判定
  + 上一帧迟滞，实现成本极低。

---

## 8. 未能确认 / 后续

- `sub_18045F630` 复用的 vort 子对象 pass 表（`off_181170650` 两个 8 项条目）各 pass 的
  常量含义未逐项展开；它只在 `+632` 非空且已有历史帧时执行。
- `Flags` bit6 (0x40) 与 `+552` 两组 motion vectors 的使用差异未验证。
- sh92/sh93 是 DXIL（`CSMain256Uniform`），属 NR（神经渲染）侧，与运动估计无关。
