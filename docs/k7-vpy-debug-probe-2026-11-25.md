# 2026-11-25 K7sfunc 补帧调试埋点（`[vpy-debug]`）

## 背景

同日 `docs/mpv-filter-bench.md` 记录：同一份 vdlssg 引擎，VS 路径（`MEMC_DLSSG_NV.vpy`
→ `FRAMEGEN_VPY`）比原生 `vf_playkit` 快 2–3 倍（1080p 2×：149.7 fps vs 51.2 fps）。
插件侧逐帧日志（`[FG_NS]`）显示两种调用下引擎内部耗时一样（`process` 5–8 ms/对、
`copy` <0.7 ms），于是怀疑差在**调度**：原生 `vf_playkit` 每对帧一次阻塞往返（单组
`in_tex`/`out_tex`，`pk_wait` 自旋等 D3D11 拷贝退休），而 VS 侧可能在多个线程上并发推进
多帧，用别的帧的干活盖住了 GPU 等待。

这个怀疑此前只是从跑分数字反推的，VS 内部没有任何打点。本次给 k7 的补帧宿主层加埋点，
让"VS 侧到底有没有并发、每帧实际在等多久"变成可观测的东西。

## 改动

### 新增 `k7sfunc/src/k7sfunc/_vpy_debug.py`

只在 `log` 打开时被 `FRAMEGEN_VPY()` 调用；正常播放（`log=0`）本模块一行都不执行，
补帧图也不会多套节点。

| 组件 | 作用 |
| --- | --- |
| `dbg(message)` | 一行 stderr 输出，带 `[vpy-debug]` 前缀并 flush |
| `Stages` | 构图阶段计时：`mark(name)` 记下距上一标记的毫秒数，`report()` 拼成一行并带 `total` |
| `_Timeline` | 逐帧时间线：按帧号把"请求进入"与"帧就绪"配对，算单帧延迟、帧间隔、同飞帧数、线程构成，维护 p50/p90 与最大在飞 |
| `attach_frame_probe(core, node, tag)` | 在节点外**套两层 `std.FrameEval`**：内层不带 `prop_src`（帧请求刚进来时触发），外层把内层当 `prop_src`（帧就绪后触发）。两者之差就是这一帧在 VS 内部的实际延迟 |

细节：

- 两层回调都直接 `return node`，不经 Python 碰像素、不拷贝帧；帧号→节点映射由
  `FrameEval` 缓存，所以只多两次空回调（外加每个输出帧一条字典操作）。
- 回调签名写成 `def _enter(n, f=None)` / `def _leave(n, f=None)`：不带 `prop_src` 时
  VS 只传帧号，带上时再传帧本身，两种调用方式都要接住（写死两个参数会报
  `TypeError: _enter() missing 1 required positional argument: 'f'`）。
- 帧属性（`_Duration` / `_AbsoluteTime`）经 `FrameEval` 原样透传——自检里有断言盯着，
  因为 mpv 靠 `_Duration` 判 VFR。
- 全部埋点逻辑包在 `try/except` 里：出任何异常只丢一行日志，不影响播放。
- 输出预算：前 90 帧每帧一行，之后每 30 帧一行，每 240 帧再打一行汇总（延迟分位数 /
  最大在飞帧数 / 线程构成）；延迟样本窗口 4096，长片也不涨内存。

### `mod_vpy.py` 接入

`FRAMEGEN_VPY()` 里 `values["log"]` 为真时：

1. 建 `Stages()`，依次 `mark`：`resolve` / `resize` / `load` / `prep` / `framegen` / `fps` /
   `publish` / `probe`；
2. 打一行 `config <参数…> in=WxH@fps out=WxH@fps`（`_debug_config()`，方便和跑分表对齐）；
3. `out = attach_frame_probe(core, out, tag=engine)`，再打 `stage …`。

探针套在 `_publish_framegen()` **之后**，sidecar（`<脚本>.k7f.json`）记录的仍然是未包装的
节点信息，upstream 报文的尺寸/帧率字段不受影响。

## 怎么用

开关沿用现有 `Log` 选项，没有新开关：

- 脚本里 `Log = True`；
- 或环境变量 `VDLSSG_LOG=1`（`VDLSSG_*` 优先级高于脚本字面量）；
- 或 mpv 里 `script-message-to k7f_vpypar set log 1` + `apply`（'耗时日志' 选项）。

