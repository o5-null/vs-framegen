这是一个dlss帧生成尝试迁移到视频的项目仓库（vs-framegen）。
.\测试数据 是用来测试实际生成速度和质量的
.\参考插件 不应该进行更改，只是用于参考
.\bench 是八引擎帧生成基准（脚本 + 结果快照），原始数据在 .\测试数据\engines
源码以 .\vs-dlssg\src 为准（一个引擎一个 DLL，build.bat 一次编四个）；.\archive 与 .\ref 只读
构建产物、引擎包、SDK、第三方装包与媒体素材不进 git（见 .gitignore）；提交前跑 git status 确认没有大文件

## Release 规则

- 安装与用法只维护 `docs\install.md`，release 正文**只写这一版改了什么 + 附件清单与 sha256**，
  安装说明写一行链接过去：`https://github.com/o5-null/vs-framegen/blob/main/docs/install.md`。
  以后所有 release 都链这一份，不逐版复述目录布局、参数表、推荐装法。
- `docs\install.md` 的推荐顺序固定为：优先 **mpv-playkit-pre 懒人包**（含 `-pack-framegen` 补丁包），
  自行搭建建议用 **K7 包**（K7sfunc 的 mpv 覆盖层）+ 本仓库 release 的 `vs-plugins` 附件。
- 正文里不要再列"四引擎插件组"表、不要写版本主线总结、不要写 OSD 参数桥细节（后者归 K7sfunc）。
- 构建产物（DLL、引擎包）不入库，只作为 release 附件分发。
- 附件只有一个：`vs-framegen-<版本>.zip`，含四个插件与四个引擎包；不加 `-mpv-lazy` 之类后缀，
  也不再拆引擎单包。
