# mpv-playkit-pre：自维护 mpv + 两档全量包（2026-11-23）

本次把 `mpv-playkit-pre` 从「上游 shinchiro mpv + 补丁包」改成「自维护 mpv + 两个全量档位包」，
并把 vs-framegen 的四颗帧生成引擎运行时全部并入默认档。

## 交付形态

| 档位 | 产物 | 内容 | 体积 |
| --- | --- | --- | --- |
| 默认（`vsdlssg`） | `mpv-playkit-pre-<ver>-windows-x64-vsdlssg.7z` | 完整播放器（自维护 mpv + Python + VS R82 + K7sfunc）+ 四个帧生成 DLL + DLSS-G 4.5 / XeSS-FG / VFG / FSR-FG 四套引擎运行时 | ~338 MB（stage 915 MB） |
| 可选（`vs-mlrt`） | `mpv-playkit-pre-<ver>-windows-x64-vs-mlrt.7z` | 同上的完整播放器 + `vstrt_rtx`（API4 移植）+ TensorRT-RTX 1.5 + 模型；**不含四颗帧生成引擎运行时** | ~681 MB（stage 1.65 GB） |

- 两包**互斥**，各自独立可跑，不再有「基础包 + 覆盖补丁」。
- `PACK_INFO.json` 记 `full: true`；`manifests/packs/vsdlssg.yml` 有 `default: true`，`common.default_pack()` 保证最多一个默认档。
- 四个插件 DLL（`vdlssg`/`vxessfg`/`vvfg`/`vfsr4`）由基础组件 `vs-framegen-dlls` 落在 `vs-plugins\`，任何档位都有。
- 默认档的引擎组件按 `include` 白名单从**同一个** `vs-framegen-0.2.0.zip` 里各取子树：
  `vs-plugins\dlss45\`（10 文件）、`xessfg\`（9）、`vfg\`（4，约 200 MB）、`fsr4\`（23）。
  v0.2.0 起上游把四个引擎合并进一个发行 zip，旧的分引擎资产已不存在。
- `vsaji` 组件本轮 `disable: true`（aji 未定版，引擎也不带）；启用方式 `build pack --name vsdlssg --with vsaji`，需先 pin。

## mpv 自维护构建

- 源码：`E:\code\dlssg-vfi\mpv-src`（fork `o5-null/mpv`，remote `fork`），分支 `master`，含 `vf_playkit`（帧倍增原型、EOF drain、outfmt、D3D11 直通桥、`PLAYKIT_PROXY` 预加载、适配器 `#<index>`）。
- 构建必须带 `-Dvapoursynth=enabled`：`E:\code\dlssg-vfi\build-mpv.sh`（MSYSTEM=CLANG64，`meson setup build-msys --prefix=/clang64 -Dlibmpv=true -Dd3d11=enabled -Dvulkan=enabled -Dshaderc=enabled -Dvapoursynth=enabled`）；依赖包 `mingw-w64-clang-x86_64-vapoursynth 65-8`。
- 出包：`mpv-src\scripts\package_windows.py`（objdump 递归依赖闭包、平铺 zip、固定时间戳 2020-01-01）。
- CI：`mpv-src\.github\workflows\playkit-windows.yml`（仅 windows-latest + MSYS2 CLANG64 x64；push `master` 自动构建出 artifact，push tag `playkit-*` 时编译 → 打包 → 发 release）。上游 `build.yml`（多平台矩阵）与 `fuzz.yml` 已在本 fork 删除（`cd313c6`），只留 lint/docs/comment/cleanup。
- 首个 release：`o5-null/mpv` 的 `playkit-2026.11.23`，资产 `mpv-x86_64-playkit-2026.11.23.zip` 72,519,650 B，sha256 `1f736931960a4d521f73736341a818353afa7b56923cf266daeafba55158cd67`，run 45150619125 全绿。
- mpv 已 pin 到该资产（`sources.lock.json`），`assemble` 直接用缓存出的 zip。
- `vf_playkit.c` 只在 `features['d3d-hwaccel']`（win32）分支加入 sources（`mpv-src/meson.build:1439-1444`），非 Windows 不编译，不需要额外 `#ifdef`。

### 注意：mpv zip 不含 VapourSynth 运行库