输出走 stderr（与插件的 `[vdlssg] …` / `[FG_NS]` 同一条流），`2> 文件` 就能单独收走：

```text
[vpy-debug] config multi=3 scene=0.25 preset=a pre_h=0 gpu=None in=1920x1080@24.0 out=1920x1080@72.0
[vpy-debug] stage resolve=0.0 resize=0.0 load=4.5 prep=0.0 framegen=2011.0 fps=0.0 publish=2.5 probe=0.2 total=2018.2ms
[vpy-debug] probe[vdlssg] 帧=4 t=41.7ms 延迟=39.86ms 间隔=9.68ms 同飞=4 线程=24756
[vpy-debug] probe[vdlssg] 汇总 0.11s 帧=240 平均=1.35ms p50=1.33 p90=1.78 最大在飞=4 线程=7 (64440:41,54244:38,17780:37,…)
```

字段读法：

| 字段 | 含义 |
| --- | --- |
| `帧=` | 输出帧号 |
| `t=` | 从探针建立到该帧就绪的累计毫秒 |
| `延迟=` | 这一帧从"请求进入"到"帧就绪"的毫秒——VS 内部实际延迟 |
| `间隔=` | 距上一个就绪帧的毫秒——吞吐视角的间隔，`1000/间隔` 即瞬时 fps |
| `同飞=` | 这一帧结束时仍在飞的帧数（含它自己）；≥2 说明这一帧确实和别的帧并行 |
| `线程=` | 完成这一帧的线程号（VS 线程池 tid，不是 vs 线程序号） |
| `最大在飞=` / `线程=`（汇总行） | 进入时刻的最大并发请求数 / 参与过的线程数与前 3 名分布 |

判断方法：

- **`同飞` 长期 ≥2 且 `线程` 出现多个 tid** → VS 侧确实多帧并行，原生单组缓冲的串行结构
  做不到这一点；
- **单帧 `延迟` 明显大于插件内部 `gpu`/`wall`** → 多出来的部分是排队（等空闲引擎槽位），
  这部分正是"靠并发掩盖"的时候被别的帧占着的；
- **`间隔` 的倒数是吞吐**，与 `docs/mpv-filter-bench.md` 的 `estimated-vf-fps` 应对得上。

## 验证

自检（不依赖 vdlssg，只用内置滤镜）`work/k7-probe-test.py`：

- BlankClip 320x180 → Bilinear 到 1920x1080，`K7_PROBE_LEN` 控制片长（默认 60）；
- 逐字节比对原始节点与探针节点（n = 0 / len/3 / len-1），并断言 `_Duration` 透传；
- 4 线程并发拉帧、重复拉同一帧；
- 结果 `SELFTEST OK`；`K7_PROBE_LEN=300` 可触发 240 帧汇总行（实测
  `平均=1.35ms p50=1.33 p90=1.78 最大在飞=4 线程=7`）。

端到端（真引擎 + 真 DLL）`work/k7-probe-live.py 12`：

- `.venv` 的 vspipe R74，`media/sintel_trailer_1080p.mp4`，`VDLSSG_LOG=1`，
  `--requests 4`，输出 12 帧；
- 阶段行：`framegen=2011.0ms`（引擎/代理初始化 + 首帧预热，与之前结论一致）；
- 逐帧行：`同飞` 稳定 3–4、共出现 9 个不同 tid，插件自己的 `wall` 2.5–9.6 ms 而探针
  `延迟` 12–40 ms —— 差额就是排队等槽位的时间，直接印证了"多帧同时在飞"；
- 13 帧输出 0.07 s（193 fps），探针开销在噪声里。

未跑：4K / 6× 的长时段重复取样（单次结果见下节）。

## 代价与边界

- `FrameEval` 按帧号缓存返回的节点，长片长时间开着 `log` 内存会慢慢涨；这是排查开关，
  不是常驻功能。
- 每个输出帧多两次空回调 + 一条 `dict` 操作，实测在噪声量级，但严格跑分时建议关掉。
- 只为 vpy 宿主层（`FRAMEGEN_VPY`）服务；原生 `vf_playkit` 的计时仍靠 `PLAYKIT_TIMING=1`
  与插件 `[FG_NS]` 日志，两条线字段不通用。

## 下一步（未做）