- 附件布局：引擎包与 DLL **同级**（`vs-plugins\dlss45\`、`vs-plugins\xessfg\`）；插件默认就按
  `<DLL 目录>\<引擎>` 找，这是设计意图。0.1.0 资产里的 `vs-plugins\bin\<引擎>` 是多套的一层，
  K7 脚本会兼容，但不要当成本仓库的发行布局，也不要据此改打包脚本。

## 邻近仓库（在仓库根下的同级目录，各自独立 .git，已被 .gitignore 忽略）

- k7sfunc：K7sfunc 的 fork（o5-null/K7sfunc），PLAYKIT 预设层与 RIFE_AJI 算子在这里；改动 push 后由 mpv-playkit 按 commit 钉版本
- mpv-playkit：Windows x64 便携 mpv 预览包的构建链（o5-null/mpv-playkit-pre），manifest + sources.lock
- vsaji：AnimeJaNai (libaji) 的 VapourSynth 插件与构建脚本，产物 dist\aji 供 k7sfunc 的 RIFE_AJI 使用
- mpv-src：mpv 源码与 msys 构建树（只读参考，`run-mpv-test.sh` 等脚本指向它的 build-msys）
- ngxsdk：NVIDIA DLSS SDK 的克隆（`vs-dlssg\src\build.bat` 的默认 NGX_SDK）
- k7sfunc-wiki：wiki 源；k7sfunc-wiki-pages、vs-framegen-wiki-pages 是静态页输出（不是仓库）

跨仓库的改动要分别提交到各自的仓库；本仓库的 .gitignore 只负责把它们挡在外面，不会被一起提交。

### 统一产物与共享缓存

`repos\` 只装构建产出的交付归档与下载缓存（子仓库本身留在根下各自目录），多个子仓库共用：

- `repos\dist\`：交付归档（本仓库的 `vs-framegen-<版本>.zip`、mpv-playkit 的基础包与两个档位 7z）
- `repos\.cache\downloads\`：mpv-playkit 的下载缓存（按 sha256 定位，一份资产只存一次）

mpv-playkit 的 `build/common.py` 会自动识别（同级存在 `repos\dist` 或 `repos\.cache`），
CI 上没有这个目录时退回仓库内 `.work\downloads` / `dist\out`。手动指定共享根用
`PLAYKIT_SHARED_ROOT`。构建完的归档请放进 `repos\dist\`，不要留在各仓库的 `dist\out`。

## Python 环境与包管理（uv）

本仓库的虚拟环境与软件包管理一律用 **uv**（`uv venv` / `uv pip install --python …` / `uv run`），
不要用 `python -m venv`、裸 `pip`、conda 或借用其它项目的 venv；`uv` 已在 PATH（`C:\software\Python312\Scripts\uv.EXE`）。
新建的环境目录统一叫 `.venv`（或实验用 `.venv*`），都在 .gitignore 里。

## 测试环境（Python / VapourSynth）

脚本用本仓库自己的 uv 虚拟环境 `.venv`（已被 .gitignore 挡住），不要借用其它项目的 venv：

```
uv venv --python 3.12 .venv
uv pip install --python .venv\Scripts\python.exe vapoursynth==74 numpy vsrepo
.venv\Scripts\vsrepo.exe install ffms2 imwri
```

- vapoursynth 钉在 **74**：`测试环境\mpv-lazy\vs-plugins\akarin.dll` 是 API 3 插件，R82 起拒绝加载
  （`uses API 3, which is no longer supported`），而 k7sfunc 的 RIFE_NV / RIFE_AJI 都依赖 akarin。
- ffms2 / imwri 由 vsrepo 装进 `.venv\Lib\site-packages\vapoursynth\plugins\vsrepo`（VS 自动扫描该子目录）。
- 其余插件不进 venv，按路径 LoadPlugin：akarin/vstrt（+ vsmlrt-cuda 运行时）在 `测试环境\mpv-lazy\vs-plugins`；
  aji 运行时是 `work\aji-models`（`vsaji\dist\aji` 的拷贝，用 `AJI_DIR` 指过去）。
- 测速脚本在 `work\aji-rife-test`（不进 git）：`one.py` 一次跑一种引擎+模型，`mk_src.py` 把
  `测试数据\1080p` 的 PNG 转成无损 FFV1（`--source ffv1`），避免把 PNG 解码算进引擎耗时。
- aji 的分阶段计时桩默认关闭，`AJI_PROFILE=1`（`AJI_PROFILE_EVERY=N`，默认 10 帧）打开，
  stderr 出 `[aji-profile/plugin]`（上传/交织/回传）与 `[aji-profile/trt]`（setup…infer…crop）两行；
  桩在 `vsaji\src\aji_host.cpp`、`aji_frames.cpp`、`.deps/engine/src/aji_trt.cpp`，改完要重编 engine + 插件。

## 测试套件（test\suite）

`test\suite` 是仓库唯一的自动化测试入口（pytest）：

```
.venv\Scripts\python.exe -m pytest test\suite          # 全套，约 30 秒
.venv\Scripts\python.exe -m pytest test\suite -m "not slow"
```

- 覆盖：环境自检 / VS 无头出帧与插件 / mpv 落盘与转换 / 色彩矩阵与位深 / 真实显示路径截图 /
  重复帧与黑帧 / playkit stage / 吞吐 / vdlssg FrameGen / k7sfunc（`SCDetect2` 切镜）。
  判定全部是数值（PNG 序列 → numpy），不靠人眼。
- 需要「真实显示路径」的帧（libplacebo 色彩管理/缩放/抖动之后）时用
  `lib/display.py:capture_display()`：mpv JSON IPC 驱动 `frame-step` +
  `screenshot-to-file`，落 RGBA 16bit PNG，与 `--vo=image` 软件路径做数值对比。
- `Run.vpy(..., debug=True)` 会在 vpy 里逐帧算帧间差并写 `DEBUG {...}` 行（切镜标志 + 重复帧），
  摘要进 `meta.json` 的 `debug`；`VFI_DEBUG=0` 关闭，`VFI_DEBUG_DUP_TOL` 调重复判定阈值。
- 每个子进程都带占用采样：`mem.csv` 是进程树内存（0.2s）与可选每进程显存，`meta.json` 的
  `memory`/`vram` 是峰值摘要；显存以 VS 脚本内的 NVML 整卡采样为准（`delta_mb` 才是本次抬升，
  `nvidia-smi` 的 per-process 在 WDDM 上恒 `[N/A]`）。细节见 `test\AGENT.md`。
- `test\` 根目录的历史一次性脚本只留档：`docs\*.md` 按 `test\<script>.py` 引用它们，
  不要移动或改名，也不要再往里加东西；规则写在 `test\AGENT.md`。
- 详细跑法、产物布局、环境变量（`VFI_MPV`/`VFI_PLAYKIT`/`VFI_DUMP` …）见
  `test\suite\README.md`；写用例前先读 `test\AGENT.md` 的「mpv + VapourSynth 实测坑」。

## mpv + VapourSynth 实测坑（测试/调试通用）

1. vpy 画面必须由 `video_in` 派生：纯 `BlankClip` 直接输出会让 mpv-lazy 崩在 `0xC0000409`。
2. `clips[]` 形参的函数不能"绑定调用 + 位置参数"（`clip.std.Expr([...], [...])` /
   `clip.std.ModifyFrame(clip, [...], sel)` 会参数错位报错）；用 `core.std.Expr(...)` 或
   `clip.std.ModifyFrame(clips=[...], selector=sel)`。逐帧 dump 用 `std.FrameEval(fn, clip)`
   （第二个参数不能是空数组）。
3. VS 输出帧数不能短于输入（mpv 按输入时间轴请求帧）；冻结画面用 `std.Loop(video_in[0:1], 400)`。
4. VS 的 `_Matrix`/`_ColorRange` 属性不参与 mpv 的 VO 转换，色彩标签用 `--vf=format=...`
   （`colormatrix` 没有 `bt.2020`，用 `bt.2020-ncl`）。
5. VS 脚本失败是**静默降级**（mpv 退出码仍 0，只在日志留 `Script evaluation failed` /
   `Could not get script output node` / `Disabling filter`）——调试先 grep 这三句。
6. `--vf=vapoursynth=` 的参数里不能出现反斜杠或盘符（`:` 是参数分隔符）。
7. `mpv --version` 不打印构建选项，判断 VS 支持用 `mpv --no-config --vf=help`。
8. 想把「真正显示的画面」抓成图：走 JSON IPC（`--input-ipc-server=\\.\pipe\<名>`）
   驱动 `frame-step` + `screenshot-to-file`，需 `--screenshot-sw=no`；截图是 RGBA 16bit PNG，
   且**不要**加 `--window-minimized=yes`（与 gpu-next 同用截不到帧）。
   `${=vf-metadata}`（`--term-playing-msg`）和 IPC 的 `vf-metadata/vapoursynth.00` 都拿不到
   帧属性，帧属性只能读 vs.log 的 `DEBUG`/`PROPS` 行。