zip 只带 `mpv.exe`/`mpv.com`/DLL 闭包。`vf vapoursynth` 的 `vsscript.dll` 是运行时延迟加载，由 playkit 包里 `vapoursynth` whl 复制到包根，所以单独解压 mpv zip 只能看到 `--vf=help` 里的条目，跑 `.vpy` 必须放进包内使用。

## 本机四引擎实测

测试包：本地 `vsdlssg` 全量包解压到 `work\pkgtest\mpv-playkit-pre-2026.11.20-79f8d93-windows-x64-vsdlssg`，
源 `work\bench_analysis\ah_1080p.mkv`（HEVC 1920x816，30 fps），RTX 3080。

统一命令形态（**必须带包内配置**，否则 vsscript 找不到 Python、`~~/` 也不指向包目录）：

```powershell
cd <包根>
.\mpv.com --vo=gpu-next --gpu-api=d3d11 --hwdec=d3d11va --no-audio --frames=180 `
  --vf=vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy" `
  --term-playing-msg='OUT vf_fps=${estimated-vf-fps}' E:\code\dlssg-vfi\work\bench_analysis\ah_1080p.mkv
```

| 引擎 | 脚本 | 默认倍率 | 实测输出 |
| --- | --- | --- | --- |
| DLSS-G 4.5 | `MEMC_DLSSG_NV.vpy` | 3× | `OUT vf_fps=90`（30×3） |
| XeSS-FG | `MEMC_XESSFG_NV.vpy` | 3× | `OUT vf_fps=90` |
| NVIDIA VFG | `MEMC_VFG_NV.vpy` | 2× | `OUT vf_fps=60` |
| AMD FSR-FG | `MEMC_FSR4.vpy` | 2× | `OUT vf_fps=60` |

四颗引擎都真实在插帧，且引擎目录按 `vs-plugins\<引擎>` 被脚本的回退链命中（`MEMC_VFG_NV.vpy:103-107`、`MEMC_FSR4.vpy:106` 先试 `bin\<引擎>` 再试同级）。

注意事项：

- `--no-config` 会跳过 `k7f_vs.lua`，vsscript 报 `Python executable and library path couldn't be determined`；`~~/` 也会指向用户配置目录而不是包内 `portable_config`。
- `--vf=vapoursynth=E:/.../x.vpy` 这种绝对路径写法会被 mpv 当成滤波参数解析而报错（`buffered-frames option must be an integer`），只能用 `~~/vs/...`（带包内配置）或改成相对包目录的路径。
- `vo=null` 没有 D3D11 设备，DLSS-G 会 `CreateDlssgFeature failed (NGX 0xbad00002)`；测帧生成必须 `--vo=gpu-next --gpu-api=d3d11`。
- 容器 fps 不可用的源（如 ffv1 测试片）会触发 `AssumeFPS: invalid framerate specified`（`MEMC_DLSSG_NV.vpy:118` 取到 nan），换正常 fps 的源即可。
- `VDLSSG_LOG=1` 下曾偶发一次 `exit=-1073741819`（0xC0000005），随后同参数连跑三次均 exit 0，暂记为已知 flaky，未定位。

## 期间踩到的坑

- **CI 用 MSYS 版 meson**：`ERROR: This python3 seems to be msys/python on MSYS2 Windows, but you are in a MinGW environment`。改装 `mingw-w64-clang-x86_64-{meson,ninja}`。
- **YAML 折叠标量里插 `#`**：把同一 `run:` 后半段（含包名）整段注释掉，表现为 `meson: command not found`。注释要放在步骤外。
- **CI 的 MSYS2 根目录是 `D:\a\_temp\msys64`**，硬编码 `C:\msys64\clang64\bin\python.exe` 会找不到；Package 步骤改用 MSYS2 外壳 + `cygpath -w /clang64/bin`。
- **k7sfunc 仓库把 `__pycache__/*.pyc` 提交进了 git**，codeload tar 带上后被 verify 拦下。已在 `install_git_tree` 固定 `ignore=["__pycache__", "*.pyc"]`。
- **出 zip 必须晚于最后一次成功链接**：曾用 17:05 的旧 zip 装包，smoke 报 `Option vf: 'vapoursynth' isn't supported`，实际 17:11 的重链接已带 `HAVE_VAPOURSYNTH 1`。
- 本地 urllib 走 `HTTPS_PROXY=http://127.0.0.1:7890` 下载 GitHub release 资产会 `ssl.SSLEOFError`；改用 `gh release download`，或把资产预放进 `.work/downloads/<id>/`。
- 上游 release 资产会被替换：`vs-framegen` v0.2.0 原来的 `*-mpv-lazy.zip` 全没了，只剩合并的 `vs-framegen-0.2.0.zip`；lock 里的旧 URL 直接 404。pin 时留意资产名是否还会变。