本次埋点是为判断"要不要给 `vf_playkit` 做双缓冲流水"提供证据。证据已经指向并发是差异
来源，但改造要动 mpv 源码、重编 mpv、重钉 `mpv` 组件版本（`docs/DECISIONS.md` 第 25 条
同样的流程），等确认后再动。

## 实测复核：mpv 真机吞吐（2026-11-25）

跑法：把 fork 的 `k7sfunc` 与新式 `MEMC_DLSSG_NV.vpy` 装进 stage 包
`mpv-playkit\dist\stage\mpv-playkit-pre-2026.11.20-70b810a-windows-x64-vsdlssg`
（R82 + Python 3.15，跑完已还原），片源 `testsrc2` 1920x1080@30 共 300 帧，
`d3d11vpp=scale=<目标宽/1920>` 缩放到目标宽度后接补帧滤镜，每个组合取 600 输出帧，
计时口径与 `docs/mpv-filter-bench.md` 相同（`installer/mpv_filter_bench.lua`）。
脚本：`work/k7-vs-speed-test.py`（prepare / run / restore）与 `work/k7-bench-analyze.py`。
GPU 空闲，每组单次，组间差异约 ±5%。

| 场景 | VS 输出 fps | 原生输出 fps | 倍数 | VS 每对 | 原生每对 |
| --- | --- | --- | --- | --- | --- |
| 1080p 2× | 199.5 | 57.8 | **3.45×** | 10.0 ms | 34.6 ms |
| 1440p（2k）2× | 132.1 | 54.3 | **2.43×** | 15.1 ms | 36.8 ms |
| 4K 2× | 70.9 | 46.9 | **1.51×** | 28.2 ms | 42.6 ms |
| 1080p 6× | 255.2 | 93.6 | **2.73×** | 23.5 ms | 64.1 ms |
| 4K 6× | 78.5 / 90.9（关日志） | 90.8 | 1.00×（关日志） | 66.0 ms | 66.1 ms |

探针自身的开销：1080p 2× 199.5 → 191.9（−3.8%），4K 6× 90.9 → 78.5（−13.7%）。
所以 4K 6× 的"原生反超"是探针开销造成的假象，关掉日志后两者完全打平。

探针抓到的并发形态（mpv 侧 `--frames` 口径，所有 VS 组一致）：

| 场景 | 延迟中位 / p90 | 最大同飞 | 参与线程 | 插件 wall 中位 | 插件 gpu 中位 | 引擎 copy 中位 |
| --- | --- | --- | --- | --- | --- | --- |
| 1080p 2× | 111 / 124 ms | 24 | 24 | 9.35 ms | 2.80 ms | 0.15 ms |
| 2k 2× | 161 / 198 ms | 24 | 24 | 13.11 ms | 4.16 ms | 0.24 ms |
| 4K 2× | 301 / 360 ms | 24 | 24 | 23.77 ms | 6.95 ms | 0.55 ms |
| 4K 6× | 240 / 347 ms | 24 | 23 | 7.60 ms | 3.08 ms | 0.89 ms |

结论：

- **mpv 的 VS 滤镜同时挂 24 帧在飞、跨 24 个线程**，单帧延迟被拉长到 111–301 ms，而吞吐
  由并发度决定（1080p 2×：24 帧 / 111 ms ≈ 216 fps，实测 199.5）。原生滤镜在同一时刻
  只有一对在飞，延迟等于吞吐——这就是 3.4× 的全部来源。
- **VS 已经打得过原生**，且差距随分辨率缩小：1080p 2× 3.45× → 4K 2× 1.51× → 4K 6× 打平。
  4K 6× 打平时瓶颈在引擎/显存（`copy` 升到 0.89 ms），不在调度，双缓冲也救不回来。
- 与引擎自身的 gpu 时间比：1080p 2× 引擎每生成帧 gpu 中位 2.80 ms，VS 实际 5.0 ms/输出帧，
  只剩约 1.8× 的差距（Python 侧拷贝 + 24 线程排队）；原生 34.6 ms/对 ≈ 每生成帧 17 ms 级，
  是引擎时间的 6 倍。
- 原生路径的价值是**省 CPU/内存**（不需要 Python 线程池与逐帧平面拷贝），代价是 1080p 2×
  上白白浪费 3.4× 吞吐。要不要为此做双缓冲，仍是产品取舍，不是性能能不能的问题。

