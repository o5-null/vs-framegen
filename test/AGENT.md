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
2. **`std.ModifyFrame` 在 mpv 下必挂**（rc=2，日志只有 `initiate EOF`，0 帧）；
   vspipe 下正常。设帧属性改用 `core.std.SetFrameProp(clip, prop="_Matrix", intval=1)`。
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