## 本地验证结果（zip 时代，产物已被下面的 7z 取代）

- `vsdlssg`（四引擎版）：stage 915.0 MB → zip 498.1 MB，sha256 `49cc0bd4e6c39eb7ae326bb12c0f96e559299e47dca9cc2adbf220e4258f70c4`；`verify --pack` 与 `smoke` 均通过。
- `vs-mlrt`：stage 1653.2 MB → zip 1086.0 MB，sha256 `3ea92d02be528c7169af420940d94553ace342e018b64462ff5f0f6c60d36929`；校验与 smoke 通过（该包不含四颗帧生成引擎）。
- CI：`gh run 45153398904`（headSha `9dd2d60`）**success**，10:08:51Z 起、10:15:23Z 完，artifact `mpv-playkit-pre-windows-x64` 1,580,184,957 B，内含两档 zip。

## CI 产物复测（2026-11-23 18:2x）

从 run 45153398904 下载 artifact 到 `work\ci-packs`：

| 文件 | 大小 | sha256 |
| --- | --- | --- |
| `mpv-playkit-pre-2026.11.20-9dd2d60-windows-x64-vsdlssg.zip` | 498,123,271 B | `E4061BA17960DF8827811D36292A19945372B11EFA1440E6716326FEABCACA13` |
| `mpv-playkit-pre-2026.11.20-9dd2d60-windows-x64-vs-mlrt.zip` | 1,086,056,548 B | `2489184F90944EB2D188D61F81F79F54623C7D96501E668A4CA3FADE4B87FFCC` |

与本地 79f8d93 构建的 sha 不同属预期（版本串内嵌提交号不同）；解压 CI 版 vsdlssg 到
`work\pkgtest-ci\mpv-playkit-pre-2026.11.20-9dd2d60-windows-x64-vsdlssg` 后，用同一命令与源复测：

| 引擎 | CI 包实测 | 本地包实测 |
| --- | --- | --- |
| DLSS-G | `OUT vf_fps=90` | `OUT vf_fps=90` |
| XeSS-FG | `OUT vf_fps=90` | `OUT vf_fps=90` |
| NVIDIA VFG | `OUT vf_fps=60` | `OUT vf_fps=60` |
| AMD FSR-FG | `OUT vf_fps=60` | `OUT vf_fps=60` |

四者 exit=0，只有上游配置噪音（`vulkan-device`/`include`、`element_vcs.lua` 语法错），
无 vapoursynth/NGX 报错。CI 包与本地包行为一致。

## 7z 出包与复测（2026-11-23 19:00）

用户要求「我们也打 7z」，选最高压缩率，并用 winget 把 7-Zip 装到本机（7-Zip 26.06，
实际路径 `C:\software\7-Zip\7z.exe`，注册表 `HKLM/HKCU\SOFTWARE\7-Zip` 的 `Path` 都指向它）。

- 新增 `build/sevenzip.py`：`find_7z()`（`PLAYKIT_7Z` → `PATH` → 注册表 → 常见目录）、
  `create_archive(stage, target, flat)`（cwd=stage + `.` 得到不带顶层目录的条目）、
  `list_entries/list_names`（`-slt` 输出转正斜杠、目录补 `/`）、`read_member`（`x -so`）。
  参数 `-t7z -mx=9 -m0=lzma2 -md=128m -mfb=273 -ms=on -mtc=off -mtm=off -mta=off -bd -y`。
- `build/package.py` 默认出 7z（`--format zip` 保留旧的确定性 zip）；`build/verify.py`
  的列名/读成员换成两种格式通用的抽象，`check_full_pack_archive` 直接从压缩包读 `PACK_INFO.json`；
  `.github/workflows/build.yml` 校验与上传改 `*.7z` + `*.sha256`。
