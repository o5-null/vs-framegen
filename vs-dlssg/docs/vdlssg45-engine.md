# vdlssg 的 DLSS 4.5 引擎包（`vs-dlssg\bin\dlss45`）

把 SmoothMyVideo（SMV）随包的 DLSS 4.5 引擎提取出来，做成 `vdlssg` 可直接加载的自包含目录。
结论先说：**"4.5 引擎"在这个仓库里的实际增量只有一个 ini 开关——`MaxGeneratedFrames=5`，
即 6× 多帧；运行库本体与 stock 代理逐字节相同。** vdlssg 原本就是同一引擎的 VapourSynth
封装，所以这次不是换引擎，而是把上限从 4× 提到 6×，并让引擎包独立成目录。

## 1. 两份运行库的关系

| 来源 | 文件 | SHA256 | 尺寸 |
|---|---|---|---|
| SMV `engine\dlssg\` | `nvngx_dlssg.dll` | `8D59D49B36410DD6B62FEF2503A5871D347415BD85D943FA0848563A95DD50C7` | 7,506,544 |
| SMV `ngxsdk\lib\Windows_x86_64\rel\` | 同上（逐字节相同） | 同上 | 同上 |
| 解锁 payload = `dist\dlssg_sm86\runtime\` | `nvngx_dlssg.dll` | `CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72` | 7,506,544 |

三者版本都是 310.9.16。前两份是 SMV 自带（SDK rel 构建），第三份是 `dlssg_for_sm86` 解锁包
的 payload，也就是 vdlssg 一直以来的运行库。`dlss45` 用第三份，理由只有一个：它已经在本仓库
的 3080 上端到端跑通（吞吐/画质数据都是基于它测的）。SMV 自带那份没有在 vdlssg 路径上验证过，
换它属于无收益的风险。

## 2. ini 才是 4.5 的差

两份 `dlssg_sm86.ini` 去掉时间戳后**只差一行**：

```ini
# dist\smv-dlssg-unlock\payload\engine\dlssg\dlssg_sm86.ini
MaxGeneratedFrames=5

# dist\dlssg_sm86.ini / work\dlssg_sm86.ini
MaxGeneratedFrames=3
```

ini 注释写得很直白：`3` = 4×（出厂），`5` = 6×，是该版本运行库支持的上限，超过 5 不启用；
其余关键项 `Directory=dlssg_sm86\logs`、`Level=2` 两边一致。

对应到插件：`multi=6` → `multiFrameCount=5`、`multiFrameIndex=1..5`。

## 3. 为什么不用 SMV 的进程外路线

SMV 的做法是 `dlssg2f.exe --server W H --gen N`，stdin 收 RGBA8（或 R10G10B10A2），stdout
吐中间帧，宿主还要打三处补丁（先载代理、扩 `PathListInfo`、放开 40/50 显卡正则）。
vdlssg 走进程内 NGX：

- 颜色是 RGBS 浮点，不经过 8/10bit 量化；
- 少一次 stdin/stdout 拷贝与握手，1080p 单张约 1.36 ms（`docs\vdlssg-throughput.md`）；
- 没有额外 exe，也没有 SMV 的宿主补丁面。

代价是 vdlssg 自己维护会话/切镜/抑制语义（`docs\vdlssg-plugin.md`），这些已经在位。
`dlssg2f.exe` 仍留在 `dlss45` 里，只当冒烟工具用。

## 4. 插件侧改动

| 位置 | 改动 |
|---|---|
| `vs-dlssg\src\frame_gen.cpp:332` | `multi` 上限 4 → 6，报错文案改为 `multi must be between 2 and 6` |
| `vs-dlssg\src\frame_gen.cpp:298` | `isProxyDir` / 缺省搜索：`<插件目录>\dlss45\version.dll` 存在则用它，否则插件目录 |
| `vs-dlssg\src\ngx_session.cpp` | 无改动。上限仍以运行时 `DLSSG.MultiFrameCountMax` 为准，超了报错 |

`multi=6` 与 `multi=2` 走同一条 `generate()`；唯一的运行时差别是 `multiFrameCount` 参数。

## 5. 验证

引擎包（不经过 VS，直接跑 SMV 宿主）：

```bat
cd /d E:\code\dlssg-vfi
E:\code\VFI\.venv\Scripts\python.exe vs-dlssg\test\smoke.py 640 360 5 6
```

实测输出：handshake `DLSSG READY gen=5 max=5 wq=1 ngx=1`，`25 interpolated frames`
（`(6-1)*5`），质心误差最大 2 px（合成盒子每步 10 px，取整导致 ±2）。

插件侧（需要先构建）：

```bat
cmd /c E:\code\dlssg-vfi\vs-dlssg\src\build.bat
E:\code\VFI\.venv\Scripts\vspipe.exe -p E:\code\dlssg-vfi\test\mf6.vpy NUL
```

`test\mf6.vpy` 用 `multi=6` + `proxy_dir=vs-dlssg\bin\dlss45` 跑 1080p Sintel，`log=1` 会按帧打印
耗时；输出帧数应为源帧数 ×6。

## 6. 边界与坑

- **stock 包不能跑 6×**：ini 是 3，`Init` 后读到的 `MultiFrameCountMax` 就是 3，插件会直接报
  `multiFrameCount 5 exceeds DLSSG.MultiFrameCountMax 3`。想 6× 就必须把 `proxy_dir` 指到
  `dlss45`（或把它放到插件旁作为缺省）。
- **6× 的质量/吞吐还没有专门测量**：`test\parity3.py`、`test\bench_multi.py` 目前只覆盖到 4×。
  按 4× 已是 2× 的 1.9 倍 MAE 的趋势，6× 单帧质量预计继续下降；SMV 侧同引擎的 1080p 6×
  约 195 fps 是它自己 8bit 宿主的数据，不能直接套到浮点路径。
- **`dlssg_sm86\logs\` 必须存在**，否则代理零日志静默失败（`smoke.py` 之前踩过）。
- 代理必须在 NGX `Init` 之前加载；插件在 `loadProxy()` 里做，`proxy_dir` 换目录时不要绕开它。
- **模型预设**只由 `dlssg_sm86.ini` 的 `[DLSSG] Preset` 决定，代理装载时一次性钉死；插件用
  `preset="a"/"c"` 在装载前改写这一行，进程内不可再换（详见 `docs\vdlssg-plugin.md`）。
- 4K 6× 显存吃紧：掉帧或 NGX 拒绝时降 `multi`（4× 用 stock 包即可）。
- 目录可以整包拷走；只要 `version.dll` 与 `dlssg_sm86\` 同级，`proxy_dir` 指过去就能用。

## 7. 相关

- `docs\smv-dlssg-unlock.md` — SMV 宿主三处补丁与 asar 门限（4.5 能不能在 30 系跑的取证）
- `docs\vdlssg-multiframe.md` — 多帧语义、抑制标志、2–4× 质量与吞吐表
- `vs-dlssg\bin\dlss45\README.md` — 包内文件清单与用法
