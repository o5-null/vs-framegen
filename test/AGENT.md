# test\ — 测试脚本目录

这个目录里有两层东西，边界必须分清：

- **`suite\`** —— 现在的测试套件（pytest）。**新增或修改测试只动这里。**
- 根目录那 ~80 个 `.py` —— 历史一次性脚本（`smoke*.py`、`vd_*.py`、`hsr6x_*.py`、
  `bench*.py`、`perf*.py`、`compare_*.py`、`probe_*.py` 及各自的 `.log/.err`）。
  **不再维护、不再新增**，也不要移动或改名：`docs\*.md` 有 30 多处按
  `test\<script>.py` 引用它们（hsr6x-measure、vdlssg-multiframe/plugin/presets/
  throughput/45-engine、licensing、vdlssg-adapter、vdlssg-vs-rife422-flow 等）。
  要跑基准请用 `..\bench`，要跑回归请用 `suite\`。

## suite\ 要点

- 跑法与环境变量见 `suite\README.md`；一句话：
  `.venv\Scripts\python.exe -m pytest test\suite`（或 `test\suite\run.ps1`）。
- 依赖只有 pytest + numpy + vapoursynth（都在仓库 `.venv` 里，见根 `AGENT.md` 的
  「Python 环境与包管理」）；`requirements-test.txt` 只列测试额外需要的包。
- **断言不看图、不看人眼**：mpv 用 `--vo=image` 把每帧落成 PNG，再由
  `lib/frames.py` 借 ffmpeg 解码成 numpy，`lib/metrics.py` 算亮度/通道均值/重复帧/
  黑帧/闪烁。新用例要判定画面就用这条链，不要引入 cv2/PIL。
- 每个用例有自己的产物目录 `test\artifacts\<时间戳>\<nodeid>\`（已 gitignore），
  失败时先看里面的 `mpv.log` / `vs.log` / `script.vpy`，其次 `argv.txt`。
- 缺依赖用 `lib/util.require*()` 转成 skip；**只有真的坏了才 fail**（例如
  VS 滤镜被 mpv 静默禁用、落盘帧数不足）。GPU 用例在没卡/没运行时上要 skip
  （见 `test_70_vdlssg.py` 的 `SKIP_MARKERS`）。
- 每条用例一个新目录、不共享中间文件；不要在用例之间传 `Path`。

## mpv + VapourSynth 实测坑（改用例前务必读）

这些都是踩过的，按"看起来该这样写"写就会挂：

1. **vpy 的画面必须从 `video_in` 派生。** 纯 `core.std.BlankClip(...)` 直接输出，
   mpv-lazy 会以 `0xC0000409`（rc 3221226505）崩溃且不留日志。合成画面用
   `core.std.Expr(video_in, ["16","128","128"])` 之类把每平面钉成常数。
2. **`clips[]` 形参的函数不能写成"绑定调用 + 位置参数"**。例如
   `clip.std.Expr([clip, prev], ["x y - abs"])` 报 `invalid literal for int() with base 10`，
   `clip.std.ModifyFrame(clip, [clip], sel)` 报 `ModifyFrame: Too many unnamed arguments specified`
   ——参数会错位到下一个形参上。正确写法是 `core.std.Expr([...], [...])`，
   或关键字形式 `clip.std.ModifyFrame(clips=[...], selector=sel)`（k7 的 `SCDetect2` 就是这么写的，
   在 mpv 链路实测可用）。设常量帧属性也可以用 `core.std.SetFrameProp(clip, prop="_Matrix", intval=1)`;
   逐帧 dump 属性用 `std.FrameEval(fn, clip)`（注意第二个参数不能是空数组）。
3. **VS 输出帧数不得短于输入**：mpv 按输入时间轴请求帧，脚本更短就报
   `Invalid frame number N requested` 并禁用滤镜。冻结画面用
   `core.std.Loop(video_in[0:1], 400)` 这种"超长"写法。
4. **VS 帧属性里的 `_Matrix` / `_ColorRange` 不参与 mpv 的 VO 转换**（只改这两个
   属性的两个脚本输出逐像素相同）。要测色彩标签，用 mpv 自己的
   `--vf=format=yuv420p:colorlevels=limited:colormatrix=bt.709`；注意
   `colormatrix` 没有 `bt.2020`，写 `bt.2020-ncl` / `bt.2020-cl`。
5. **VS 失败是静默降级**：脚本报错或缺 `set_output()` 时 mpv 退出码仍是 0，
   只在日志里留下 `Script evaluation failed` / `Could not get script output node` /
   `Disabling filter`。任何画质用例都必须先断言 `MpvRun.fatal()` 为空。
6. **滤镜参数里不能出现反斜杠和盘符**：mpv 会把 `\` 当转义、把 `:` 当参数分隔符。
   脚本路径一律 `Path.as_posix()` 的**相对 cwd 路径**。
7. **`--terminal=no` 会吞掉选项解析错误**：写错滤镜参数时 rc=1 而 `mpv.log` 为空，
   排错时临时去掉这个参数直跑。
8. `vo=image` 只支持 jpg/png/webp/jxl/avif，**没有 ppm**；落盘命名是
   `%08d.png` 从 1 开始。10bit 源落 PNG 时位深是 16。
9. R74 的 `std.BlankClip` 没有 `matrix`/`primaries`/`transfer` 参数；
   `core.set_message_handler` 不存在，用 `core.add_log_handler`。
   `vs.__api_version__` 是带 `api_major` 字段的对象（`R4.2`），不是 int。
10. 引擎类用例（`test_70_vdlssg.py`）：`proxy_dir` 指**引擎包目录本身**
    （`vs-dlssg\bin\dlss45`，内含 `version.dll`）；FrameGen 输出是 full range 且
    只写 `_Duration*` 不写 `_AbsoluteTime`；RGBS 输出必须转成整数 YUV 才能过
    mpv 的 VS 桥，转换时给 `matrix_in_s`/`matrix_s`，否则
    `Resize error 3074: no path between colorspaces`。
11. **不要用 `mpv --version` 判断 VS 支持**（构建选项行不打印），用
    `mpv --no-config --vf=help` 找 `vapoursynth`。

## 调试与 dump

- `VFI_DUMP=1` 且 PATH 上有 procdump 时，子进程会被 procdump 包一层自动抓 minidump；
  没有 procdump 时用 `lib/crash.py:wer_localdump_hint()` 打印出注册 WER LocalDumps 的
  PowerShell 命令（写 `%LOCALAPPDATA%\CrashDumps`，DumpType=2 全 dump）。
- vpy 里已经挂了 `core.add_log_handler`，日志落 `vs.log`，并在 `vs_info.json` 记
  VS 版本/python/可执行文件/cwd，用来区分「跑的是 stage 里的 python 还是 .venv」。
- Windows 上 PowerShell 命令含 `;` 会被 shell 扫描器拒绝（`invalid-structure`），
  调试时把多语句拆成多次调用，或写成临时脚本再用 python 跑。

## 内存与显存采样

每条用例的产物里都带占用数据，分两条独立来源，互不替代：

- **进程内存**：`lib/memory.py` 用 psapi `GetProcessMemoryInfo` 采工作集/私有字节，
  `runner.run()` 起样，写 `mem.csv`（0.2s 一次）并把峰值塞进 `meta.json` 的 `memory`。
  uv 建出来的 `python.exe` 只是**启动器**，5MB 左右；真正的解释器是它的子进程，
  所以必须按进程树求和（`CreateToolhelp32Snapshot` 取父子关系），只监控 Popen 的 pid
  会读到恒定的 5MB。`OpenProcess` 的 `restype` 必须声明成 `c_void_p`，否则 64 位句柄被截断。
- **显存**：只有两条路，且各有硬限制
  1. OS 每进程计数器 `Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory`
     （实例名 `pid_<pid>_luid_...`）——只在进程**真的持有 GPU 分配**时列出该 pid，
     短命进程、CUDA 进程常常查不到；一次查询约 1s，所以默认只对 GPU 用例开（`VFI_MEM_GPU=1` 全开）。
  2. **VS 脚本内的 NVML 整卡采样**（默认开）：`lib/vpy.py` 的 `VRAM_DEF` 在 vpy 头部起
     后台线程，`ctypes.WinDLL("nvml.dll")` → `nvmlInit_v2` → `nvmlDeviceGetHandleByIndex_v2(0)`
     → `nvmlDeviceGetMemoryInfo`，每 0.25s 往 `vs.log` 写 `VRAM {"t_ms","used_mb","total_mb"}`。
     `vpy.vram_summary()` 压成 `baseline_mb/peak_mb/delta_mb/samples` 进 `meta.json` 的 `vram`。
- 三条实测硬结论：`nvidia-smi --query-compute-apps` 的 per-process 显存在 WDDM GeForce 上
  恒为 `[N/A]`；`nvmlDeviceGetComputeRunningProcesses_v2` 在这台机器上返回 rc=7
  （`INSUFFICIENT_SIZE`，count 有效但缓冲区不可靠）；**因此显存只能看整卡 used 的基线差**
  （FrameGen 1080p/200 帧实测 delta ≈ 370MB），绝对峰值里混着别的进程。
- 纯 `--vo=image` + 无 vpy 的场景没有 `vram` 字段（没有脚本就没有采样点）；要看显存就带一个
  identity vpy，或走 `--vo=gpu` 让 OS 计数器有机会命中。

## debug 模式：逐帧差异 / 切镜 / 重复帧

`Run.vpy(body, debug=True)` 会在脚本最后一个 `set_output()` 前插入 `debug_dump(clip)`
（`lib/vpy.py` 的 `DEBUG_DEF`），对每一帧算「与前一帧的平均绝对差」并写一行
`DEBUG {"frame","diff","scene_prev","scene_next","dup"}` 到 `vs.log`：

- `diff` 由 `core.std.Expr([clip, prev], ["x y - abs"])` 的 `PlaneStats` 均值给出（0–1；
  平坦色块的相邻帧通常是 0）。
- `scene_prev` / `scene_next` 直接取上游（例如 `k7f.SCDetect2`）写好的
  `_SceneChangePrev` / `_SceneChangeNext`；没有上游检测器时恒为 0。
- `dup` = `diff <= VFI_DEBUG_DUP_TOL`（默认 `1e-6`）；**第 0 帧永远不算重复**。
- 默认开启，`VFI_DEBUG=0` 关闭；`Run.debug()` 返回摘要
  （`samples/dups/scene_prev/scene_next/min_diff/median_diff/max_diff`），`Run.debug_rows()`
  返回原始行，mpv 侧对应 `MpvRun.debug`，两者都会落进 `meta.json` 的 `debug`。
- **同一帧会被 VS 重复求值**（vspipe 每帧 2 次、mpv 每帧 4 次），所以 `read_debug()`
  按帧号去重后再统计；自己写解析时不要按行数算帧数。
- `DEBUG_DEF` 是**直接拼接**进脚本头部的（不像 `PROPS_DEF` 会过 `.format()`），
  所以里面的花括号写单个 `{ }`，写成 `{{ }}` 会得到 `TypeError: unhashable type: 'dict'`。

## k7sfunc 用例（test_80_k7.py）

- 仓库 `.venv` 里**没有** k7sfunc，`paths.k7sfunc_src()` 按
  `VFI_K7` → `<repo>\k7sfunc\src` → playkit stage 的 `Lib\site-packages` 解析；
  用例正文用 `sys.path.insert(0, "<posix 路径>")` 引入（即 `_prelude()`）。
- 已覆盖：`k7f.__version__` / `PLAYKIT_PRESETS` / `PLAYKIT_RUN` 可用性、
  `SCDetect2` 在硬切点（帧 3→4）写 `_SceneChangeNext[3]` / `_SceneChangePrev[4]`、
  静态片不误报、`threshold` 越界立刻 `ValueError`、mpv 真链路里 SCDetect2 可用，
  以及 debug 模式同时给出切镜与重复帧数据。
- k7sfunc 里**没有**现成的重复帧检测函数，重复帧由 `debug_dump()` 的 `diff` 判定。