- 提交 `12ad2d4 feat(build): 默认用 7z 最高压缩率出包，verify/CI 同步`。
- 第一次重出包失败：`OSError: [Errno 28] No space left on device`（E: 0 GB 可用）。
  清理了历次 stage/zip 与 `tensorrt-rtx__*` 重复下载缓存，共 30.1 GB。教训：`dist/stage`
  会为每个提交留一整套 0.9–1.65 GB 目录，长时间迭代必须手动回收。

产物（本地提交 `12ad2d4`）：

| 档位 | stage | 7z | sha256 |
| --- | --- | --- | --- |
| `vsdlssg` | 915.0 MB | 337.7 MB | `b39f1ac37bd956d527f49e175b683be8f4776db9880504e85988c1de1a9a1406` |
| `vs-mlrt` | 1653.2 MB | 680.9 MB | `dda4796d1209e28190e9e490d04df96d19a8854b0f38db7b449ecf56fd697bf3` |

同一内容 zip 时代是 498.1 MB / 1086.0 MB：7z 省 32% / 37%（与旧 DECISIONS 第 13 条
「7z 只省 5MB」的判断相反，那条针对的是把已压缩的模型 zip 再套一层）。

复测（vsdlssg 的 7z 用 `7z x` 解到 `work\pkgtest-7z\...`，exit 0，`mpv.exe`/`PACK_INFO.json`
都在，`portable_config/playkit-preset.conf` 里 `idle = yes` 生效）：

| 引擎 | 900 帧实时播放 | 掉帧 |
| --- | --- | --- |
| DLSS-G | `OUT vf_fps=90` | 0 |
| XeSS-FG | `OUT vf_fps=90` | 0 |
| NVIDIA VFG | `OUT vf_fps=60` | 0 |
| AMD FSR-FG | `OUT vf_fps=60` | 0 |

四者 exit=0。`build smoke` 对 vs-mlrt 的 stage 通过（python 3.15.2 / VS R82 /
k7sfunc 1.9.0+pre.7 / `mpv v0.42.0-dev-g1e6a68304`）。

新发现的两个测试坑：

- `idle = yes` 生效后，任何“播完 N 帧就退出”的自动化调用都必须显式 `--idle=no`，
  否则 mpv 播完进入常驻，子进程挂到超时。`build/smoke.py` 的 vapoursynth 链路因此超时过，
  已给该调用加 `--idle=no`。引擎复测脚本同理。
- `NGX 0xbad00002`（`CreateDlssgFeature` 失败）不只是 `vo=null` 才会出：短时间连跑多次时，
  新 7z 包与旧 CI 包**都偶尔**复现一次，下一次同参数即成功——与 `VDLSSG_LOG=1` 的
  `0xC0000005` 同属 NGX 初始化 flaky，与包内容无关。
- `--frames=120` 太短，mpv 的 `estimated-vf-fps` 在滤镜链重建时会给 0.4–0.5 的假值；
  测速至少 `--frames=900`，或用 `--untimed` 量墙上时间（no filter 442 fps、
  DLSS-G 89 fps，量级一致）。

## 仓库状态

- `o5-null/mpv-playkit-pre`：`ad90d83`（自维护 mpv + 两档全量包）→ `addff9d`（k7sfunc 重钉 11fea6b）→ `a442aef`（剔除 `__pycache__`）→ `c3c0fcb`（文档：首选推 tag 让 CI 出包）→ `79f8d93`（mpv 钉到 playkit-2026.11.23）→ `9dd2d60`（跟上 vs-framegen v0.2.0 合并发行包），均已推。
- `o5-null/K7sfunc`：`11fea6b feat(vpy): 四个补帧脚本接入公用参数桥` 已推（`k7f_bridge.lua`/`k7f_vpypar.lua`/`k7f_bridge.py`）。
- `o5-null/mpv`：`32c60a5`（playkit 桥）、`a9215ea`（打包脚本 + CI）、`16af031`/`d7d5972`/`1e6a683`（CI 修复）、`cd313c6`（只留 Win x64 构建），tag `playkit-2026.11.23`。
- 本地未推（`mpv-playkit`）：`cf93551`（README/DECISIONS 体积与四引擎说明）、
  `8001ddb`（overlay 默认 `idle = yes`）、`12ad2d4`（默认 7z 出包）；根仓库：
  `9fb41ab`、CI 复测文档、7z 文档更新（本文与 `docs/install.md`）。
- 三个更早的提交（mpv-playkit `cf93551`、根仓库 `9fb41ab`/`de991bc`）此前已问过用户是否推送，
  仍未答复；按仓库约定不自动 push。

