# test\suite — vs-framegen 测试套件

pytest 套件：把仓库里散落的一次性脚本收拾成**可重复、可自动判定**的检查。
目标是不依赖人眼——mpv 的视频输出直接落盘成 PNG 序列，再按帧做数值判定。

覆盖四层：

| 层 | 检查什么 | 用例 |
| --- | --- | --- |
| 环境 | python 依赖、VS 绑定、mpv 的 vapoursynth 支持、vspipe、ffmpeg、样片、playkit stage | `test_00_env.py` |
| VapourSynth | 无头出帧、日志回传、帧属性、插件加载与报错 | `test_10_vs_headless.py` |
| mpv 输出 | `vo=image` 落盘、帧数/命名/尺寸、容器帧率、identity 无损 | `test_20_mpv_frames.py` |
| 画质判定 | 矩阵/范围标签、10bit 位深、VS 失败必须被抓到 | `test_30_color.py` |
| 时间轴 | 重复帧、黑帧、冻结画面、真片推进 | `test_40_temporal.py` |
| playkit | stage 里的 python 预设层、stage 的 mpv + identity vpy | `test_50_playkit.py` |
| 吞吐 | 60 帧 identity 链的 fps 与落盘帧数 | `test_60_throughput.py` |
| 引擎 | vdlssg FrameGen 2×：帧数/帧率翻倍、`_Duration`、经 mpv 链路 | `test_70_vdlssg.py` |

## 跑法

```powershell
# 全套（约 15 秒，含 GPU 用例；缺依赖的用例会 skip 而不是 error）
.venv\Scripts\python.exe -m pytest test\suite

# 按标记
.venv\Scripts\python.exe -m pytest test\suite -m "env or vs"     # 不启动 mpv/GPU
.venv\Scripts\python.exe -m pytest test\suite -m mpv
.venv\Scripts\python.exe -m pytest test\suite -m "not slow"

# 单条
.venv\Scripts\python.exe -m pytest test\suite\cases\test_30_color.py::test_matrix_follows_input_tag -q
```

`run.ps1` 是同一件事的包装（自动定位 `.venv`、失败后打印产物目录）：

```powershell
test\suite\run.ps1                 # 全套
test\suite\run.ps1 -m mpv          # 只跑 mpv 层
test\suite\run.ps1 -Case test_30   # 只跑匹配的文件名
```

## 标记

| marker | 含义 |
| --- | --- |
| `env` | 只查环境，不跑媒体 |
| `vs` | 用 VapourSynth（vspipe 或插件） |
| `mpv` | 启动 mpv 落盘帧 |
| `gpu` | 需要 NVIDIA GPU + DLSS 运行库（不可用时 skip） |
| `data` | 需要 `测试数据\` 里的素材 |
| `playkit` | 需要 mpv-playkit 的 stage |
| `slow` | 秒级以上的用例 |

## 产物

每次运行落在 `test\artifacts\<YYYYmmdd-HHMMSS>\<nodeid_slug>\<scenario>\`（已 gitignore）：

```
mpv.log        mpv 的 --log-file
frames\        00000001.png …（vo=image 逐帧输出）
script.vpy     该场景实际喂给 VS 的脚本
vs.log         vpy 里 core.add_log_handler 收到的日志
vs_info.json   VS/python/可执行文件/cwd 现场
props.json|PROPS 行   逐帧关键属性（_Duration*/_Matrix/_SceneChange* …）
stdout.log / stderr.log / argv.txt / meta.json   子进程现场
```

判失败时先看 `mpv.log` 里的 `[f]`/`[e]` 行与 `vs.log`；两者都在用例目录里。

## 环境变量（都可选）

| 变量 | 作用 |
| --- | --- |
| `VFI_ROOT` | 仓库根（默认按 `lib/paths.py` 往上推三层） |
| `VFI_MPV` | 指定 mpv.exe（默认 `测试环境\mpv-lazy\mpv.exe`，其次 playkit stage） |
| `VFI_VSPIPE` | 指定 vspipe.exe（默认 `.venv\Scripts\vspipe.exe`） |
| `VFI_FFMPEG` | 指定 ffmpeg.exe（默认 PATH） |
| `VFI_VIDEO` | 替换默认样片（默认 `media\sintel_trailer_1080p.mp4`） |
| `VFI_DATA` | `测试数据` 目录 |
| `VFI_PLAYKIT` | 指定 mpv-playkit 的 stage 目录（不指就从 `mpv-playkit\dist\stage` / `repos\dist\mpv-playkit-pre-*` 里挑最新） |
| `VFI_ARTIFACTS` | 产物根目录 |
| `VFI_DUMP` | `1` 时若 PATH 上有 procdump，崩溃用例自动抓 dump |

套件不写死绝对路径：所有外部依赖都走上面这张表 + 相对仓库根的推导，所以
mpv-playkit 侧（`..\mpv-playkit`）可以用 `VFI_PLAYKIT=<stage>` 复用它验证打包产物。

## 目录

```
lib/paths.py    外部依赖定位（全部带 env 覆盖）
lib/runner.py   子进程封装（超时/退出码/日志落盘/可选 procdump）
lib/mpv.py      mpv 命令行 + 日志解析（converted / fps / fatal / 落盘帧）
lib/vpy.py      生成 vpy（日志钩子 + 逐帧属性 dump）与 vspipe 调用
lib/frames.py   PNG 序列 → numpy（借 ffmpeg 解码，支持 8/16bit）
lib/metrics.py  亮度/通道均值/重复帧/黑帧/闪烁指标
lib/case.py     一条用例的目录与运行句柄（Case / Run）
lib/crash.py    dump 与崩溃现场打包
cases/          用例，文件名前缀是层级顺序
```

## 已知的 mpv / VS 行为（写用例前先读）

见 `..\AGENT.md` 的「mpv + VapourSynth 实测坑」一节——那里记的是必须绕开的坑
（脚本画面必须从 `video_in` 派生、`ModifyFrame` 在 mpv 下必崩、输出帧数不能短于输入、
VS 帧属性里的 `_Matrix` 不参与 mpv 的 VO 转换等）。