## CI lint 转绿（2026-11-23 20:2x）

`o5-null/mpv` 上 `playkit-windows` 已全绿，但同一仓库的 `lint` 一直是红的
（run 45151597990，cd313c6 之后仍失败）。两处根因都在 `scripts/package_windows.py`：

1. `pre-commit` 的 `check-shebang-scripts-are-executable`：文件有 shebang 但 git 里是 100644，
   报 `has a shebang but is not marked executable`。
2. `ruff`（`ruff check . TOOLS/umpv`，规则 F,E,W,N,UP,A,Q,COM，line-length 100）报 6 处：
   4× `COM812` 缺尾逗号（原 116/189/235/249 行），2× `E501` 超 100 字符
   （原 311 行 `zf.comment = f"..."` 102 字符、326 行 skipped 复数拼接 106 字符）。

修法：补尾逗号；311 行拆成括号赋值；326 行抽出 `suffix` 变量；
`git update-index --chmod=+x scripts/package_windows.py`。本地提交 `759c296`
（`ci: fix lint findings for the Windows package script`），未推。

本地验证：`python -m ruff check . TOOLS/umpv` → All checks passed；`py_compile` 通过；
`python ci/lint-commit-msg.py cd313c6...HEAD` 通过（注意 subject 前缀不允许括号，
`ci(playkit): ...` 会被 `subsystem_prefix` 判失败，必须写 `ci: ...`）。

同轮的两个历史红灯不是真故障：`build` workflow（上游多平台矩阵）已在 `cd313c6` 删除，
故 09:44/09:50 的 aarch64 失败不会重现；`mpv-playkit-pre` 10:01 的 failure 是
`vs-framegen-0.2.0-mpv-lazy.zip` 404（旧资产名），10:06 用 `build.pin` 重钉到合并发行包
`vs-framegen-0.2.0.zip` 后，10:08 的 build 已全绿。

另：`E:\code\dlssg-vfi\mpv-winbuild`（shinchiro/mpv-winbuild-cmake 的 fork，
HEAD `a54f7d8`）对应的 GitHub 仓库 `o5-null/mpv-winbuild-cmake` 此前已 404 不存在，
该本地目录已于 2026-11-23 20:30 删除（3 个本地提交 `7e36e1e`/`eccb90d`/`a54f7d8` 随之丢弃，
共 172 文件 / 375 KB）。现在只保留 `mpv-src` 的 MSYS2 CLANG64 一条构建路径：
`meson setup` 默认后端就是 ninja（仓库里 `build-msys/build.ninja` 可证），CI 也显式装
`mingw-w64-clang-x86_64-ninja`，`meson compile -C build-msys` 实际调用 ninja。

自动构建（`506d8e0`）：`playkit-windows.yml` 现在除 tag 外也监听 `push: branches: [master]`，
每次推到 fork master 都会自动编译打包并上传 artifact；release 仍只在 tag push 时创建。
`concurrency: playkit-windows-${{ github.ref }}` + `cancel-in-progress: true`，
连续推多个 commit 只保留最后一次构建。

## libmpv 与依赖裁剪（2026-11-23 20:4x）

- 打包脚本此前只把 `mpv.exe` / `mpv.com` 当种子，`libmpv-2.dll` 一直没进 zip。
  已在 `scripts/package_windows.py` 的种子里加上 `libmpv-2.dll`（缺失时报错提示
  `rebuild with -Dlibmpv=true`），本地提交 `7bb4856`。
- 本地实测（`--build-dir build-msys --msys-bin C:\msys64\clang64\bin`）：
  126 个文件，raw 192.4 MiB，zip 70.8 MiB，`libmpv-2.dll` 存在。
- ggml/whisper 为什么删不掉：MSYS2 的 ffmpeg 9.1 用 `--enable-whisper` 编的，
  `avfilter-12.dll` 的**静态导入表**里有 `ggml-1.dll`、`libwhisper-1.dll`，
  打包阶段直接删这两个 DLL 会让 mpv 加载失败。`mpv.exe` 还直接导入
  `libcaca-0.dll` / `libjpeg-8.dll` / `libbluray-4.dll` / `liblcms2-2.dll`。
  要真正去掉只能自建 ffmpeg（configure 去掉 `--enable-whisper`）。本轮选择了
  「不动 ffmpeg」，`ggml-1.dll` / `ggml-base-1.dll` / `libwhisper-1.dll`（约 1.3 MB）
  继续随包发布。
- 如需瘦身：MSYS2 的 ffmpeg PKGBUILD 在 `MINGW-packages/mingw-w64-ffmpeg/PKGBUILD`，
  `--enable-whisper` 在 `CARCH != i686` 分支里；照抄它的 configure 参数（只编 shared）、
  去掉 whisper，再配合 mpv 的 `-Dlibbluray=disabled -Dcaca=disabled -Djpeg=disabled
  -Dlcms2=disabled`、关掉 x265/aom-enc/svt-av1/rav1e/xvid/lame 等编码器，可把 zip 压到
  约 50 MB。

## 上游自动同步（`d970efe`）

新增 `.github/workflows/sync-upstream.yml`：每天 UTC 05:00（北京时间 13:00）检查
`mpv-player/mpv` 的 master，有新提交就把 fork 的 14 个补丁 rebase 到新上游之上，
`git push --force-with-lease origin master` 推回，再 `gh workflow run playkit-windows.yml`
显式触发构建（GITHUB_TOKEN 推的提交不会自动触发 workflow）。

- 用 rebase 不用 merge：mpv 的 commit-msg lint 禁止 merge commit。
- 预期冲突只有一处：本 fork 删掉的 `.github/workflows/build.yml` 上游还在改。
  脚本对 `build.yml` / `fuzz.yml` 的 `modify/delete` 冲突自动 `git rm` 保留删除；
  其它文件冲突直接让 job 失败，留给人处理。
- 权限：`contents: write`（推回）+ `actions: write`（dispatch）。
- 本地用 Git Bash 跑过同一段脚本：以当前上游 `1eb941f`（`wayland: don't use old
  viewport after scale changes`）为目标，14 个补丁里只有 `cd313c6` 的 build.yml
  modify/delete 冲突被自动解掉，rebase 全部成功。本地 master 已同步到该结果。

## 推送后 CI 验证（2026-11-23 20:41）

`git push --force-with-lease fork master`（`cd313c6...d970efe`）后三个 workflow 均按预期：

| run | 结果 | 关键日志 |
| --- | --- | --- |
| `lint` 45175936764 | success 32s | editorconfig / pre-commit / lua / ruff / mypy / swift 全 ✓ |
| `playkit-windows` 45175936891 | success 3m41s | seeds `mpv.exe` 16,015,872 B、`mpv.com` 25,600 B、`libmpv-2.dll` 15,998,464 B；141 文件，raw 199.9 MiB，zip 78,308,027 B（74.7 MiB） |
| `sync-upstream` 45176605828（手动 dispatch） | success 21s | 检查步骤通过；上游无新提交，rebase / 推回两步跳过 |

- CI 包比本地多 15 个 DLL，是 `msys2 update:true` 拉到的新版依赖，属正常。
- `Create release` 按 tag-only 条件跳过。
- 唯一告警：Node.js 20 弃用（`actions/checkout@v4`、`actions/upload-artifact@v4`），不影响构建。
- `sync-upstream` 的 rebase / force-push 分支在 CI 上尚未真实命中（上游未动），
  但同一段脚本已在本地以 `1eb941f` 为上游完整跑通。

## 待办

- 想彻底去掉 `ggml-1.dll` / `ggml-base-1.dll` / `libwhisper-1.dll` 只能自建 ffmpeg
  （configure 里去掉 `--enable-whisper`，其余参数照抄 MSYS2 PKGBUILD）；本轮按选择
  保持 MSYS2 ffmpeg 不动。
- `vsaji` 定版后：推 GitHub、发 `dist/aji` release（zip 根 `aji/**`，由 `vsaji/scripts/make-release.py` 生成）、`build.pin --pack vsdlssg --id vsaji`、去 `disable`。
- `VDLSSG_LOG=1` 偶发访问违例继续观察（NGX 初始化、proxy 适配器、引擎包选择均已确认正常）。
- 包内上游配置噪音未修（`vulkan-device`/`include` 报错、`xxxxx.lua` 缺失、`uosc_addones/element_vcs.lua:278` 语法错、`script-opts/k7f_playkit.conf` 的 key 带尾随空格、`shaders/hooks_0*.glsl` 缺失）。
